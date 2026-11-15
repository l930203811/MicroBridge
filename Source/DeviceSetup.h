#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "AudioEngine.h"
#include "I18n.h"
#include "Theme.h"

//==============================================================================
// 启动 / 运行时的「默认设备」选择层。
// 首次启动必选一次；之后可从顶栏「设备」重新打开。
// 这里没有「启动 / 停止」——点确定后音频引擎自动开始运行。
//
// 结构：
//   1) 只选「驱动类型」+ 一台「设备」（不再分别选输入/输出设备）
//   2) 设备下拉下方左右两列：主设备输入通道（左）/ 输出通道（右），默认全部勾选
//   3) 其它设备同样左右两列：输入设备（左）/ 输出设备（右），默认不勾选；
//      列内按驱动分组折叠，每组标题带「全选 / 清空」（只作用于本组）；
//      每行都有「备注」入口（设备备注 / 通道备注，共用同一个编辑浮层）
//==============================================================================
class DeviceSetupOverlay : public juce::Component,
                           private juce::Button::Listener
{
public:
    std::function<void (mb::DeviceChoice)> onApply;
    std::function<void()> onCancel;
    std::function<void()> onNoteChanged;   // 备注保存后通知主界面刷新节点视图 / 通道条

    DeviceSetupOverlay (mb::AudioEngine& engine, mb::DeviceChoice initial, bool firstRun)
        : engine_ (engine), firstRun_ (firstRun)
    {
        addAndMakeVisible (card_);

        title_.setText (mb::tr ("默认音频设备", "Default Audio Device"), juce::dontSendNotification);
        title_.setFont (mb::ui::font (17.0f, true));
        title_.setColour (juce::Label::textColourId, mb::ui::text);
        card_.addAndMakeVisible (title_);

        subtitle_.setText (mb::tr ("选择驱动与设备，勾选需要的通道；确认后音频自动运行。",
                                   "Pick a driver and device, tick the channels you need; audio runs automatically."),
                           juce::dontSendNotification);
        subtitle_.setFont (mb::ui::font (10.5f));
        subtitle_.setColour (juce::Label::textColourId, mb::ui::subText);
        subtitle_.setJustificationType (juce::Justification::centredLeft);
        card_.addAndMakeVisible (subtitle_);

        auto makeLabel = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (mb::ui::font (10.5f, true));
            l.setColour (juce::Label::textColourId, mb::ui::subText);
            card_.addAndMakeVisible (l);
        };

        makeLabel (typeLabel_,      mb::tr ("驱动类型  /  DRIVER",        "DRIVER TYPE"));
        makeLabel (deviceLabel_,    mb::tr ("设备  /  DEVICE",             "DEVICE"));
        makeLabel (rateLabel_,      mb::tr ("采样率  /  RATE",             "SAMPLE RATE"));
        makeLabel (bufLabel_,       mb::tr ("缓冲  /  BUFFER",             "BUFFER"));
        makeLabel (masterInLabel_,  mb::tr ("输入通道  /  INPUT CHANNELS",  "INPUT CHANNELS"));
        makeLabel (masterOutLabel_, mb::tr ("输出通道  /  OUTPUT CHANNELS", "OUTPUT CHANNELS"));
        makeLabel (auxLabel_,       mb::tr ("其他设备  /  OTHER DEVICES",   "OTHER DEVICES"));
        makeLabel (auxInLabel_,     mb::tr ("输入设备  /  INPUT DEVICES",   "INPUT DEVICES"));
        makeLabel (auxOutLabel_,    mb::tr ("输出设备  /  OUTPUT DEVICES",  "OUTPUT DEVICES"));

        auto setupCombo = [this] (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, mb::ui::card);
            c.setColour (juce::ComboBox::textColourId,       mb::ui::text);
            c.setColour (juce::ComboBox::outlineColourId,    mb::ui::border);
            c.setColour (juce::ComboBox::arrowColourId,      mb::ui::subText);
            card_.addAndMakeVisible (c);
        };
        setupCombo (typeBox_);
        setupCombo (deviceBox_);
        setupCombo (rateBox_);
        setupCombo (bufferBox_);

        auto setupList = [this] (juce::TextButton* allB, juce::TextButton* noneB,
                                 juce::Viewport& v, DeviceList& list)
        {
            if (allB != nullptr && noneB != nullptr)
            {
                for (auto* b : { allB, noneB })
                {
                    b->setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
                    b->setColour (juce::TextButton::textColourOffId, mb::ui::text);
                    b->setButtonText (b == allB ? mb::tr ("全选", "All") : mb::tr ("清空", "None"));
                    card_.addAndMakeVisible (b);
                }
            }
            v.setViewedComponent (&list, false);
            // 折叠 / 展开后外层重算卡片与列表高度。
            list.onLayoutChanged = [this] { if (getWidth() > 0 && getHeight() > 0) resized(); };
            card_.addAndMakeVisible (v);
        };
        setupList (&masterInAllBtn_,  &masterInNoneBtn_,  masterInView_,  masterInList_);
        setupList (&masterOutAllBtn_, &masterOutNoneBtn_, masterOutView_, masterOutList_);
        // 其它设备同样分左右两列，但列标题不再放「全选 / 清空」；
        // 全选 / 清空只保留在每个驱动分组标题上，避免同页重复操作。
        setupList (nullptr, nullptr, auxInView_,  auxInList_);
        setupList (nullptr, nullptr, auxOutView_, auxOutList_);
        auxInList_.groupActions  = true;
        auxOutList_.groupActions = true;

        // 主设备下拉旁不再放「备注」：备注应针对输入 / 输出通道，而不是驱动 / 设备名。
        // 下方各列表每行右侧的「备注」小按钮 → 打开同一个编辑浮层；
        // 主设备通道行是通道对备注，浮层标题改为「通道备注」。
        auto onAuxNote = [this] (const juce::String& key, const juce::String& nm, bool ch)
        {
            if (ch)
                showNoteEditor (key, nm,
                                mb::tr ("通道备注", "Channel note"),
                                mb::tr ("通道名  /  CHANNEL", "CHANNEL"));
            else
                showNoteEditor (key, nm);
        };
        masterInList_.onEditNote  = onAuxNote;
        masterOutList_.onEditNote = onAuxNote;
        auxInList_.onEditNote     = onAuxNote;
        auxOutList_.onEditNote    = onAuxNote;

        masterInAllBtn_.onClick  = [this] { masterInList_.setAllTicked (true); };
        masterInNoneBtn_.onClick = [this] { masterInList_.setAllTicked (false); };
        masterOutAllBtn_.onClick = [this] { masterOutList_.setAllTicked (true); };
        masterOutNoneBtn_.onClick= [this] { masterOutList_.setAllTicked (false); };

        okBtn_.setButtonText (mb::tr ("确定", "Apply"));
        okBtn_.addListener (this);
        card_.addAndMakeVisible (okBtn_);

        cancelBtn_.setButtonText (mb::tr ("取消", "Cancel"));
        cancelBtn_.addListener (this);
        card_.addAndMakeVisible (cancelBtn_);   // 取消按钮始终显示（含首次/设备失效场景）

        error_.setFont (mb::ui::font (10.0f));
        error_.setColour (juce::Label::textColourId, mb::ui::meterRed);
        card_.addAndMakeVisible (error_);

        loadChoices (initial);
        refreshLists();
        // 必须放在 loadChoices 之后：构造期 addItem 可能触发选择变化，
        // 提前挂 onChange 会把刚预选的驱动/设备重置掉。
        typeBox_.onChange   = [this] { refreshDeviceList(); refreshLists(); };
        deviceBox_.onChange = [this] { refreshLists(); };
    }

    void applyLanguage()
    {
        refreshTypeItems();     // 驱动类型下拉的显示名随语言重建，真实名保存在 typeRawNames_
        // 设备下拉首项也是翻译文案；原地改文本，不重建列表以免冲掉未应用的勾选
        if (deviceBox_.getNumItems() > 0)
            deviceBox_.changeItemText (1, mb::tr ("（不使用设备）", "(no device)"));
        title_.setText (mb::tr ("默认音频设备", "Default Audio Device"), juce::dontSendNotification);
        subtitle_.setText (mb::tr ("选择驱动与设备，勾选需要的通道；确认后音频自动运行。",
                                   "Pick a driver and device, tick the channels you need; audio runs automatically."),
                           juce::dontSendNotification);
        typeLabel_.setText      (mb::tr ("驱动类型  /  DRIVER",         "DRIVER TYPE"), juce::dontSendNotification);
        deviceLabel_.setText    (mb::tr ("设备  /  DEVICE",              "DEVICE"), juce::dontSendNotification);
        rateLabel_.setText      (mb::tr ("采样率  /  RATE",              "SAMPLE RATE"), juce::dontSendNotification);
        bufLabel_.setText       (mb::tr ("缓冲  /  BUFFER",              "BUFFER"), juce::dontSendNotification);
        masterInLabel_.setText  (mb::tr ("输入通道  /  INPUT CHANNELS",  "INPUT CHANNELS"), juce::dontSendNotification);
        masterOutLabel_.setText (mb::tr ("输出通道  /  OUTPUT CHANNELS", "OUTPUT CHANNELS"), juce::dontSendNotification);
        auxLabel_.setText       (mb::tr ("其他设备  /  OTHER DEVICES",   "OTHER DEVICES"), juce::dontSendNotification);
        auxInLabel_.setText     (mb::tr ("输入设备  /  INPUT DEVICES",   "INPUT DEVICES"), juce::dontSendNotification);
        auxOutLabel_.setText    (mb::tr ("输出设备  /  OUTPUT DEVICES",  "OUTPUT DEVICES"), juce::dontSendNotification);
        for (auto* b : { &masterInAllBtn_, &masterOutAllBtn_ })
            b->setButtonText (mb::tr ("全选", "All"));
        for (auto* b : { &masterInNoneBtn_, &masterOutNoneBtn_ })
            b->setButtonText (mb::tr ("清空", "None"));
        okBtn_.setButtonText (mb::tr ("确定", "Apply"));
        cancelBtn_.setButtonText (mb::tr ("取消", "Cancel"));
        if (noteEditor_ != nullptr) noteEditor_->applyLanguage();
        repaint();
        // 分组标题上的「全选 / 清空」、行内「备注」按钮在 paint 里实时读当前语言。
        auxInList_.repaint();
        auxOutList_.repaint();
    }

    void setError (const juce::String& message)
    {
        error_.setText (message, juce::dontSendNotification);
        repaint();
    }

    /** 内嵌模式（设置页「设备设置」Tab）：不画遮罩，卡片铺满；
        自带的 确定 / 取消 按钮隐藏，改由设置页页脚统一「应用」。 */
    void setPlainMode (bool p)
    {
        plainMode_ = p;
        okBtn_.setVisible (! p);
        cancelBtn_.setVisible (! p);
        resized();
        repaint();
    }

    /** 校验并应用当前选择（供内嵌模式的设置页页脚「应用」按钮调用）。返回是否成功。 */
    bool tryApply()
    {
        // 驱动 / 设备未选好时给出页内错误提示，不应用。
        // 注意：下标 0 是合法项（驱动列表第一项 / 设备列表「（不使用设备）」），
        // 只有 -1 才是「未选择」。旧版 <= 0 会让「不使用设备」根本无法应用。
        juce::String err;
        if (typeBox_.getSelectedItemIndex() < 0)  err = mb::tr ("请先选择驱动类型。", "Pick a driver type first.");
        else if (deviceBox_.getSelectedItemIndex() < 0) err = mb::tr ("请先选择设备。", "Pick a device first.");
        error_.setText (err, juce::dontSendNotification);
        if (err.isNotEmpty()) return false;

        // 主设备通道：未勾选的写入「取消名单」（默认全部可见）
        juce::StringArray offIn, offOut;
        for (auto* list : { &masterInList_, &masterOutList_ })
        {
            for (const auto& k : list->keysWithState (true,  false)) offIn.addIfNotAlreadyThere (k);
            for (const auto& k : list->keysWithState (false, false)) offOut.addIfNotAlreadyThere (k);
        }
        // 其他设备：左列输入 / 右列输出，勾选的写入「显示名单」（默认不显示）
        juce::StringArray shownIn, shownOut;
        for (auto* list : { &auxInList_, &auxOutList_ })
        {
            for (const auto& k : list->keysWithState (true,  true)) shownIn.addIfNotAlreadyThere (k);
            for (const auto& k : list->keysWithState (false, true)) shownOut.addIfNotAlreadyThere (k);
        }

        engine_.catalog().setAuxShown (shownIn, shownOut);
        engine_.catalog().setMasterOff (offIn, offOut);
        mb::DevicePreferences::ShownAux shown { shownIn, shownOut };
        mb::DevicePreferences{}.saveShown (shown);
        mb::DevicePreferences::MasterOff off { offIn, offOut };
        mb::DevicePreferences{}.saveMasterOff (off);

        mb::DeviceChoice c;
        c.typeName = currentTypeName();
        // 下拉显示的是「备注名  设备原名」，应用时还原成真实设备名；
        // 单向端点（虚拟声卡 A/B 端、WASAPI 麦克风/扬声器）自动补上配对的另一侧。
        const auto resolved = engine_.catalog().resolveChoice (c.typeName, selectedDeviceName());
        c.inputName  = resolved.inputName;
        c.outputName = resolved.outputName;
        c.sampleRate = rateBox_.getSelectedItemIndex() == 0 ? 0.0
                     : rateBox_.getText().upToFirstOccurrenceOf (mb::S(" "), false, false).getDoubleValue();
        c.bufferSize = bufferBox_.getSelectedItemIndex() == 0 ? 0
                     : bufferBox_.getText().upToFirstOccurrenceOf (mb::S(" "), false, false).getIntValue();
        c.valid = true;

        if (onApply) onApply (c);
        return true;
    }

    void paint (juce::Graphics& g) override
    {
        if (! plainMode_)                       // 内嵌模式不画全屏遮罩
            g.fillAll (mb::ui::text.withAlpha (0.42f));
    }

    void resized() override
    {
        // 弹窗模式：在上一版加宽基础上裁掉五分之一：卡片宽度取可用宽度的 80%，
        // 上限 900 -> 720；下限 420 保证左右两列布局不被挤坏。
        // 内嵌模式（设置页 Tab）：卡片直接铺满给定的组件区域。
        // 长通道名仍由 drawFittedText + tooltip 兜底。
        const int cardW = plainMode_ ? getWidth()
                                     : juce::jlimit (420, 720,
                                                     juce::roundToInt ((getWidth() - 24) * 0.8f));
        const int maxH  = plainMode_ ? getHeight() : juce::jmax (320, getHeight() - 24);

        // 主设备通道区按内容自适应高度：多条通道时不需要下拉即可完整显示。
        const int contentH = juce::jmax (masterInList_.preferredHeight(),
                                         masterOutList_.preferredHeight()) + 4;
        // 主区之前 168+20、间隔 8、其它设备区（区标题 20 + 两列标题 20 + 列表最小 120）、底部 70
        const int reserveH = 426;
        const int masterH  = juce::jlimit (80, juce::jmax (80, maxH - reserveH), contentH);
        const int cardH    = plainMode_ ? getHeight()
                                        : juce::jmin (maxH, juce::jmax (470, reserveH + masterH));
        card_.setBounds (plainMode_ ? juce::Rectangle<int> (0, 0, cardW, cardH)
                                    : juce::Rectangle<int> ((getWidth() - cardW) / 2,
                                                            (getHeight() - cardH) / 2,
                                                            cardW, cardH));

        title_.setBounds    (24, 14, cardW - 48, 24);
        subtitle_.setBounds (24, 40, cardW - 48, 18);

        const int colW = (cardW - 48 - 16) / 2;
        const int x1 = 24, x2 = 24 + colW + 16;
        const int fullW = cardW - 48;

        typeLabel_.setBounds   (x1, 64, colW, 16);
        typeBox_.setBounds     (x1, 82, colW, 26);
        rateLabel_.setBounds   (x2, 64, colW, 16);
        rateBox_.setBounds     (x2, 82, colW, 26);

        deviceLabel_.setBounds (x1, 114, colW, 16);
        deviceBox_.setBounds   (x1, 132, colW, 26);
        bufLabel_.setBounds    (x2, 114, colW, 16);
        bufferBox_.setBounds   (x2, 132, colW, 26);

        // 主设备通道：输入在左、输出在右，两列各自全选 / 清空
        const int masterY = 168;
        masterInLabel_.setBounds  (x1, masterY, colW - 104, 16);
        masterInAllBtn_.setBounds (x1 + colW - 100, masterY - 1, 48, 18);
        masterInNoneBtn_.setBounds(x1 + colW - 48,  masterY - 1, 48, 18);
        masterOutLabel_.setBounds (x2, masterY, colW - 104, 16);
        masterOutAllBtn_.setBounds (x2 + colW - 100, masterY - 1, 48, 18);
        masterOutNoneBtn_.setBounds(x2 + colW - 48,  masterY - 1, 48, 18);

        masterInView_.setBounds  (x1, masterY + 20, colW, masterH);
        masterOutView_.setBounds (x2, masterY + 20, colW, masterH);

        // 其他设备区：左右两列（输入 / 输出）只显示列标题；
        // 「全选 / 清空」由每个驱动分组标题自带，列标题可占满整列宽度。
        const int auxY = masterY + 20 + masterH + 8;
        auxLabel_.setBounds  (x1, auxY, fullW, 16);
        const int colLabelY = auxY + 20;
        auxInLabel_.setBounds   (x1, colLabelY, colW, 16);
        auxOutLabel_.setBounds  (x2, colLabelY, colW, 16);
        const int auxListY = colLabelY + 20;
        const int auxH = juce::jmax (80, cardH - auxListY - (plainMode_ ? 24 : 70));
        auxInView_.setBounds   (x1, auxListY, colW, auxH);
        auxOutView_.setBounds  (x2, auxListY, colW, auxH);

        masterInList_.setSize  (juce::jmax (10, colW - 14),  masterInList_.preferredHeight());
        masterOutList_.setSize (juce::jmax (10, colW - 14),  masterOutList_.preferredHeight());
        auxInList_.setSize     (juce::jmax (10, colW - 14),  auxInList_.preferredHeight());
        auxOutList_.setSize    (juce::jmax (10, colW - 14),  auxOutList_.preferredHeight());

        error_.setBounds       (24, cardH - 60, cardW - 48, 18);
        okBtn_.setBounds       (cardW - 24 - 116, cardH - 40, 116, 30);
        cancelBtn_.setBounds   (cardW - 24 - 116 - 8 - 90, cardH - 40, 90, 30);
        positionNoteEditor();
    }

private:
    class Card : public juce::Component
    {
    public:
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (mb::ui::card);
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (mb::ui::border);
            g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
            g.setColour (mb::ui::accent[0]);
            g.fillRoundedRectangle (0.0f, 0.0f, 4.0f, r.getHeight(), 2.0f);
        }
    };

    //==========================================================================
    /** 备注编辑浮层：左半是备注名输入框，右半只读显示设备 / 通道原名。
        其它设备行与主设备通道行共用这一份编辑器；主设备下拉不再提供入口。 */
    class NoteEditor : public juce::Component
    {
    public:
        std::function<void (bool, const juce::String&)> onDone;   // true = 保存
        juce::String deviceName;

        NoteEditor()
        {
            edit_.setJustification (juce::Justification::centredLeft);
            edit_.setIndents (6, 0);
            edit_.setFont (mb::ui::font (12.0f));
            edit_.setColour (juce::TextEditor::backgroundColourId, mb::ui::sectionBg);
            edit_.setColour (juce::TextEditor::textColourId,       mb::ui::text);
            edit_.setColour (juce::TextEditor::outlineColourId,    mb::ui::border);
            edit_.onReturnKey = [this] { finish (true); };
            edit_.onEscapeKey = [this] { finish (false); };
            addAndMakeVisible (edit_);

            for (auto* b : { &ok_, &cancel_ })
            {
                b->setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
                b->setColour (juce::TextButton::textColourOffId, mb::ui::text);
                addAndMakeVisible (b);
            }
            ok_.onClick     = [this] { finish (true); };
            cancel_.onClick = [this] { finish (false); };
            applyLanguage();
        }

        void applyLanguage()
        {
            ok_.setButtonText     (mb::tr ("确定", "Save"));
            cancel_.setButtonText (mb::tr ("取消", "Cancel"));
            repaint();
        }

        void start (const juce::String& device, const juce::String& current,
                    const juce::String& caption = {}, const juce::String& rightCaption = {})
        {
            deviceName = device;
            caption_    = caption;       // 空 = 默认「设备备注 / Device note」
            rightText_  = rightCaption;
            edit_.setText (current, false);
            edit_.setSelectAllWhenFocused (true);
            setVisible (true);
            toFront (false);
            edit_.grabKeyboardFocus();
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat();
            g.setColour (mb::ui::card);
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (mb::ui::border);
            g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
            g.setColour (mb::ui::accent[0]);
            g.fillRoundedRectangle (0.0f, 0.0f, 4.0f, r.getHeight(), 2.0f);

            g.setFont (mb::ui::font (13.0f, true));
            g.setColour (mb::ui::text);
            g.drawText (caption_.isEmpty() ? mb::tr ("设备备注", "Device note") : caption_,
                        16, 10, getWidth() - 32, 22,
                        juce::Justification::centredLeft, false);

            const int split = splitX();
            g.setFont (mb::ui::font (9.0f));
            g.setColour (mb::ui::subText);
            g.drawText (mb::tr ("备注名  /  NOTE", "NOTE"), 16, 36, split - 32, 16,
                        juce::Justification::centredLeft, false);
            g.drawText (rightText_.isEmpty() ? mb::tr ("设备名  /  DEVICE", "DEVICE") : rightText_,
                        split + 8, 36,
                        getWidth() - split - 24, 16, juce::Justification::centredRight, false);

            g.setFont (mb::ui::font (10.5f));
            g.setColour (mb::ui::text);
            g.drawFittedText (deviceName, split + 8, 56, getWidth() - split - 24, 28,
                              juce::Justification::centredRight, 1, 0.55f);
        }

        void resized() override
        {
            edit_.setBounds (16, 56, splitX() - 26, 28);
            ok_.setBounds     (getWidth() - 16 - 90, getHeight() - 36, 90, 26);
            cancel_.setBounds (getWidth() - 16 - 90 - 8 - 74, getHeight() - 36, 74, 26);
        }

    private:
        int splitX() const { return juce::roundToInt (getWidth() * 0.52f); }

        void finish (bool save)
        {
            if (onDone) onDone (save, save ? edit_.getText().trim() : juce::String());
        }

        juce::TextEditor edit_;
        juce::TextButton ok_, cancel_;
        juce::String caption_, rightText_;   // 浮层标题 / 右栏说明（通道备注模式覆盖）
    };

    //==========================================================================
    // 通道 / 设备多选列表。
    // 条目自绘（勾选框 + drawFittedText），长通道名缩放而不省略；
    // 分组标题是普通的下拉展开行（淡色块 + Path 三角，不用 Unicode 字符），
    // 右侧可带本组专用的「全选 / 清空」小按钮（其它设备列表开启）。
    //==========================================================================
    class DeviceList : public juce::Component
    {
    public:
        struct Item
        {
            juce::String key, label, tooltip, group, groupKey;
            bool input = true;
            bool header = false;
            bool headerActions = true;         // 占位提示行不画「全选 / 清空」
            bool headerCollapsed = false;      // 分组是否默认折叠（非主驱动分组为 true）
            // 备注两栏行：左侧备注名、右侧设备原名；noteKey 用于写回 device.xml
            juce::String note, deviceName, noteKey;
            bool noteEditable = false;
            bool showIoTag = false;            // 行首 IN / OUT 方向标记（其它设备行开启）
            bool channelNoteRow = false;       // 通道对备注行（浮层标题区分）
        };

        /** 点击行右侧「备注」小按钮：请求上层打开备注编辑浮层。
            isChannel = 通道对备注（浮层标题用「通道备注」而非「设备备注」）。 */
        std::function<void (const juce::String& key, const juce::String& deviceName,
                            bool isChannel)> onEditNote;

        /** keys 语义由 keysAreOffList 决定：
            true  = keys 是「取消勾选名单」→ 不在名单里默认全勾（主设备通道）；
            false = keys 是「显示名单」    → 在名单里才勾选（其他设备）。

            inKeys / outKeys 必须按方向分开传：同一台设备的输入名与输出名
            相同时，IN 行与 OUT 行的 key 完全一致，若合并成一个名单，只勾
            一侧会在下次打开设置页时把另一侧也显示成已勾选并被应用写回 ——
            也就是「勾一个强制把输入/输出都勾上」。 */
        void setItems (std::vector<Item> items,
                       const juce::StringArray& inKeys,
                       const juce::StringArray& outKeys,
                       bool keysAreOffList)
        {
            items_ = std::move (items);
            boxes_.clear();
            removeAllChildren();

            for (const auto& it : items_)
                if (it.header && collapsed_.find (it.groupKey) == collapsed_.end())
                    collapsed_[it.groupKey] = it.headerCollapsed;

            for (const auto& it : items_)
            {
                if (it.header)
                {
                    auto h = std::make_unique<HeaderRow>();
                    h->title = it.group;
                    h->showActions = groupActions && it.headerActions;
                    h->setMouseCursor (juce::MouseCursor::PointingHandCursor);
                    h->onClick = [this, gk = it.groupKey]
                    {
                        collapsed_[gk] = ! collapsed_[gk];
                        refreshHeaders();
                        resized();
                        if (onLayoutChanged) onLayoutChanged();
                    };
                    h->onGroupAction = [this, gk = it.groupKey] (bool ticked)
                    {
                        setGroupTicked (gk, ticked);
                    };
                    addAndMakeVisible (*h);
                    boxes_.push_back (std::move (h));
                    continue;
                }

                auto row = std::make_unique<CheckRow>();
                row->label   = it.label;
                const auto& keys = it.input ? inKeys : outKeys;
                row->ticked  = keysAreOffList ? ! keys.contains (it.key) : keys.contains (it.key);
                row->noteEditable = it.noteEditable;
                row->showIoTag   = it.showIoTag;
                if (it.showIoTag)
                    row->ioTag = it.input ? mb::tr ("IN", "IN") : mb::tr ("OUT", "OUT");
                row->note        = it.note;
                row->deviceName  = it.deviceName;
                row->onEditNote  = [this, key = it.noteKey, nm = it.deviceName,
                                    ch = it.channelNoteRow]
                {
                    if (onEditNote) onEditNote (key, nm, ch);
                };
                row->setTooltip (it.tooltip);
                row->setMouseCursor (juce::MouseCursor::PointingHandCursor);
                addAndMakeVisible (*row);
                boxes_.push_back (std::move (row));
            }
            refreshHeaders();
            resized();
        }

        void resized() override
        {
            int y = 2;
            for (size_t i = 0; i < boxes_.size(); ++i)
            {
                const auto& it = items_[i];
                if (it.header)
                {
                    boxes_[i]->setBounds (2, y, getWidth() - 4, 22);
                    y += 24;
                    continue;
                }
                const bool show = ! isCollapsed (it.groupKey);
                boxes_[i]->setVisible (show);
                if (show)
                {
                    boxes_[i]->setBounds (2, y, getWidth() - 4, 22);
                    y += 23;
                }
            }
        }

        int preferredHeight() const
        {
            int h = 2;
            for (const auto& it : items_)
            {
                if (it.header)                    { h += 24; continue; }
                if (isCollapsed (it.groupKey)) continue;
                h += 23;
            }
            return juce::jmax (10, h);
        }

        juce::StringArray keysWithState (bool wantInput, bool wantTicked) const
        {
            juce::StringArray keys;
            for (size_t i = 0; i < boxes_.size(); ++i)
            {
                const auto& it = items_[i];
                if (it.header || it.input != wantInput) continue;
                if (auto* r = dynamic_cast<CheckRow*> (boxes_[i].get()))
                    if (r->ticked == wantTicked)
                        keys.addIfNotAlreadyThere (it.key);
            }
            return keys;
        }

        void setAllTicked (bool ticked)
        {
            for (size_t i = 0; i < boxes_.size(); ++i)
            {
                if (items_[i].header) continue;
                if (auto* r = dynamic_cast<CheckRow*> (boxes_[i].get()))
                {
                    r->ticked = ticked;
                    r->repaint();
                }
            }
        }

        /** 只改某一驱动分组的勾选状态（折叠中的组同样生效，但不会展开它）。 */
        void setGroupTicked (const juce::String& groupKey, bool ticked)
        {
            for (size_t i = 0; i < boxes_.size(); ++i)
            {
                if (items_[i].header || items_[i].groupKey != groupKey) continue;
                if (auto* r = dynamic_cast<CheckRow*> (boxes_[i].get()))
                {
                    r->ticked = ticked;
                    r->repaint();
                }
            }
        }

        /** 备注保存后就地刷新行内备注文字，不重建列表 —— 重建会把用户
            尚未点「应用」的勾选状态冲掉（勾选态是从存档名单重算的）。 */
        void updateNotes (const std::function<juce::String (const juce::String&)>& noteForKey)
        {
            for (size_t i = 0; i < boxes_.size(); ++i)
            {
                if (items_[i].header || ! items_[i].noteEditable) continue;
                if (auto* r = dynamic_cast<CheckRow*> (boxes_[i].get()))
                {
                    r->note = noteForKey (items_[i].noteKey);
                    r->repaint();
                }
            }
        }

        /** 分组标题行右侧是否绘制「全选 / 清空」（仅其它设备列表开启）。 */
        bool groupActions = false;
        std::function<void()> onLayoutChanged;

        /** 清空分组的展开 / 折叠记忆：下次 setItems 时按 headerCollapsed
            默认值重建（用于顶部驱动类型变化后，让「同类型组」重新展开）。 */
        void resetGroupStates()
        {
            collapsed_.clear();
        }

    private:
        /** 自绘勾选行：文字用 drawFittedText 横向缩放，长名称不会被省略。 */
        struct CheckRow : public juce::Component,
                          public juce::SettableTooltipClient
        {
            juce::String label, note, deviceName, ioTag;
            bool ticked = false;
            bool noteEditable = false;                 // 设备行：两栏 + 备注按钮
            bool showIoTag = false;                    // 行首 IN / OUT 标记
            std::function<void()> onEditNote;

            void layoutNoteArea()
            {
                constexpr int bw = 40, bh = 16, right = 3;
                noteArea_ = { getWidth() - right - bw, (getHeight() - bh) / 2, bw, bh };
            }

            void paint (juce::Graphics& g) override
            {
                const auto r = getLocalBounds().toFloat();
                if (isMouseOver())
                {
                    g.setColour (mb::ui::sectionBg.withAlpha (0.55f));
                    g.fillRoundedRectangle (r.reduced (0.0f, 1.0f), 3.0f);
                }
                const auto box = juce::Rectangle<float> (5.0f, r.getCentreY() - 7.0f, 14.0f, 14.0f);
                g.setColour (mb::ui::border);
                g.drawRoundedRectangle (box, 3.0f, 1.2f);
                if (ticked)
                {
                    g.setColour (mb::ui::accent[0]);
                    g.fillRoundedRectangle (box.reduced (1.5f), 2.0f);
                    juce::Path tick;
                    tick.startNewSubPath (box.getX() + 3.5f, box.getCentreY());
                    tick.lineTo (box.getX() + 6.5f, box.getCentreY() + 3.0f);
                    tick.lineTo (box.getX() + 11.0f, box.getCentreY() - 3.5f);
                    g.setColour (mb::ui::card);
                    g.strokePath (tick, juce::PathStrokeType (1.6f));
                }

                if (! noteEditable)
                {
                    g.setColour (mb::ui::text);
                    g.setFont (mb::ui::font (10.5f));
                    g.drawFittedText (label, 25, 0, getWidth() - 29, getHeight(),
                                      juce::Justification::centredLeft, 1, 0.60f);
                    return;
                }

                layoutNoteArea();
                const int textX = 25;
                const int avail = juce::jmax (60, getWidth() - textX - noteArea_.getWidth() - 12);
                const int tagW   = showIoTag ? 26 : 0;
                const int nameX  = textX + tagW;
                const int nameAvail = juce::jmax (10, avail - tagW);

                // 方向标记：IN / OUT（仅其它设备行显示）
                if (showIoTag)
                {
                    g.setColour (mb::ui::subText);
                    g.setFont (mb::ui::font (9.0f));
                    g.drawText (ioTag, textX, 0, 26, getHeight(),
                                juce::Justification::centredLeft, false);
                }

                if (note.isEmpty())
                {
                    // 未设置备注：完整名称一直画到「备注」小按钮前。
                    g.setColour (mb::ui::text);
                    g.setFont (mb::ui::font (10.5f));
                    g.drawFittedText (deviceName, nameX, 0, nameAvail, getHeight(),
                                      juce::Justification::centredLeft, 1, 0.50f);
                }
                else
                {
                    // 有备注：仍保持「备注名在左、原名在右」，
                    // 长名缩放而不省略（tooltip 里始终有完整名）。
                    const int rightW = juce::jmax (48, juce::roundToInt (nameAvail * 0.52f));
                    const int leftW  = juce::jmax (10, nameAvail - rightW);
                    g.setColour (mb::ui::text);
                    g.setFont (mb::ui::font (10.5f, true));
                    g.drawFittedText (note, nameX, 0, leftW, getHeight(),
                                      juce::Justification::centredLeft, 1, 0.60f);
                    g.setColour (mb::ui::subText);
                    g.setFont (mb::ui::font (9.0f));
                    g.drawFittedText (deviceName, nameX + leftW, 0, rightW, getHeight(),
                                      juce::Justification::centredRight, 1, 0.50f);
                }

                // 全行唯一的「备注」小按钮：IN / OUT 行标题、样式完全一致
                const auto ar = noteArea_.toFloat();
                g.setColour (hoverNote_ ? mb::ui::border.withAlpha (0.85f)
                                        : mb::ui::card.withAlpha (0.72f));
                g.fillRoundedRectangle (ar, 3.0f);
                g.setColour (mb::ui::border);
                g.drawRoundedRectangle (ar.reduced (0.5f), 3.0f, 1.0f);
                g.setColour (mb::ui::text);
                g.setFont (mb::ui::font (9.0f));
                g.drawText (mb::tr ("备注", "Note"), noteArea_.getX(), 0,
                            noteArea_.getWidth(), getHeight(),
                            juce::Justification::centred, false);
            }

            void updateHover (juce::Point<int> p)
            {
                const bool h = noteEditable && noteArea_.contains (p);
                if (h != hoverNote_) { hoverNote_ = h; repaint(); }
            }
            void mouseEnter (const juce::MouseEvent& e) override { layoutNoteArea(); updateHover (e.getPosition()); }
            void mouseMove  (const juce::MouseEvent& e) override { updateHover (e.getPosition()); }
            void mouseExit  (const juce::MouseEvent&) override   { updateHover ({ -1, -1 }); }

            void mouseDown (const juce::MouseEvent& e) override
            {
                if (noteEditable)
                {
                    layoutNoteArea();
                    if (noteArea_.contains (e.getPosition()))
                    {
                        if (onEditNote) onEditNote();
                        return;                       // 点备注按钮不切换勾选
                    }
                }
                ticked = ! ticked;
                repaint();
            }

        private:
            juce::Rectangle<int> noteArea_;
            bool hoverNote_ = false;
        };

        /** 普通的下拉展开行：淡色块 + 自绘三角（不用 Unicode 字符，避免编码乱码）
            + 普通文字色；不高亮、不像链接。
            右侧两个自绘小按钮 = 只作用于本驱动分组的「全选 / 清空」，
            点击按钮不会触发整行的折叠 / 展开。 */
        struct HeaderRow : public juce::Component
        {
            juce::String title;
            bool expanded = false;
            bool showActions = false;
            std::function<void()> onClick;
            std::function<void (bool)> onGroupAction;   // true = 全选，false = 清空

            void layoutAreas()
            {
                constexpr int bw = 38, bh = 16, gap = 4, right = 3;
                const int y = (getHeight() - bh) / 2;
                int x = getWidth() - right - bw;
                noneArea_ = { x, y, bw, bh };
                x -= gap + bw;
                allArea_  = { x, y, bw, bh };
            }

            void resized() override { layoutAreas(); }

            void paint (juce::Graphics& g) override
            {
                layoutAreas();
                const auto r = getLocalBounds().toFloat();
                g.setColour (mb::ui::sectionBg.withAlpha (isMouseOver() ? 0.95f : 0.60f));
                g.fillRoundedRectangle (r.reduced (0.0f, 1.0f), 4.0f);

                juce::Path tri;
                const float cx = 12.0f, cy = r.getCentreY();
                if (expanded)
                    tri.addTriangle (cx - 3.5f, cy - 2.0f, cx + 3.5f, cy - 2.0f, cx, cy + 3.0f);
                else
                    tri.addTriangle (cx - 2.0f, cy - 3.5f, cx - 2.0f, cy + 3.5f, cx + 3.0f, cy);
                g.setColour (mb::ui::subText);
                g.fillPath (tri);

                const int actionW = showActions ? (getWidth() - allArea_.getX()) + 6 : 0;
                g.setColour (mb::ui::text);
                g.setFont (mb::ui::font (10.0f));
                g.drawText (title, 24, 0, juce::jmax (40, getWidth() - 28 - actionW), getHeight(),
                            juce::Justification::centredLeft, false);

                if (showActions)
                {
                    // 文字在 paint 时实时取语言，切换中英文无需重建列表。
                    drawChip (g, allArea_,  mb::tr ("全选", "All"),  hoverAll_);
                    drawChip (g, noneArea_, mb::tr ("清空", "None"), hoverNone_);
                }
            }

            void drawChip (juce::Graphics& g, juce::Rectangle<int> a,
                           const juce::String& text, bool hover)
            {
                const auto ar = a.toFloat();
                g.setColour (hover ? mb::ui::border.withAlpha (0.85f)
                                   : mb::ui::card.withAlpha (0.72f));
                g.fillRoundedRectangle (ar, 3.0f);
                g.setColour (mb::ui::border);
                g.drawRoundedRectangle (ar.reduced (0.5f), 3.0f, 1.0f);
                g.setColour (mb::ui::text);
                g.setFont (mb::ui::font (9.5f));
                g.drawText (text, a.getX(), 0, a.getWidth(), getHeight(),
                            juce::Justification::centred, false);
            }

            void updateHover (juce::Point<int> p)
            {
                const bool a = showActions && allArea_.contains (p);
                const bool n = showActions && noneArea_.contains (p);
                if (a != hoverAll_ || n != hoverNone_)
                {
                    hoverAll_  = a;
                    hoverNone_ = n;
                    repaint();
                }
            }

            void mouseEnter (const juce::MouseEvent& e) override { updateHover (e.getPosition()); }
            void mouseMove  (const juce::MouseEvent& e) override { updateHover (e.getPosition()); }
            void mouseExit  (const juce::MouseEvent&) override { updateHover ({ -1, -1 }); }

            void mouseDown (const juce::MouseEvent& e) override
            {
                layoutAreas();
                const auto p = e.getPosition();
                if (showActions && allArea_.contains (p))
                {
                    if (onGroupAction) onGroupAction (true);
                    return;
                }
                if (showActions && noneArea_.contains (p))
                {
                    if (onGroupAction) onGroupAction (false);
                    return;
                }
                if (onClick) onClick();
            }

        private:
            juce::Rectangle<int> allArea_, noneArea_;
            bool hoverAll_ = false, hoverNone_ = false;
        };

        bool isCollapsed (const juce::String& groupKey) const
        {
            const auto hit = collapsed_.find (groupKey);
            return hit != collapsed_.end() && hit->second;
        }

        void refreshHeaders()
        {
            for (size_t i = 0; i < boxes_.size(); ++i)
            {
                if (! items_[i].header) continue;
                if (auto* h = dynamic_cast<HeaderRow*> (boxes_[i].get()))
                {
                    h->expanded = ! isCollapsed (items_[i].groupKey);
                    h->repaint();
                }
            }
        }

        std::vector<Item> items_;
        std::vector<std::unique_ptr<juce::Component>> boxes_;
        std::map<juce::String, bool> collapsed_;
    };

    static juce::String pairLabel (const juce::StringArray& names, int c, int total)
    {
        auto one = [&names] (int i)
        {
            return i < names.size() && names[i].isNotEmpty()
                 ? names[i] : (mb::S("CH") + juce::String (i + 1));
        };
        if (names.isEmpty() && c == 0) return mb::S("L / R");
        const int k = juce::jmin (2, total - c);
        return k == 1 ? one (c) : one (c) + mb::S(" / ") + one (c + 1);
    }

    static DeviceList::Item placeholder (const juce::String& text,
                                         const juce::String& groupKey, bool input)
    {
        DeviceList::Item it;
        it.header   = true;
        it.group    = text;
        it.groupKey = groupKey;
        it.input    = input;
        it.headerActions = false;
        it.headerCollapsed = false;
        return it;
    }

    const mb::DeviceEntry* masterDevice() const
    {
        const auto type = currentTypeName();
        const auto dev  = selectedDeviceName();
        if (dev.isEmpty()) return nullptr;
        for (const auto& d : engine_.catalog().devices())
            if (d.typeName == type && (d.inputName == dev || d.outputName == dev))
                return &d;
        return nullptr;
    }

    void refreshLists()
    {
        refreshMasterChannels();
        refreshAuxDevices();
        if (getWidth() > 0 && getHeight() > 0) resized();
    }

    /** 主设备通道：左列输入 / 右列输出，默认全部勾选（取消名单之外都勾）。 */
    void refreshMasterChannels()
    {
        juce::StringArray offIn, offOut;
        offIn.addArray  (engine_.catalog().masterOffKeys (true));
        offOut.addArray (engine_.catalog().masterOffKeys (false));

        std::vector<DeviceList::Item> inItems, outItems;
        const auto* d = masterDevice();

        // 目录里同一块声卡的采集端 / 播放端是两个条目（Windows 报的名字不同）。
        // 通道列必须与点「应用」时交给引擎的两侧名字完全一致：统一走 resolveChoice()
        // （配对成功取配对端点；名字认不出时回退到该驱动首个合法端点）。
        // 否则在配对失败的机器上会出现「界面显示无输出通道、引擎却已打开输出」。
        const mb::DeviceEntry* inSrc  = nullptr;
        const mb::DeviceEntry* outSrc = nullptr;
        if (d != nullptr)
        {
            const auto& cat = engine_.catalog();
            const auto resolved = cat.resolveChoice (d->typeName, selectedDeviceName());
            if (resolved.inputName.isNotEmpty())
                inSrc = cat.entryWithInput (d->typeName, resolved.inputName);
            if (resolved.outputName.isNotEmpty())
                outSrc = cat.entryWithOutput (d->typeName, resolved.outputName);
        }

        auto fill = [&] (bool input, int total, const juce::StringArray& names,
                         const juce::String& devName, const mb::DeviceEntry& src,
                         std::vector<DeviceList::Item>& dst)
        {
            const auto base = mb::DeviceCatalog::auxKey (src.typeName, devName);
            for (int c = 0; c < total; c += 2)
            {
                DeviceList::Item it;
                it.key      = base + mb::S("|@") + juce::String (c);
                it.label    = pairLabel (names, c, total);
                it.tooltip  = src.typeShortName + mb::S(" · ") + devName + mb::S(" · ")
                              + pairLabel (names, c, total);
                it.groupKey = input ? mb::S("@master-in") : mb::S("@master-out");
                it.input    = input;
                // 通道对备注：键 = 设备备注键|@对基址|in|out，与设备备注同表。
                it.note         = engine_.catalog().channelNote (src, input, c);
                it.noteKey      = mb::DeviceCatalog::channelNoteKey (src, input, c);
                it.deviceName   = pairLabel (names, c, total);
                it.noteEditable = true;
                it.showIoTag    = false;   // 两列本身就是方向，行首不再重复 IN/OUT
                it.channelNoteRow = true;
                dst.push_back (it);
            }
        };
        if (inSrc != nullptr && inSrc->inputName.isNotEmpty())
            fill (true,  juce::jmax (2, inSrc->inputChannels),  inSrc->inputChannelNames,
                  inSrc->inputName, *inSrc, inItems);
        if (outSrc != nullptr && outSrc->outputName.isNotEmpty())
            fill (false, juce::jmax (2, outSrc->outputChannels), outSrc->outputChannelNames,
                  outSrc->outputName, *outSrc, outItems);

        const bool noMaster = (d == nullptr);
        if (inItems.empty())
            inItems.push_back (placeholder (
                noMaster ? mb::tr ("未选主设备 · 请在下方勾选输入", "No master · tick an input below")
                         : mb::tr ("无输入通道", "No input channels"),
                mb::S("@in-none"), true));
        if (outItems.empty())
            outItems.push_back (placeholder (
                noMaster ? mb::tr ("未选主设备 · 请在下方勾选输出", "No master · tick an output below")
                         : mb::tr ("无输出通道", "No output channels"),
                mb::S("@out-none"), false));

        masterInList_.setItems  (std::move (inItems),  offIn,  offIn,  true);
        masterOutList_.setItems (std::move (outItems), offOut, offOut, true);
    }

    /** 其他设备（辅助设备）：与主设备一样左右两列，左列只列输入设备、
        右列只列输出设备，不含已选主设备，默认不勾选。
        两列的 groupKey 带方向前缀，分组全选 / 清空互不串方向。 */
    void refreshAuxDevices()
    {
        const auto& devs = engine_.catalog().devices();
        juce::StringArray shownIn, shownOut;
        shownIn.addArray  (engine_.catalog().auxShownKeys (true));
        shownOut.addArray (engine_.catalog().auxShownKeys (false));

        const auto curType = currentTypeName();
        // 驱动类型变化（含对话框首次打开）时，重置「其他设备」分组的展开记忆：
        // 同类型组重新排首 + 默认展开，其它组折叠；否则 collapsed_ 会一直保留
        // 旧状态的记忆，出现顶部已是低延迟模式、底部却展开普通 WASAPI 的错位。
        if (curType != lastAuxType_)
        {
            auxInList_.resetGroupStates();
            auxOutList_.resetGroupStates();
            lastAuxType_ = curType;
        }
        const auto masterDev = selectedDeviceName();
        // 主设备实际占用的两端都要从「其它设备」里排除（含配对补上的一侧）。
        const auto masterChoice = engine_.catalog().resolveChoice (curType, masterDev);

        struct Row
        {
            int rank;
            juce::String typeName, shortName;
            DeviceList::Item item;
        };
        std::vector<Row> inRows, outRows;

        for (const auto& d : devs)
        {
            const bool isMaster = d.typeName == curType && ! masterDev.isEmpty()
                               && ((d.inputName.isNotEmpty()
                                    && d.inputName  == masterChoice.inputName)
                                || (d.outputName.isNotEmpty()
                                    && d.outputName == masterChoice.outputName));
            if (isMaster) continue;

            const int rank = d.typeName == curType ? 0 : (d.isAsio ? 1 : 2);
            if (d.inputName.isNotEmpty())
            {
                DeviceList::Item it;
                it.key      = mb::DeviceCatalog::auxKey (d.typeName, d.inputName);
                it.label    = mb::tr ("IN   ", "IN   ") + d.displayName;
                it.tooltip  = d.typeShortName + mb::S(" · ") + d.typeName + mb::S(" · ") + d.inputName;
                it.groupKey = mb::S("auxIn|") + d.typeName;
                it.input    = true;
                it.note         = engine_.catalog().noteFor (d.typeName, d.inputName);
                it.noteKey      = mb::DeviceCatalog::noteKey (d);
                it.deviceName   = d.inputName;      // 完整原名，不再用 shortName 的省略串
                it.noteEditable = true;
                it.showIoTag    = true;
                inRows.push_back ({ rank, d.typeName, d.typeShortName, it });
            }
            if (d.outputName.isNotEmpty())
            {
                DeviceList::Item it;
                it.key      = mb::DeviceCatalog::auxKey (d.typeName, d.outputName);
                it.label    = mb::tr ("OUT  ", "OUT  ") + d.displayName;
                it.tooltip  = d.typeShortName + mb::S(" · ") + d.typeName + mb::S(" · ") + d.outputName;
                it.groupKey = mb::S("auxOut|") + d.typeName;
                it.input    = false;
                it.note         = engine_.catalog().noteFor (d.typeName, d.outputName);
                it.noteKey      = mb::DeviceCatalog::noteKey (d);
                it.deviceName   = d.outputName;     // 完整原名，不再用 shortName 的省略串
                it.noteEditable = true;
                it.showIoTag    = true;
                outRows.push_back ({ rank, d.typeName, d.typeShortName, it });
            }
        }

        /** 单列排序 + 按驱动类型插入分组标题（计数只算本列）。 */
        auto buildColumn = [] (std::vector<Row> rows, const juce::String& noneText,
                               const juce::String& noneKey, bool input)
        {
            std::stable_sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b)
            {
                if (a.rank != b.rank) return a.rank < b.rank;
                if (a.typeName != b.typeName) return a.typeName < b.typeName;
                return a.item.label < b.item.label;
            });

            std::vector<DeviceList::Item> items;
            juce::String lastType;
            int lastRank = -1;
            for (const auto& r : rows)
            {
                if (r.typeName != lastType || r.rank != lastRank)
                {
                    lastType = r.typeName;
                    lastRank = r.rank;
                    int count = 0;
                    for (const auto& x : rows)
                        if (x.typeName == r.typeName && x.rank == r.rank) ++count;

                    DeviceList::Item h;
                    h.header   = true;
                    h.group    = (r.shortName.isEmpty() ? r.typeName : r.shortName)
                                 + mb::S("  (") + juce::String (count) + mb::S(")");
                    h.groupKey = r.item.groupKey;
                    // 当前驱动类型的分组默认展开，其它驱动保持折叠。
                    h.headerCollapsed = (r.rank != 0);
                    items.push_back (h);
                }
                items.push_back (r.item);
            }

            if (items.empty())
                items.push_back (placeholder (noneText, noneKey, input));
            return items;
        };

        auto inItems = buildColumn (std::move (inRows),
                                    mb::tr ("没有输入设备", "No input devices"),
                                    mb::S("@aux-in-none"), true);
        auto outItems = buildColumn (std::move (outRows),
                                     mb::tr ("没有输出设备", "No output devices"),
                                     mb::S("@aux-out-none"), false);

        auxInList_.setItems  (std::move (inItems),  shownIn,  shownIn,  false);
        auxOutList_.setItems (std::move (outItems), shownOut, shownOut, false);
    }

    void loadChoices (mb::DeviceChoice initial)
    {
        const auto& types = engine_.catalog().typeNames();
        typeRawNames_.clear();
        int id = 1;
        for (const auto& t : types)
        {
            typeRawNames_.add (t);   // 真实类型名：设备匹配 / 会话保存 / 备注 key 都用它
            typeBox_.addItem (displayTypeName (t), id++);
        }

        if (! initial.valid || initial.typeName.isEmpty())
        {
            initial.typeName = types.isEmpty() ? juce::String()
                             : (types.contains ("ASIO") ? juce::String ("ASIO") : types[0]);
            const auto ins  = engine_.catalog().deviceNamesForType (initial.typeName, true);
            const auto outs = engine_.catalog().deviceNamesForType (initial.typeName, false);
            initial.inputName  = ins.isEmpty()  ? juce::String() : ins[0];
            initial.outputName = outs.isEmpty() ? juce::String() : outs[0];
        }

        rateBox_.addItemList (engine_.availableSampleRates(), 1);
        bufferBox_.addItemList (engine_.availableBufferSizes(), 1);
        rateBox_.setSelectedItemIndex (initial.sampleRate > 0.0
                                        ? indexOfRate (initial.sampleRate) : 0,
                                       juce::dontSendNotification);
        bufferBox_.setSelectedItemIndex (initial.bufferSize > 0
                                          ? indexOfBuffer (initial.bufferSize) : 0,
                                         juce::dontSendNotification);

        const int typeId = types.indexOf (initial.typeName) + 1;
        typeBox_.setSelectedId (typeId > 0 ? typeId : 1, juce::dontSendNotification);
        const bool noMaster = initial.valid
                           && initial.inputName.isEmpty() && initial.outputName.isEmpty();
        refreshDeviceList (initial.inputName.isNotEmpty() ? initial.inputName : initial.outputName);
        // 上次保存的是「不使用设备」：下拉必须回到该项。否则会误显示第一台
        // 设备，用户一确定就又变回有主设备，内部时钟模式永远进不去。
        if (noMaster)
            deviceBox_.setSelectedId (1, juce::dontSendNotification);
    }

    /** 下拉里显示的是「备注名  设备原名」，用它还原真实设备名（id 1 = 不使用设备）。 */
    juce::String selectedDeviceName() const
    {
        const int idx = deviceBox_.getSelectedId() - 2;
        return idx >= 0 && idx < deviceRawNames_.size()
             ? deviceRawNames_[static_cast<size_t> (idx)] : juce::String();
    }

    //======================================================================
    // 驱动类型下拉：中文界面显示本地化名称，内部一律使用真实类型名。
    // 下拉 getText() 返回的是显示文本，绝不能再用于 deviceNamesForType /
    // resolveChoice / 备注 key 生成，否则中文界面会选不到设备、备注也会对不上。
    //======================================================================
    static juce::String displayTypeName (const juce::String& t)
    {
        if (! mb::isZh()) return t;
        if (t.containsIgnoreCase ("Low Latency"))   return mb::S ("Windows 音频（低延迟模式）");
        if (t.containsIgnoreCase ("Exclusive"))     return mb::S ("Windows 音频（独占模式）");
        if (t.containsIgnoreCase ("DirectSound"))   return mb::S ("DirectSound（直接声音）");
        if (t.containsIgnoreCase ("Windows Audio")) return mb::S ("Windows 音频");
        return t;   // ASIO 等专有名保持原文
    }

    void refreshTypeItems()
    {
        const int keep = typeBox_.getSelectedId();
        typeBox_.clear (juce::dontSendNotification);
        for (int i = 0; i < static_cast<int> (typeRawNames_.size()); ++i)
            typeBox_.addItem (displayTypeName (typeRawNames_[i]), i + 1);
        typeBox_.setSelectedId (keep > 0 ? keep : 1, juce::dontSendNotification);
    }

    juce::String currentTypeName() const
    {
        const int idx = typeBox_.getSelectedId() - 1;
        return idx >= 0 && idx < static_cast<int> (typeRawNames_.size())
             ? typeRawNames_[idx] : juce::String();
    }

    void refreshDeviceList (const juce::String& prefer = {})
    {
        const auto type = currentTypeName();
        // clear() 默认会异步发变更通知 → 触发 onChange → refreshLists() 重建
        // 全部列表，把用户还没点「应用」的勾选状态冲掉（备注保存时也会走到这）。
        deviceBox_.clear (juce::dontSendNotification);
        deviceRawNames_.clear();
        deviceBox_.addItem (mb::tr ("（不使用设备）", "(no device)"), 1);

        juce::StringArray names;
        for (const auto& n : engine_.catalog().deviceNamesForType (type, true))  names.addIfNotAlreadyThere (n);
        for (const auto& n : engine_.catalog().deviceNamesForType (type, false)) names.addIfNotAlreadyThere (n);
        int id = 2;
        for (const auto& n : names)
        {
            deviceBox_.addItem (engine_.catalog().labelledName (type, n), id++);
            deviceRawNames_.add (n);
        }

        const int wanted = prefer.isNotEmpty() ? names.indexOf (prefer) + 2 : 2;
        deviceBox_.setSelectedId (juce::jmax (1, juce::jmin (id - 1, wanted > 1 ? wanted : 2)),
                                  juce::dontSendNotification);
    }

    //==========================================================================
    // 备注：其它设备行的「设备备注」、主设备通道行的「通道备注」都走同一个浮层；
    // 保存后立即写盘并刷新所有显示位置。主设备下拉旁的入口已移除。
    //==========================================================================
    void showNoteEditor (const juce::String& key, const juce::String& deviceName,
                         const juce::String& caption = {}, const juce::String& rightCaption = {})
    {
        if (key.isEmpty()) return;
        if (noteEditor_ == nullptr)
        {
            noteEditor_ = std::make_unique<NoteEditor>();
            noteEditor_->onDone = [this] (bool save, const juce::String& text)
            {
                // 只隐藏不销毁：回调正运行在编辑器自己的子控件里，析构会有风险
                if (noteEditor_ != nullptr) noteEditor_->setVisible (false);
                if (! save) return;
                engine_.catalog().setNote (editingNoteKey_, text);   // 写盘 + 重建端口标签
                refreshDeviceList (selectedDeviceName());             // 主设备下拉文案
                // 就地刷新四列行内备注文字：不重建列表，保住尚未应用的勾选状态。
                auto noteFor = [this] (const juce::String& k)
                {
                    return engine_.catalog().noteForKey (k);
                };
                masterInList_.updateNotes (noteFor);
                masterOutList_.updateNotes (noteFor);
                auxInList_.updateNotes (noteFor);
                auxOutList_.updateNotes (noteFor);
                if (onNoteChanged) onNoteChanged();                   // 节点视图 / 通道条同步
            };
            card_.addAndMakeVisible (noteEditor_.get());
        }
        editingNoteKey_ = key;
        noteEditor_->applyLanguage();
        noteEditor_->start (deviceName, engine_.catalog().noteForKey (key), caption, rightCaption);
        positionNoteEditor();
    }

    void positionNoteEditor()
    {
        if (noteEditor_ == nullptr || ! noteEditor_->isVisible()) return;
        const int w = juce::jlimit (320, 460, card_.getWidth() - 40);
        noteEditor_->setBounds ((card_.getWidth() - w) / 2,
                                juce::jmax (8, (card_.getHeight() - 132) / 2), w, 132);
    }

    int indexOfRate (double rate) const
    {
        int i = 0;
        for (double r : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        {
            ++i;
            if (std::abs (r - rate) < 1.0) return i;
        }
        return 0;
    }

    int indexOfBuffer (int size) const
    {
        int i = 0;
        for (int b : { 64, 96, 128, 192, 256, 384, 512, 768, 1024, 2048 })
        {
            ++i;
            if (b == size) return i;
        }
        return 0;
    }

    void buttonClicked (juce::Button* b) override
    {
        if (b == &cancelBtn_)
        {
            if (onCancel) onCancel();
            return;
        }
        if (b == &okBtn_) tryApply();   // 应用逻辑在 tryApply()（内嵌模式由设置页页脚调用）
    }

    mb::AudioEngine& engine_;
    bool firstRun_;
    Card card_;
    bool plainMode_ = false;   // 设置页内嵌模式：无遮罩、卡片铺满
    juce::Label title_, subtitle_, typeLabel_, deviceLabel_, rateLabel_, bufLabel_,
                masterInLabel_, masterOutLabel_, auxLabel_, auxInLabel_, auxOutLabel_, error_;
    juce::ComboBox typeBox_, deviceBox_, rateBox_, bufferBox_;
    juce::TextButton masterInAllBtn_, masterInNoneBtn_, masterOutAllBtn_, masterOutNoneBtn_;
    juce::StringArray typeRawNames_;                // 下拉 itemId 1..N 对应的真实驱动类型名
    juce::StringArray deviceRawNames_;              // 下拉 itemId 2..N 对应的真实设备名
    juce::String lastAuxType_;                      // 上次刷新「其他设备」时的驱动类型（变化才重置分组展开）
    std::unique_ptr<NoteEditor> noteEditor_;        // 备注编辑浮层（挂在 card_ 上，复用不销毁）
    juce::String editingNoteKey_;
    juce::Viewport masterInView_, masterOutView_, auxInView_, auxOutView_;
    DeviceList masterInList_, masterOutList_, auxInList_, auxOutList_;
    juce::TextButton okBtn_, cancelBtn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSetupOverlay)
};
