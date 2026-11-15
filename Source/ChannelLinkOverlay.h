#pragma once

#include <JuceHeader.h>

#include "ChannelLinks.h"
#include "I18n.h"
#include "ModernMenu.h"
#include "Theme.h"

//==============================================================================
// 「快捷键与链接」弹窗：动作 × 类型字母 × 序号 三行网格选择（链接），
// 外加快捷键录入按钮（可选；不录入 = 只链接不触发快捷键）。
// 顶部显示当前通道名称；同码（同步组）/ 同字母不同码（互斥组）的通道
// 分别列出，方便识别通道之间的联动关系。
// 分组名单与快捷键继承均为实时计算：弹窗内切换字母/序号立即重算，
// 通道名等其他通道数据通过 linkFor 回调实时读取，不使用打开时的快照。
// 录入的快捷键由宿主注册为系统级全局热键（本应用不在前台也生效）；
// 录入期间通过 onHotkeyCapture 让宿主临时抑制钩子，避免吞键 / 误触发。
//==============================================================================
class ChannelLinkOverlay : public juce::Component
{
public:
    std::function<void (int ch, mb::links::LinkSpec)> onApply;
    std::function<void()> onCancel;
    // 实时数据源（构造后由宿主接线）：通道总数 / 读取任意通道当前链接
    std::function<int()> channelCount;
    std::function<mb::links::LinkSpec (int)> linkFor;
    // 录入模式开关（宿主据此临时抑制系统级钩子，防止吞掉正在录入的按键）
    std::function<void (bool)> onHotkeyCapture;

    ChannelLinkOverlay (int ch, mb::links::LinkSpec current)
        : channel_ (ch), spec_ (current)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        title_.setText    (mb::tr ("快捷键与链接", "Hotkeys & Links"), juce::dontSendNotification);
        title_.setFont    (mb::ui::font (18.0f, true));
        title_.setColour  (juce::Label::textColourId, mb::ui::accent[0]);
        title_.setJustificationType (juce::Justification::centred);
        card_.addAndMakeVisible (title_);

        // 当前通道名称（高亮加粗，方便确认正在配置谁）
        name_.setText (mb::tr ("当前通道：", "Channel: ")
                           + mb::S("CH") + juce::String (ch + 1)
                           + mb::S("  ") + mb::channelDisplayName (ch),
                       juce::dontSendNotification);
        name_.setFont (mb::ui::font (13.5f, true));
        name_.setColour (juce::Label::textColourId, mb::ui::text);
        name_.setJustificationType (juce::Justification::centred);
        card_.addAndMakeVisible (name_);

        // 分组提示（前缀高亮，组内通道名按各通道颜色渲染，refreshGroups 实时填写）
        card_.addAndMakeVisible (sameCodeLabel_);
        card_.addAndMakeVisible (mutexLabel_);

        // 大号链接码 + 状态提示
        codeLabel_.setText (mb::links::codeString (spec_).isEmpty()
                                ? mb::S("—")
                                : mb::links::codeString (spec_),
                            juce::dontSendNotification);
        codeLabel_.setFont (mb::ui::font (32.0f, true));
        codeLabel_.setColour (juce::Label::textColourId, mb::ui::accent[0]);
        codeLabel_.setJustificationType (juce::Justification::centred);
        card_.addAndMakeVisible (codeLabel_);

        hintLabel_.setFont (mb::ui::font (12.0f));
        hintLabel_.setColour (juce::Label::textColourId, mb::ui::subText);
        hintLabel_.setJustificationType (juce::Justification::centred);
        card_.addAndMakeVisible (hintLabel_);

        // 动作说明：随动作行选择实时变化（含「可仅链接 / 仅快捷键」提示）
        descLabel_.setFont (mb::ui::font (10.5f));
        descLabel_.setColour (juce::Label::textColourId, mb::ui::subText);
        descLabel_.setJustificationType (juce::Justification::centred);
        card_.addAndMakeVisible (descLabel_);

        // 三行分段按钮
        auto styleSeg = [this] (juce::TextButton& b, int row, int col)
        {
            b.onClick = [this, row, col] { pick (row, col); };
            card_.addAndMakeVisible (b);
        };
        for (int a = 0; a < mb::links::kActionCount; ++a) styleSeg (actionBtns_[a], 0, a);
        for (int l = 0; l < mb::links::kLetterCount; ++l) styleSeg (letterBtns_[l], 1, l);
        for (int i = 0; i < mb::links::kIndexCount;  ++i) styleSeg (indexBtns_[i],  2, i);

        // 快捷键录入按钮（可选；Del/Backspace 清除，Esc 取消录入）
        hotkeyBtn_.onClick = [this]
        {
            setCapturing (! capturing_);
            refreshHotkeyDisplay();
            if (capturing_) grabKeyboardFocus();
        };
        hotkeyBtn_.setWantsKeyboardFocus (false);
        card_.addAndMakeVisible (hotkeyBtn_);

        okBtn_.setButtonText (mb::tr ("确定", "Apply"));
        cancelBtn_.setButtonText (mb::tr ("取消", "Cancel"));
        clearBtn_.setButtonText (mb::tr ("清空链接", "Clear link"));
        auto styleBottom = [this] (juce::TextButton& b, std::function<void()> fn)
        {
            b.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
            b.setColour (juce::TextButton::textColourOffId, mb::ui::text);
            b.onClick = std::move (fn);
            card_.addAndMakeVisible (b);
        };
        styleBottom (okBtn_,    [this] { if (onApply) onApply (channel_, spec_); });
        styleBottom (cancelBtn_, [this] { if (onCancel) onCancel(); });
        styleBottom (clearBtn_,  [this]
        {
            spec_.index = 0;
            if (onApply) onApply (channel_, spec_);   // 清空即解除绑定
        });

        addAndMakeVisible (card_);
        refreshLook();
        refreshHotkeyDisplay();
        refreshHint();
        refreshGroups();     // 分组名单 + 同码快捷键继承（实时计算）
    }

    void paint (juce::Graphics& g) override { g.fillAll (mb::ui::background); }

    void resized() override
    {
        const int cardW = juce::jlimit (380, 460, getWidth() - 60);
        const int cardH = 412;
        card_.setBounds ((getWidth() - cardW) / 2, (getHeight() - cardH) / 2, cardW, cardH);

        title_.setBounds        (20, 14, cardW - 40, 26);
        name_.setBounds         (20, 42, cardW - 40, 22);
        codeLabel_.setBounds    (20, 66, cardW - 40, 40);
        sameCodeLabel_.setBounds (20, 108, cardW - 40, 18);
        mutexLabel_.setBounds   (20, 126, cardW - 40, 18);
        hintLabel_.setBounds    (20, 146, cardW - 40, 18);

        const int segW = (cardW - 40 - 4 * 6) / mb::links::kActionCount;
        for (int i = 0; i < mb::links::kActionCount; ++i)
            actionBtns_[i].setBounds (20 + i * (segW + 6), 172, segW, 30);
        for (int i = 0; i < mb::links::kLetterCount; ++i)
            letterBtns_[i].setBounds (20 + i * (segW + 6), 210, segW, 30);
        for (int i = 0; i < mb::links::kIndexCount; ++i)
            indexBtns_[i].setBounds  (20 + i * (segW + 6), 248, segW, 30);

        descLabel_.setBounds (20, 282, cardW - 40, 30);

        hotkeyBtn_.setBounds (20, 316, cardW - 40, 30);

        okBtn_.setBounds    (cardW / 2 - 170, cardH - 52, 100, 30);
        cancelBtn_.setBounds (cardW / 2 - 60,  cardH - 52, 100, 30);
        clearBtn_.setBounds (cardW / 2 + 50,  cardH - 52, 110, 30);
    }

    /** 快捷键录入：仅在录入状态下拦截按键，否则冒泡给全局快捷键钩子。 */
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (! capturing_) return false;

        if (key == juce::KeyPress::escapeKey)            // Esc = 取消录入
        {
            setCapturing (false);
            refreshHotkeyDisplay();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::deleteKey
            || key.getKeyCode() == juce::KeyPress::backspaceKey)   // Del/退格 = 清除
        {
            spec_.key = 0;
            spec_.mods = 0;
            setCapturing (false);
            hotkeyInherited_ = false;
            refreshHotkeyDisplay();
            refreshHint();
            return true;
        }

        spec_.key  = key.getKeyCode();
        spec_.mods = key.getModifiers().getRawFlags();
        setCapturing (false);
        hotkeyInherited_ = false;                    // 用户自设快捷键，不再是继承态
        refreshHotkeyDisplay();
        refreshHint();
        return true;
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

    /** 多彩单行文本：分组提示前缀用高亮色，组内各通道名用其通道颜色，
        便于一眼识别互斥/同步关系里都有谁。 */
    class GroupLine : public juce::Component
    {
    public:
        void set (juce::AttributedString s) { text_ = std::move (s); repaint(); }

        void paint (juce::Graphics& g) override
        {
            if (text_.getNumAttributes() == 0) return;
            juce::TextLayout tl;
            tl.createLayout (text_, (float) getWidth());
            tl.draw (g, getLocalBounds().toFloat());
        }

    private:
        juce::AttributedString text_;
    };

    /** 行/列选择：动作行直接改 action；字母行改 letter；序号行改 index。 */
    void pick (int row, int col)
    {
        // 继承自原同码组的快捷键随换码失效：先清除，再由 refreshGroups
        // 按新组重新继承（组内有人设键才继承）；用户自设键保留不受影响
        if (hotkeyInherited_)
        {
            spec_.key = 0;
            spec_.mods = 0;
            hotkeyInherited_ = false;
            refreshHotkeyDisplay();
        }
        if (row == 0)      spec_.action = col;
        else if (row == 1) spec_.letter = col;
        else               spec_.index  = col + 1;
        refreshLook();
        refreshHint();
        refreshGroups();     // 切换链接码即时重算 同码/互斥 分组与快捷键继承
    }

    void refreshLook()
    {
        for (int a = 0; a < mb::links::kActionCount; ++a)
            styleSegment (actionBtns_[a], spec_.action == a, mb::links::actionName (a));
        for (int l = 0; l < mb::links::kLetterCount; ++l)
            styleSegment (letterBtns_[l], spec_.letter == l, mb::links::letterName (l));
        for (int i = 0; i < mb::links::kIndexCount; ++i)
            styleSegment (indexBtns_[i], spec_.index == i + 1, juce::String (i + 1));

        codeLabel_.setText (spec_.bound() ? mb::links::codeString (spec_) : mb::S("—"),
                            juce::dontSendNotification);
        descLabel_.setText (mb::links::actionDesc (spec_.action), juce::dontSendNotification);
    }

    void styleSegment (juce::TextButton& b, bool selected, const juce::String& text)
    {
        b.setButtonText (text);
        b.setColour (juce::TextButton::buttonColourId,
                     selected ? mb::ui::accent[0] : mb::ui::sectionBg);
        b.setColour (juce::TextButton::textColourOffId,
                     selected ? juce::Colours::white : mb::ui::text);
    }

    /** 切换录入态并通知宿主抑制 / 恢复系统级全局钩子。 */
    void setCapturing (bool on)
    {
        if (capturing_ == on) return;
        capturing_ = on;
        if (onHotkeyCapture) onHotkeyCapture (on);
    }

    void refreshHotkeyDisplay()
    {
        hotkeyBtn_.setColour (juce::TextButton::buttonColourId,
                              capturing_ ? mb::ui::accent[0] : mb::ui::sectionBg);
        hotkeyBtn_.setColour (juce::TextButton::textColourOffId,
                              capturing_ ? juce::Colours::white : mb::ui::text);
        hotkeyBtn_.setButtonText (capturing_
            ? mb::tr ("请按下快捷键…（系统级全局，Esc 取消 / Del 清除）",
                      "Press keys… (system-wide; Esc cancel / Del clear)")
            : (mb::links::hotkeyText (spec_).isNotEmpty()
                   ? mb::tr ("快捷键（全局）  ", "Hotkey (global):  ") + mb::links::hotkeyText (spec_)
                         + (hotkeyInherited_ ? mb::tr ("（同码共享，修改后全组同步）",
                                                       " (shared by code — edits sync to group)")
                                             : juce::String())
                   : mb::tr ("快捷键未设置（点击录入，全局生效，可只链接）",
                             "No hotkey — click to record (global, link only)")));
    }

    void refreshHint()
    {
        // 快捷键冲突检测（实时）：其他通道已用相同按键且不属于同码共享组。
        // 同字母不同码（互斥组）内冲突最危险 —— 按下会互相顶掉 / 同时翻转，
        // 与无关通道冲突则会一起被触发，两种都要明确提示用户。
        if (spec_.hasHotkey() && channelCount && linkFor)
        {
            juce::StringArray mutexOthers, others;
            for (int c = 0; c < channelCount(); ++c)
            {
                if (c == channel_) continue;
                const auto l = linkFor (c);
                if (! l.hasHotkey() || l.key != spec_.key || l.mods != spec_.mods)
                    continue;
                if (l.bound() && l.action == spec_.action
                    && l.letter == spec_.letter && l.index == spec_.index)
                    continue;                        // 同码组共享快捷键 = 正常，不算冲突
                const auto n = mb::S ("CH") + juce::String (c + 1)
                             + mb::S (" ") + mb::channelDisplayName (c);
                if (l.bound() && l.action == spec_.action && l.letter == spec_.letter)
                    mutexOthers.add (n);             // 同字母不同码 = 互斥组内冲突
                else
                    others.add (n);                  // 无关通道冲突
            }
            if (! mutexOthers.isEmpty() || ! others.isEmpty())
            {
                juce::String txt = mb::tr ("⚠ 快捷键冲突：", "⚠ Hotkey conflict: ");
                if (! mutexOthers.isEmpty())
                    txt += mb::tr ("互斥组通道 ", "mutex channels ")
                           + mutexOthers.joinIntoString (mb::S ("、"));
                if (! others.isEmpty())
                {
                    if (! mutexOthers.isEmpty()) txt += mb::S ("；");
                    txt += others.joinIntoString (mb::S ("、"));
                }
                txt += mb::tr (" 共用此键，按下会同时触发",
                               " share this key — pressing triggers them together");
                hintLabel_.setText (txt, juce::dontSendNotification);
                hintLabel_.setColour (juce::Label::textColourId, mb::ui::meterRed);
                return;
            }
        }

        hintLabel_.setColour (juce::Label::textColourId, mb::ui::subText);
        if (! spec_.bound())
        {
            hintLabel_.setText (mb::tr ("未选择序号 = 仅保存动作与类型，暂不参与联动",
                                        "Pick an index to enable linking"),
                                juce::dontSendNotification);
            return;
        }
        hintLabel_.setText (spec_.hasHotkey()
                                ? mb::tr ("按快捷键可整组翻转开关状态",
                                          "Hotkey toggles the whole group")
                                : mb::tr ("同码同步 · 同字母互斥 · 无快捷键",
                                          "Sync by code, mutex by letter, no hotkey"),
                            juce::dontSendNotification);
    }

    /** 实时重算同码（同步）/ 同字母不同码（互斥）分组名单，
        并做同码组快捷键继承：当前码组内已有快捷键而本通道未设时自动取用。
        每次 pick（切换动作/字母/序号）与构造时调用 —— 弹窗内换码立即反映。
        渲染：前缀与占位提示用高亮文字色，组内通道名用各自通道颜色。 */
    void refreshGroups()
    {
        juce::Array<int> syncCh, mutexCh;
        juce::StringArray mutexCodes;
        const bool ready = spec_.bound() && channelCount && linkFor;

        if (ready)
            for (int c = 0; c < channelCount(); ++c)
            {
                if (c == channel_) continue;
                const auto l = linkFor (c);
                if (! l.bound() || l.action != spec_.action || l.letter != spec_.letter)
                    continue;
                if (l.index == spec_.index)
                    syncCh.add (c);                                   // 同字母同码：同步组
                else
                {
                    mutexCh.add (c);
                    mutexCodes.add (mb::links::codeString (l));
                }
            }

        // —— 同码同步组（前缀高亮，通道名按通道色）——
        {
            juce::AttributedString s;
            if (! spec_.bound())
                s.append (mb::tr ("同码同步组：—（先选择序号绑定链接码）",
                                  "Sync group (same code): — (pick an index first)"),
                          mb::ui::font (12.0f), mb::ui::text);
            else if (syncCh.isEmpty())
                s.append (mb::tr ("同码同步组：暂无其他通道",
                                  "Sync group (same code): none yet"),
                          mb::ui::font (12.0f), mb::ui::text);
            else
            {
                s.append (mb::tr ("同码同步组：", "Sync group (same code): "),
                          mb::ui::font (12.0f, true), mb::ui::text);
                for (int i = 0; i < syncCh.size(); ++i)
                {
                    const int c = syncCh[i];
                    if (i > 0) s.append (mb::S ("、"), mb::ui::font (12.0f), mb::ui::subText);
                    s.append (mb::S ("CH") + juce::String (c + 1)
                              + mb::S (" ") + mb::channelDisplayName (c),
                              mb::ui::font (12.0f, true), mb::ui::stripColour (c));
                }
            }
            s.setJustification (juce::Justification::centred);
            sameCodeLabel_.set (std::move (s));
        }

        // —— 同字母互斥组（前缀高亮，通道名+码按通道色）——
        {
            juce::AttributedString s;
            if (! spec_.bound())
                s.append (mb::tr ("同字母互斥组：—（先选择序号绑定链接码）",
                                  "Mutex group (same letter): — (pick an index first)"),
                          mb::ui::font (12.0f), mb::ui::text);
            else if (mutexCh.isEmpty())
                s.append (mb::tr ("同字母互斥组：暂无其他通道",
                                  "Mutex group (same letter): none yet"),
                          mb::ui::font (12.0f), mb::ui::text);
            else
            {
                s.append (mb::tr ("同字母互斥组：", "Mutex group (same letter): "),
                          mb::ui::font (12.0f, true), mb::ui::text);
                for (int i = 0; i < mutexCh.size(); ++i)
                {
                    const int c = mutexCh[i];
                    if (i > 0) s.append (mb::S ("、"), mb::ui::font (12.0f), mb::ui::subText);
                    s.append (mb::S ("CH") + juce::String (c + 1)
                              + mb::S (" ") + mb::channelDisplayName (c)
                              + mb::S (" (") + mutexCodes[i] + mb::S (")"),
                              mb::ui::font (12.0f, true), mb::ui::stripColour (c));
                }
            }
            s.setJustification (juce::Justification::centred);
            mutexLabel_.set (std::move (s));
        }

        // 同码组快捷键实时继承：本通道未设快捷键 → 取组内已有快捷键作默认
        if (ready && ! spec_.hasHotkey())
            for (int c = 0; c < channelCount(); ++c)
            {
                if (c == channel_) continue;
                const auto l = linkFor (c);
                if (l.bound() && l.hasHotkey()
                    && l.action == spec_.action && l.letter == spec_.letter
                    && l.index == spec_.index)
                {
                    spec_.key = l.key;
                    spec_.mods = l.mods;
                    hotkeyInherited_ = true;
                    refreshHotkeyDisplay();
                    break;
                }
            }
        if (! spec_.bound())
            hotkeyInherited_ = false;

        refreshHint();       // 继承/换码后快捷键可能新产生冲突，同步刷新警示
    }

    int channel_;
    mb::links::LinkSpec spec_;
    bool capturing_ = false;
    bool hotkeyInherited_ = false;               // 快捷键来自同码组默认（修改后全组同步）
    Card card_;
    juce::Label title_, name_, codeLabel_, hintLabel_, descLabel_;
    GroupLine sameCodeLabel_, mutexLabel_;       // 分组提示：前缀高亮 + 通道名按通道色
    juce::TextButton hotkeyBtn_;
    juce::TextButton actionBtns_[mb::links::kActionCount];
    juce::TextButton letterBtns_[mb::links::kLetterCount];
    juce::TextButton indexBtns_ [mb::links::kIndexCount];
    juce::TextButton okBtn_, cancelBtn_, clearBtn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelLinkOverlay)
};
