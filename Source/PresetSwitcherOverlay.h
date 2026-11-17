#pragma once

#include <JuceHeader.h>

#include "I18n.h"
#include "Theme.h"

//==============================================================================
// 预设切换浮层：抽屉「切换预设」入口 / 首次启动选预设 共用。
// 结构：顶部「＋ 新建预设」→「我的预设」（用户导入 / 保存的 .mbpreset）
//       →「内置预设」（「默认预设」一套官方配线）。
// 点击任一预设回调外层载入；× 关闭仅供继续后续流程（首启 = 进入设备设置）。
//==============================================================================
class PresetSwitcherOverlay : public juce::Component
{
public:
    std::function<void()> onNewPreset;
    std::function<void()> onImport;                       // 导入预设（原抽屉入口迁入）
    std::function<void (const juce::File&)> onPickUser;   // 用户预设文件
    std::function<void()> onPickBuiltin;                  // 默认预设（唯一内置）
    std::function<void()> onClose;

    PresetSwitcherOverlay (const juce::Array<juce::File>& userPresets)
        : presets_ (userPresets)
    {
        addAndMakeVisible (card_);

        title_.setText (mb::tr ("切换预设", "Switch preset"),
                        juce::dontSendNotification);
        title_.setFont (mb::ui::font (15.0f, true));
        title_.setColour (juce::Label::textColourId, mb::ui::text);
        card_.addAndMakeVisible (title_);

        hint_.setText (mb::tr ("载入预设会替换当前全部通道与连线。",
                               "Loading a preset replaces all channels and routes."),
                       juce::dontSendNotification);
        hint_.setFont (mb::ui::font (9.5f));
        hint_.setColour (juce::Label::textColourId, mb::ui::subText);
        card_.addAndMakeVisible (hint_);

        closeBtn_.setButtonText (mb::S("×"));
        closeBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
        closeBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::subText);
        closeBtn_.onClick = [this] { if (onClose) onClose(); };
        card_.addAndMakeVisible (closeBtn_);

        newBtn_.setButtonText (mb::tr ("新建预设", "New preset"));
        newBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
        newBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::text);
        newBtn_.onClick = [this] { if (onNewPreset) onNewPreset(); };
        card_.addAndMakeVisible (newBtn_);

        importBtn_.setButtonText (mb::tr ("导入预设", "Import preset"));
        importBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
        importBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::text);
        importBtn_.onClick = [this] { if (onImport) onImport(); };
        card_.addAndMakeVisible (importBtn_);

        vp_.setViewedComponent (&rows_, false);
        card_.addAndMakeVisible (vp_);

        mineHeader_.setText (mb::tr ("我的预设", "My presets"),
                             juce::dontSendNotification);
        builtinHeader_.setText (mb::tr ("内置预设", "Built-in presets"),
                                juce::dontSendNotification);
        for (auto* l : { &mineHeader_, &builtinHeader_ })
        {
            l->setFont (mb::ui::font (10.0f, true));
            l->setColour (juce::Label::textColourId, mb::ui::subText);
            rows_.addAndMakeVisible (l);
        }

        if (presets_.isEmpty())
        {
            mineEmpty_.setText (mb::tr ("（暂无 · 导入或保存预设后出现在这里）",
                                        "(none · import or save presets first)"),
                                juce::dontSendNotification);
            mineEmpty_.setFont (mb::ui::font (9.0f));
            mineEmpty_.setColour (juce::Label::textColourId, mb::ui::faintText);
            rows_.addAndMakeVisible (mineEmpty_);
        }
        for (const auto& f : presets_)
            addRow (f.getFileNameWithoutExtension(), [this, f] { if (onPickUser) onPickUser (f); });

        addRow (mb::tr ("默认预设", "Default preset"), [this] { if (onPickBuiltin) onPickBuiltin(); });
    }

    ~PresetSwitcherOverlay() override { vp_.setViewedComponent (nullptr, false); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (mb::ui::text.withAlpha (0.42f));
    }

    void resized() override
    {
        const int cardW = juce::jlimit (253, 307, getWidth() - 60);   // 比初版收窄约 1/3
        const int headH = 150;                       // 标题 + 提示 + 新建/导入按钮
        const int listH = rowsHeight (cardW - 52);
        const int cardH = juce::jlimit (300, getHeight() - 32, headH + listH + 14);
        card_.setBounds ((getWidth() - cardW) / 2, (getHeight() - cardH) / 2, cardW, cardH);

        title_.setBounds   (20, 16, cardW - 72, 24);
        closeBtn_.setBounds (cardW - 40, 14, 26, 26);
        hint_.setBounds    (20, 44, cardW - 40, 16);
        // 顶栏两按钮与下方预设行同宽同列（x=12 / w=cardW-52），高度统一 30
        newBtn_.setBounds  (12, 68, cardW - 52, 30);
        importBtn_.setBounds (12, 104, cardW - 52, 30);
        vp_.setBounds      (12, 140, cardW - 24, cardH - 152);
        rows_.setBounds (0, 0, cardW - 32, listH);
        layoutRows (cardW - 52);
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

    void addRow (const juce::String& text, std::function<void()> fn)
    {
        auto b = std::make_unique<juce::TextButton>();
        b->setButtonText (text);
        b->setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
        b->setColour (juce::TextButton::textColourOffId, mb::ui::text);
        b->onClick = std::move (fn);
        rows_.addAndMakeVisible (b.get());
        rowButtons_.push_back (std::move (b));
    }

    int builtinCount() const { return 1; }

    int rowsHeight (int w) const
    {
        int h = 22;                                  // 「我的预设」标题
        h += presets_.isEmpty() ? 20 : static_cast<int> (presets_.size()) * 34;
        h += 26;                                     // 「内置预设」标题 + 间距
        h += 1 * 36;
        return h + 6;
    }

    void layoutRows (int w)
    {
        int y = 0;
        mineHeader_.setBounds (4, y, w - 8, 18); y += 22;
        if (presets_.isEmpty())
        {
            mineEmpty_.setBounds (4, y, w - 8, 18); y += 20;
        }
        for (size_t i = 0; i < rowButtons_.size() - builtinCount(); ++i)
        {
            rowButtons_[i]->setBounds (0, y, w, 30); y += 34;
        }
        builtinHeader_.setBounds (4, y + 2, w - 8, 18); y += 26;
        for (size_t i = rowButtons_.size() - builtinCount(); i < rowButtons_.size(); ++i)
        {
            rowButtons_[i]->setBounds (0, y, w, 30); y += 36;
        }
    }

    Card card_;
    juce::Label title_, hint_, mineHeader_, builtinHeader_, mineEmpty_;
    juce::TextButton newBtn_, importBtn_, closeBtn_;
    juce::Viewport vp_;
    juce::Component rows_;
    std::vector<std::unique_ptr<juce::TextButton>> rowButtons_;
    juce::Array<juce::File> presets_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetSwitcherOverlay)
};
