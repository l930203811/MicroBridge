#pragma once

#include <JuceHeader.h>

#include "I18n.h"
#include "Theme.h"

//==============================================================================
// 首次启动欢迎页：语言选择（跟随系统 / 简体中文 / English）。
// 选完即写盘并进入设备设置页（DeviceSetupOverlay），两步合为首次开箱流程。
// 「跟随系统」解析规则：系统语言是中文 → 中文，其余（含其他语种）一律英文。
//==============================================================================
class WelcomeOverlay : public juce::Component
{
public:
    /** 参数 = 语言模式：0 跟随系统 / 1 中文 / 2 English。 */
    std::function<void (int)> onLanguagePicked;

    WelcomeOverlay()
    {
        addAndMakeVisible (card_);

        title_.setText (mb::tr ("欢迎使用微音桥", "Welcome to MicroBridge"),
                        juce::dontSendNotification);
        title_.setFont (mb::ui::font (17.0f, true));
        title_.setColour (juce::Label::textColourId, mb::ui::text);
        card_.addAndMakeVisible (title_);

        subtitle_.setText (mb::tr ("先选择界面语言，下一步再设置音频设备。",
                                   "Pick a language first, then set up your audio device."),
                           juce::dontSendNotification);
        subtitle_.setFont (mb::ui::font (10.5f));
        subtitle_.setColour (juce::Label::textColourId, mb::ui::subText);
        card_.addAndMakeVisible (subtitle_);

        auto makeBtn = [this] (juce::TextButton& b, int mode, const juce::String& main)
        {
            b.setButtonText (main);
            b.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
            b.setColour (juce::TextButton::textColourOffId, mb::ui::text);
            b.onClick = [this, mode] { if (onLanguagePicked) onLanguagePicked (mode); };
            card_.addAndMakeVisible (b);
        };

        makeBtn (sysBtn_, 0, mb::tr ("跟随系统", "Follow system"));
        makeBtn (zhBtn_, 1, mb::S("简体中文"));
        makeBtn (enBtn_, 2, mb::S("English"));

        // 「跟随系统」解析结果说明：小字挂在按钮正下方
        sysHint_.setText (mb::tr ("系统为中文时显示中文，否则显示英文 · 当前：",
                                  "Chinese if the system is, English otherwise · now: ")
                          + mb::systemLanguageName(),
                          juce::dontSendNotification);
        sysHint_.setFont (mb::ui::font (8.5f));
        sysHint_.setColour (juce::Label::textColourId, mb::ui::subText);
        sysHint_.setJustificationType (juce::Justification::centred);
        card_.addAndMakeVisible (sysHint_);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (mb::ui::text.withAlpha (0.42f));
    }

    void resized() override
    {
        const int cardW = juce::jlimit (380, 480, getWidth() - 60);
        const int cardH = 268;
        card_.setBounds ((getWidth() - cardW) / 2, (getHeight() - cardH) / 2, cardW, cardH);

        title_.setBounds    (24, 20, cardW - 48, 26);
        subtitle_.setBounds (24, 50, cardW - 48, 18);

        const int x = 24, w = cardW - 48;
        sysBtn_.setBounds  (x, 80, w, 32);
        sysHint_.setBounds (x, 113, w, 14);
        zhBtn_.setBounds   (x, 136, w, 32);
        enBtn_.setBounds   (x, 176, w, 32);
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

    Card card_;
    juce::Label title_, subtitle_, sysHint_;
    juce::TextButton sysBtn_, zhBtn_, enBtn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WelcomeOverlay)
};
