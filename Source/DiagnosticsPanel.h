#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "ChannelRecorder.h"
#include "I18n.h"
#include "Theme.h"

//==============================================================================
// 设备与诊断面板（浮层，参考主流机架的设备探测报告 / 录音目录管理）
//
// · 列出全部驱动条目：驱动类型、主/辅、可用性、错误、输入/输出端口。
// · 引擎状态：采样率 / 缓冲 / 延迟、辅助设备数、欠载计数。
// · 录音目录：更换目录、打开目录。
//==============================================================================
class DiagnosticsPanel : public juce::Component
{
public:
    std::function<void()> onClose;
    std::function<void (const juce::String&)> onMessage;

    DiagnosticsPanel (mb::AudioEngine& engine, mb::ChannelRecorder& recorder)
        : engine_ (engine), recorder_ (recorder)
    {
        addAndMakeVisible (copyBtn_);
        copyBtn_.setButtonText (mb::tr ("复制报告", "Copy report"));
        copyBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
        copyBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::text);
        copyBtn_.onClick = [this]
        {
            juce::SystemClipboard::copyTextToClipboard (buildReport());
            if (onMessage) onMessage (mb::tr ("诊断报告已复制到剪贴板", "Report copied to clipboard"));
        };

        addAndMakeVisible (dirBtn_);
        dirBtn_.setButtonText (mb::tr ("更换录音目录", "Recording folder…"));
        dirBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
        dirBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::text);
        dirBtn_.onClick = [this]
        {
            chooser_ = std::make_unique<juce::FileChooser> (
                mb::tr ("选择录音目录", "Choose recording folder"), recorder_.directory());
            chooser_->launchAsync (juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::FileChooser& fc)
                {
                    const auto dir = fc.getResult();
                    if (dir.isDirectory())
                    {
                        recorder_.setDirectory (dir);
                        if (onMessage)
                            onMessage (mb::tr ("录音目录：", "Recording folder: ") + dir.getFullPathName());
                    }
                });
        };

        addAndMakeVisible (openBtn_);
        openBtn_.setButtonText (mb::tr ("打开录音目录", "Open folder"));
        openBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::accent[0]);
        openBtn_.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        openBtn_.onClick = [this] { recorder_.directory().startAsProcess(); };

        addAndMakeVisible (closeBtn_);
        closeBtn_.setButtonText (mb::tr ("关闭", "Close"));
        closeBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::card);
        closeBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::text);
        closeBtn_.onClick = [this] { if (onClose) onClose(); };
    }

    void applyLanguage()
    {
        copyBtn_.setButtonText (mb::tr ("复制报告", "Copy report"));
        dirBtn_.setButtonText  (mb::tr ("更换录音目录", "Recording folder…"));
        openBtn_.setButtonText (mb::tr ("打开录音目录", "Open folder"));
        closeBtn_.setButtonText(mb::tr ("关闭", "Close"));
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.35f));

        const auto card = cardBounds();
        g.setColour (mb::ui::card);
        g.fillRoundedRectangle (card, 10.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (card, 10.0f, 1.2f);

        g.setFont (mb::ui::font (14.0f, true));
        g.setColour (mb::ui::text);
        g.drawText (mb::tr ("设备与诊断", "Devices & Diagnostics"),
                    card.getX() + 18, card.getY() + 10, 400, 30,
                    juce::Justification::centredLeft, false);

        // 设备列表
        g.setFont (mb::ui::font (8.8f));
        int y = card.getY() + 48;
        const auto& devs = engine_.catalog().devices();
        const auto& inPorts  = engine_.catalog().inputPorts();
        const auto& outPorts = engine_.catalog().outputPorts();

        for (size_t di = 0; di < devs.size(); ++di)
        {
            const auto& d = devs[di];
            if (y > card.getBottom() - 150) break;

            int in = 0, out = 0;
            for (const auto& p : inPorts)  if (p.deviceIndex == static_cast<int> (di)) ++in;
            for (const auto& p : outPorts) if (p.deviceIndex == static_cast<int> (di)) ++out;

            const bool failed = ! d.available || d.error.isNotEmpty()
                             || engine_.deviceError (static_cast<int> (di)).isNotEmpty();
            g.setColour (d.isMaster ? mb::ui::meterGreen
                         : (failed ? mb::ui::meterRed : mb::ui::subText));
            g.fillEllipse (static_cast<float> (card.getX() + 20), static_cast<float> (y + 4), 7.0f, 7.0f);

            g.setFont (mb::ui::font (9.2f, d.isMaster));
            g.setColour (d.isMaster ? mb::ui::text : mb::ui::subText);
            juce::String line = d.typeName + mb::S("  ·  ") + d.displayName
                              + mb::S("   ") + juce::String (in) + mb::S(" in / ")
                              + juce::String (out) + mb::S(" out");
            g.drawText (line, card.getX() + 34, y, card.getWidth() - 200, 15,
                        juce::Justification::centredLeft, false);

            juce::String err = d.error.isNotEmpty() ? d.error : engine_.deviceError (static_cast<int> (di));
            if (err.isNotEmpty())
            {
                g.setFont (mb::ui::font (8.0f));
                g.setColour (mb::ui::meterRed);
                g.drawText (err, card.getX() + 34, y + 14, card.getWidth() - 200, 13,
                            juce::Justification::centredLeft, false);
            }
            y += err.isNotEmpty() ? 30 : 19;
        }

        // 引擎状态 + 录音目录
        g.setFont (mb::ui::font (8.8f));
        g.setColour (mb::ui::subText);
        const double sr = engine_.sampleRate();
        const int bs = engine_.bufferSizeSamples();
        const double lat = sr > 0.0 ? bs * 1000.0 / sr : 0.0;
        g.drawText (mb::tr ("引擎", "Engine") + mb::S(":  ")
                        + engine_.driverTypeName() + mb::S("  ·  ")
                        + juce::String (sr / 1000.0, 1) + mb::S(" kHz  ·  ")
                        + juce::String (bs) + mb::S(" f  ·  ")
                        + juce::String (lat, 1) + mb::S(" ms  ·  ")
                        + mb::tr ("辅助设备", "aux") + mb::S(" ") + juce::String (engine_.auxDeviceCount())
                        + mb::S("  ·  ") + mb::tr ("欠载", "underrun") + mb::S(" ")
                        + juce::String (engine_.auxUnderruns()),
                    card.getX() + 18, card.getBottom() - 118, card.getWidth() - 36, 16,
                    juce::Justification::centredLeft, false);
        g.drawText (mb::tr ("录音目录", "Recordings") + mb::S(":  ") + recorder_.directory().getFullPathName(),
                    card.getX() + 18, card.getBottom() - 98, card.getWidth() - 36, 16,
                    juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const auto card = cardBounds().toNearestInt();
        const int btnY = card.getBottom() - 44;
        copyBtn_.setBounds (card.getX() + 16,  btnY, 92, 28);
        dirBtn_.setBounds  (card.getX() + 116, btnY, 120, 28);
        openBtn_.setBounds (card.getX() + 244, btnY, 110, 28);
        closeBtn_.setBounds (card.getRight() - 108, btnY, 92, 28);
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        // 长按触控板 = 右键时不要误关诊断页
        if (ev.mods.isPopupMenu() || ev.mods.isRightButtonDown()) return;
        if (! cardBounds().contains (ev.position) && onClose) onClose();
    }

private:
    juce::Rectangle<float> cardBounds() const
    {
        const int cardH = 560;
        const int cardW = juce::jmin (760, juce::jmax (560, getWidth() - 80));
        const float x = static_cast<float> ((getWidth()  - cardW) / 2);
        const float y = static_cast<float> ((getHeight() - cardH) / 2);
        return { x, y, static_cast<float> (cardW), static_cast<float> (cardH) };
    }

    juce::String buildReport() const
    {
        juce::StringArray lines;
        lines.add (mb::S("MicroBridge device report"));
        lines.add (juce::String());

        const double sr = engine_.sampleRate();
        const int bs = engine_.bufferSizeSamples();
        lines.add (mb::tr ("引擎", "Engine") + mb::S(": ") + engine_.driverTypeName()
                   + mb::S(" / ") + juce::String (sr, 0) + mb::S(" Hz / ")
                   + juce::String (bs) + mb::S(" f / ")
                   + juce::String (sr > 0.0 ? bs * 1000.0 / sr : 0.0, 1) + mb::S(" ms"));
        lines.add (mb::tr ("辅助设备", "Aux devices") + mb::S(": ")
                   + juce::String (engine_.auxDeviceCount())
                   + mb::S(" / ") + mb::tr ("欠载", "underruns") + mb::S(": ")
                   + juce::String (engine_.auxUnderruns()));
        lines.add (juce::String());

        const auto& devs = engine_.catalog().devices();
        for (const auto& d : devs)
        {
            lines.add ((d.isMaster ? mb::S("[MASTER] ") : mb::S("         "))
                       + d.typeName + mb::S("  |  ") + d.displayName
                       + (d.available ? mb::S("") : mb::S("  [UNAVAILABLE]")));
            if (d.error.isNotEmpty())
                lines.add (mb::S("           error: ") + d.error);
        }
        return lines.joinIntoString (mb::S("\r\n"));
    }

    mb::AudioEngine& engine_;
    mb::ChannelRecorder& recorder_;

    juce::TextButton copyBtn_, dirBtn_, openBtn_, closeBtn_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE (DiagnosticsPanel)
};
