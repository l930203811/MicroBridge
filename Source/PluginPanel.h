#pragma once

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "I18n.h"
#include "PluginRack.h"
#include "Theme.h"

//==============================================================================
// 插件库面板（浮层）：
// · 左列：扫描目录管理 —— 默认目录（只读）+ 自定义目录（可勾选多个批量移除），
//   每行左侧多选框，勾选按路径保存（目录增删导致行号变化也不会错位）。
// · 右列：已扫描到的 VST3 插件清单（按所在文件夹分组，只读；含厂商/类型/通道数）。
//==============================================================================
class PluginPanel : public juce::Component,
                    private juce::Timer,
                    private juce::ListBoxModel
{
public:
    std::function<void()> onClose;
    std::function<void (const juce::String&)> onMessage;
    std::function<void()> onListChanged;

    explicit PluginPanel (mb::PluginCatalog& catalog) : catalog_ (catalog)
    {
        addAndMakeVisible (dirList_);
        dirList_.setModel (&dirModel_);
        dirList_.setRowHeight (24);                 // 与右列行高一致
        dirModel_.onToggle = [this]
        {
            dirList_.repaint();
            refreshRemoveButtonText();
        };
        dirList_.setColour (juce::ListBox::backgroundColourId, mb::ui::sectionBg);
        dirList_.setColour (juce::ListBox::outlineColourId, mb::ui::border);
        dirList_.setOutlineThickness (1);
        dirList_.selectRow (0);

        addAndMakeVisible (list_);
        list_.setModel (this);
        list_.setRowHeight (24);
        list_.setColour (juce::ListBox::backgroundColourId, mb::ui::sectionBg);
        list_.setColour (juce::ListBox::outlineColourId, mb::ui::border);
        list_.setOutlineThickness (1);

        // 注意：必须用 const char* 直通 mb::tr，经 juce::String 中转会按 Latin-1 逐字节构造导致乱码
        auto flatButton = [] (juce::TextButton& b, const char* zh, const char* en,
                              juce::Colour bg)
        {
            b.setButtonText (mb::tr (zh, en));
            b.setColour (juce::TextButton::buttonColourId, bg);
            b.setColour (juce::TextButton::textColourOffId,
                         bg == mb::ui::accent[0] ? juce::Colours::white : mb::ui::text);
        };

        addAndMakeVisible (scanBtn_);
        flatButton (scanBtn_, "重新扫描", "Rescan", mb::ui::accent[0]);
        scanBtn_.onClick = [this]
        {
            catalog_.startScan();
            startTimerHz (10);
            if (onMessage) onMessage (mb::tr ("开始扫描 VST3 插件…", "Scanning VST3 plugins…"));
        };

        addAndMakeVisible (addDirBtn_);
        flatButton (addDirBtn_, "添加目录", "Add folder…", mb::ui::sectionBg);
        addDirBtn_.onClick = [this]
        {
            chooser_ = std::make_unique<juce::FileChooser> (
                mb::tr ("选择 VST3 目录", "Choose VST3 folder"));
            chooser_->launchAsync (juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::FileChooser& fc)
                {
                    const auto dir = fc.getResult();
                    if (! dir.isDirectory()) return;
                    catalog_.addSearchDirectory (dir);
                    catalog_.startScan();
                    startTimerHz (10);
                    if (onMessage)
                        onMessage (mb::tr ("已添加目录并开始扫描：", "Folder added, scanning: ")
                                   + dir.getFullPathName());
                });
        };

        addAndMakeVisible (openBtn_);
        flatButton (openBtn_, "打开默认目录", "Open default", mb::ui::sectionBg);
        openBtn_.onClick = [this]
        {
            const auto dirs = mb::PluginCatalog::defaultDirectories();
            if (dirs.size() > 0) juce::File (dirs[0]).startAsProcess();
        };

        addAndMakeVisible (removeDirBtn_);
        flatButton (removeDirBtn_, "移除目录", "Remove", mb::ui::sectionBg);
        removeDirBtn_.onClick = [this]
        {
            auto paths = dirModel_.checkedPaths();           // 勾选优先（多选批量移除）
            if (paths.empty())                               // 未勾选时兼容旧行为：移除当前选中的自定义目录
            {
                const auto row = dirList_.getSelectedRow();
                if (row < 0 || ! dirModel_.isCustom (row))
                {
                    if (onMessage) onMessage (mb::tr ("请先勾选要移除的自定义目录",
                                                     "Tick custom folders to remove first"));
                    return;
                }
                paths.push_back (dirModel_.pathOf (row));
            }
            for (const auto& p : paths) catalog_.removeSearchDirectory (p);
            dirModel_.clearChecked();
            dirList_.updateContent();
            dirList_.selectRow (0, true);
            refreshRemoveButtonText();
            catalog_.startScan();                            // 扫描结果同步剔除已移除目录
            startTimerHz (10);
            if (onMessage) onMessage (mb::tr ("已移除 ", "Removed ")
                                      + juce::String (paths.size())
                                      + mb::tr (" 个扫描目录，正在重新扫描", " folder(s), rescanning"));
        };

        addAndMakeVisible (selectAllBtn_);
        flatButton (selectAllBtn_, "全选", "Select all", mb::ui::sectionBg);
        selectAllBtn_.onClick = [this]
        {
            dirModel_.toggleSelectAll();
            dirList_.repaint();
            refreshRemoveButtonText();
        };

        addAndMakeVisible (scanOnlyBtn_);
        flatButton (scanOnlyBtn_, "只扫描此目录", "Scan only", mb::ui::sectionBg);
        scanOnlyBtn_.onClick = [this]
        {
            const auto row = dirList_.getSelectedRow();
            if (row < 0) return;
            const auto path = dirModel_.pathOf (row);
            catalog_.startScan (path);
            startTimerHz (10);
            if (onMessage) onMessage (mb::tr ("只扫描目录：", "Scanning folder only: ") + path);
        };

        addAndMakeVisible (closeBtn_);
        flatButton (closeBtn_, "关闭", "Close", mb::ui::card);
        closeBtn_.onClick = [this] { if (onClose) onClose(); };

        if (catalog_.isScanning()) startTimerHz (10);
        rebuildRows();
    }

    ~PluginPanel() override { stopTimer(); }

    void refresh()
    {
        rebuildRows();
        list_.updateContent();
        dirList_.updateContent();
        dirModel_.pruneChecked();                 // 目录已在别处移除时清掉对应勾选
        refreshRemoveButtonText();
        repaint();
    }

    /** 内嵌模式（设置页「插件库」Tab）：不画遮罩，卡片铺满组件区域，隐藏关闭按钮。 */
    void setPlainMode (bool p) { plainMode_ = p; closeBtn_.setVisible (! p); resized(); repaint(); }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto card = plainMode_ ? getLocalBounds() : cardBounds();
        if (plainMode_)
        {
            g.setColour (mb::ui::sectionBg);
            g.fillRoundedRectangle (card.toFloat(), 10.0f);
            g.setColour (mb::ui::border);
            g.drawRoundedRectangle (card.toFloat(), 10.0f, 1.0f);
        }
        else
        {
            g.fillAll (juce::Colours::black.withAlpha (0.35f));
            g.setColour (mb::ui::card);
            g.fillRoundedRectangle (card.toFloat(), 10.0f);
            g.setColour (mb::ui::border);
            g.drawRoundedRectangle (card.toFloat(), 10.0f, 1.2f);
        }

        g.setFont (mb::ui::font (14.0f, true));
        g.setColour (mb::ui::text);
        g.drawText (mb::tr ("插件库 · VST3", "Plugin library · VST3"),
                    card.getX() + 18, card.getY() + 10, 400, 30,
                    juce::Justification::centredLeft, false);

        // 状态行 / 进度
        g.setFont (mb::ui::font (9.0f));
        juce::String status;
        if (catalog_.isScanning())
            status = mb::tr ("正在扫描：", "Scanning: ") + catalog_.currentName();
        else
            status = mb::tr ("共 ", "Found ") + juce::String (catalog_.count())
                   + mb::tr (" 个插件 · 默认目录 ",
                             " plugins · default folder ")
                   + defaultDir().getFullPathName();
        g.setColour (mb::ui::subText);
        g.drawText (status, card.getX() + 18, card.getY() + 40,
                    card.getWidth() - 36, 16, juce::Justification::centredLeft, true);

        if (catalog_.isScanning())
        {
            const auto bar = juce::Rectangle<float> (static_cast<float> (card.getX() + 18),
                                                     static_cast<float> (card.getY() + 60),
                                                     static_cast<float> (card.getWidth() - 36), 4.0f);
            g.setColour (mb::ui::track);
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (mb::ui::accent[0]);
            g.fillRoundedRectangle (bar.withWidth (static_cast<float> (bar.getWidth()
                                        * juce::jlimit (0.02, 1.0, catalog_.progress()))), 2.0f);
        }

        // 右列用途说明：与左下角操作按钮同一行，两栏底部留白保持整齐
        int colW = 250, rightW = 210;
        columnWidths (card, colW, rightW);
        g.setFont (mb::ui::font (9.0f));
        g.setColour (mb::ui::subText);
        g.drawText (mb::tr ("右：已扫描到的 VST3 插件（按目录分组，只读）",
                            "Right: scanned VST3 plugins, grouped by folder (read-only)"),
                    card.getX() + 18 + colW + 12, card.getBottom() - 70, rightW, 26,
                    juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        const auto card = plainMode_ ? getLocalBounds() : cardBounds();
        const int bx = card.getX() + 18;
        const int by = card.getY() + (catalog_.isScanning() ? 72 : 64);
        const int bottom = card.getBottom() - 40;
        int colW = 250, rightW = 210;
        columnWidths (card, colW, rightW);

        scanBtn_.setBounds  (bx, by, 104, 26);
        addDirBtn_.setBounds (bx + 112, by, 112, 26);
        openBtn_.setBounds  (bx + 232, by, 126, 26);
        closeBtn_.setBounds (card.getRight() - 108, card.getY() + 12, 90, 28);

        // 两栏等高：底部统一留出 34px 给操作按钮 / 说明文字
        const int listTop = by + 26;
        const int listH = juce::jmax (40, bottom - listTop - 34);
        dirList_.setBounds (bx, listTop, colW, listH);
        list_   .setBounds (bx + colW + 12, listTop, rightW, listH);

        const int bw = juce::jmax (72, (colW - 16) / 3);
        int px = bx;
        removeDirBtn_.setBounds (px, bottom - 30, bw, 26);   px += bw + 8;
        selectAllBtn_.setBounds (px, bottom - 30, bw, 26);   px += bw + 8;
        scanOnlyBtn_.setBounds  (px, bottom - 30, juce::jmax (72, bx + colW - px), 26);
    }

    //==========================================================================
    // 插件列表：文件夹分组表头 + 插件行
    int getNumRows() override { return static_cast<int> (rows_.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= static_cast<int> (rows_.size())) return;
        const auto& r = rows_[static_cast<size_t> (row)];

        if (r.header)   // 文件夹分组表头
        {
            g.setColour (mb::ui::card);
            g.fillRect (0, 0, w, h);
            g.setColour (mb::ui::border);
            g.fillRect (0, h - 1, w, 1);
            g.setFont (mb::ui::font (8.6f, true));
            g.setColour (mb::ui::subText);
            g.drawText (r.text, 8, 0, w - 16, h, juce::Justification::centredLeft, true);
            return;
        }

        const auto d = catalog_.get (r.idx);
        if (d.name.isEmpty() && d.fileOrIdentifier.isEmpty()) return;

        if (selected)
        {
            g.setColour (mb::ui::accent[0].withAlpha (0.15f));
            g.fillRect (0, 0, w, h);
        }
        else if (row % 2 == 1)
        {
            g.setColour (mb::ui::stripOdd);
            g.fillRect (0, 0, w, h);
        }

        g.setColour (mb::ui::meterGreen);
        g.fillEllipse (8.0f, h * 0.5f - 3.0f, 6.0f, 6.0f);

        // 窄栏自适应：右侧先留「类型 / 通道数」，够宽才额外显示厂商
        const int chanX = w - 50;
        const int typeX = chanX - (w >= 360 ? 66 : 56);
        const bool showVendor = w >= 440;
        const int nameEnd = showVendor ? juce::jmax (150, w - 290) : typeX - 6;

        g.setFont (mb::ui::font (10.5f, true));
        g.setColour (mb::ui::text);
        g.drawText (d.name, 20, 0, juce::jmax (60, nameEnd - 20), h,
                    juce::Justification::centredLeft, true);

        g.setFont (mb::ui::font (8.8f));
        g.setColour (mb::ui::subText);
        if (showVendor)
            g.drawText (d.manufacturerName, nameEnd, 0, 140, h,
                        juce::Justification::centredLeft, true);
        g.drawText (d.isInstrument ? mb::tr ("乐器", "instrument") : mb::tr ("效果", "effect"),
                    typeX, 0, 56, h, juce::Justification::centredLeft, true);
        g.drawText (juce::String (d.numInputChannels) + mb::S("/") + juce::String (d.numOutputChannels),
                    chanX, 0, 44, h, juce::Justification::centredRight, true);
    }

private:
    //--------------------------------------------------------------------------
    // 左列：扫描目录（默认目录在前，自定义目录在后）
    // 每行左侧多选框：自定义目录可多选批量移除；默认目录只读。
    // 勾选按「路径」而非行号保存，扫描 / 增删目录导致行号变化也不会错位。
    //--------------------------------------------------------------------------
    class DirListModel : public juce::ListBoxModel
    {
    public:
        explicit DirListModel (mb::PluginCatalog& c) : catalog (c) {}

        std::function<void()> onToggle;

        int getNumRows() override
        {
            return mb::PluginCatalog::defaultDirectories().size()
                 + catalog.customDirectories().size();
        }

        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            const bool custom = isCustom (row);
            const auto path = pathOf (row);
            const bool on = custom && checked_.count (path) > 0;

            if (selected)
            {
                g.setColour (mb::ui::accent[0].withAlpha (0.15f));
                g.fillRect (0, 0, w, h);
            }

            // 多选框：自定义目录可勾选；默认目录画淡框表示只读
            const auto box = juce::Rectangle<float> (6.0f, h * 0.5f - 6.0f, 12.0f, 12.0f);
            g.setColour (on ? mb::ui::accent[0] : mb::ui::sectionBg);
            g.fillRoundedRectangle (box, 2.0f);
            g.setColour (on ? mb::ui::accent[0]
                            : (custom ? mb::ui::subText : mb::ui::faintText));
            g.drawRoundedRectangle (box.reduced (0.5f), 2.0f, 1.0f);
            if (on)
            {
                g.setColour (juce::Colours::white);
                juce::Path tick;
                tick.startNewSubPath (box.getX() + 2.5f, box.getCentreY());
                tick.lineTo (box.getX() + 5.0f, box.getCentreY() + 2.5f);
                tick.lineTo (box.getX() + 9.5f, box.getCentreY() - 2.5f);
                g.strokePath (tick, juce::PathStrokeType (1.6f));
            }

            g.setFont (mb::ui::font (9.0f, ! custom));
            g.setColour (custom ? mb::ui::text : mb::ui::subText);
            g.drawText (path, 24, 0, juce::jmax (40, w - 24 - 48), h,
                        juce::Justification::centredLeft, true);
            g.setFont (mb::ui::font (7.6f));
            g.setColour (mb::ui::faintText);
            g.drawText (custom ? mb::tr ("自定义", "custom") : mb::tr ("默认", "default"),
                        w - 44, 0, 40, h, juce::Justification::centredRight, true);
        }

        void listBoxItemClicked (int row, const juce::MouseEvent& e) override
        {
            if (! isCustom (row) || e.x > 20) return;      // 只点左侧多选框才切换
            const auto path = pathOf (row);
            if (checked_.count (path) > 0) checked_.erase (path);
            else                           checked_.insert (path);
            if (onToggle) onToggle();
        }

        juce::String getTooltipForRow (int row) override
        {
            return isCustom (row)
                 ? pathOf (row) + mb::tr ("（勾选可批量移除）", " (tick to remove)")
                 : pathOf (row) + mb::tr ("（默认目录，不可移除）", " (default, read-only)");
        }

        juce::String pathOf (int row) const
        {
            const auto def = mb::PluginCatalog::defaultDirectories();
            if (row < def.size()) return def[row];
            const auto custom = catalog.customDirectories();
            const int i = row - def.size();
            return i >= 0 && i < custom.size() ? custom[i] : juce::String();
        }

        bool isCustom (int row) const { return row >= mb::PluginCatalog::defaultDirectories().size(); }

        int checkedCount() const { return static_cast<int> (checked_.size()); }
        std::vector<juce::String> checkedPaths() const
        {
            return std::vector<juce::String> (checked_.begin(), checked_.end());
        }
        void clearChecked() { checked_.clear(); }

        /** 全选 / 取消全选（只针对自定义目录）。 */
        void toggleSelectAll()
        {
            const auto custom = catalog.customDirectories();
            bool all = ! custom.isEmpty();
            for (const auto& p : custom)
                if (checked_.count (p) == 0) { all = false; break; }
            if (all) checked_.clear();
            else for (const auto& p : custom) checked_.insert (p);
        }

        /** 目录在别处被移除后清掉失效勾选。 */
        void pruneChecked()
        {
            const auto custom = catalog.customDirectories();
            std::set<juce::String> keep;
            for (const auto& p : checked_)
                if (custom.contains (p)) keep.insert (p);
            checked_.swap (keep);
        }

        mb::PluginCatalog& catalog;
        std::set<juce::String> checked_;
    };

    /** 移除按钮显示已勾选数量，便于批量操作前确认。 */
    void refreshRemoveButtonText()
    {
        const int n = dirModel_.checkedCount();
        removeDirBtn_.setButtonText (n > 0
            ? mb::tr ("移除目录 (", "Remove (") + juce::String (n) + mb::S(")")
            : mb::tr ("移除目录", "Remove"));
    }

    /** 左右两栏宽度：左列（目录）占大头，长路径不再被右侧清单挤压；
        右列保留但收窄到约 1/3，仍是插件扫描结果清单。 */
    void columnWidths (const juce::Rectangle<int>& card, int& leftW, int& rightW) const
    {
        const int bx = card.getX() + 18;
        const int avail = juce::jmax (260, card.getRight() - 18 - bx);
        rightW = juce::jlimit (210, 300, avail * 34 / 100);
        leftW  = avail - rightW - 12;
    }

    struct Row { bool header = false; juce::String text; int idx = -1; };

    void rebuildRows()
    {
        rows_.clear();
        std::map<juce::String, std::vector<int>> groups;
        for (int i = 0; i < catalog_.count(); ++i)
        {
            const auto d = catalog_.get (i);
            auto dir = juce::File (d.fileOrIdentifier).getParentDirectory().getFullPathName();
            if (dir.isEmpty()) dir = mb::tr ("未知目录", "Unknown folder");
            groups[dir].push_back (i);
        }
        for (const auto& [dir, idxs] : groups)
        {
            rows_.push_back ({ true, dir + mb::S("  ·  ") + juce::String (idxs.size()), -1 });
            for (int i : idxs) rows_.push_back ({ false, {}, i });
        }
    }

    juce::Rectangle<int> cardBounds() const
    {
        const int w = juce::jmin (820, juce::jmax (560, getParentWidth() - 80));
        const int h = juce::jmin (560, juce::jmax (380, getParentHeight() - 80));
        return juce::Rectangle<int> ((getParentWidth() - w) / 2,
                                     (getParentHeight() - h) / 2, w, h);
    }

    static juce::File defaultDir()
    {
        const auto dirs = mb::PluginCatalog::defaultDirectories();
        return dirs.size() > 0 ? juce::File (dirs[0])
             : juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                    .getChildFile ("VST3");
    }

    void timerCallback() override
    {
        if (! catalog_.isScanning())
        {
            stopTimer();
            refresh();
            if (onMessage)
                onMessage (mb::tr ("插件扫描完成，共 ", "Scan finished: ") + juce::String (catalog_.count())
                           + mb::tr (" 个插件", " plugins"));
            if (onListChanged) onListChanged();
            return;
        }
        repaint();
    }

    mb::PluginCatalog& catalog_;
    bool plainMode_ = false;   // 设置页内嵌模式：无遮罩、卡片铺满
    juce::ListBox list_, dirList_;
    DirListModel dirModel_ { catalog_ };
    juce::TextButton scanBtn_, addDirBtn_, openBtn_, removeDirBtn_,
                     selectAllBtn_, scanOnlyBtn_, closeBtn_;
    std::vector<Row> rows_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPanel)
};
