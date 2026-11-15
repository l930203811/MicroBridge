#pragma once

#include <JuceHeader.h>

#include <functional>

#include "ChannelIcon.h"
#include "I18n.h"
#include "Theme.h"

//==============================================================================
// 「颜色与图标」统一页面
//
// 上半部：标题「图标」+ 预设图标宫格（首格为「无图标」清除项）
// 下半部：标题「颜色」+ 15 宫格预设色；同页再给自定义 R/G/B、HEX、预览、恢复默认
// 确定回写最终颜色与图标；取消 / 点遮罩保持原状。所有文案经 I18n。
//==============================================================================
class ColourPickerPanel : public juce::Component
{
public:
    std::function<void (juce::Colour, juce::String, juce::String)> onApply;
    std::function<void()> onCancel;

    ColourPickerPanel (juce::Colour initial, juce::Colour fallback,
                        juce::String initialIcon, juce::String initialName)
        : defaultColour_ (fallback.withAlpha (static_cast<juce::uint8> (255)))
    {
        colour_ = initial.withAlpha (static_cast<juce::uint8> (255));

        // 顶部通道名称：可直接改名，随「确定」一并回写
        name_.setText (initialName, false);
        name_.setJustification (juce::Justification::centredLeft);
        name_.setColour (juce::TextEditor::backgroundColourId, mb::ui::slotBg);
        name_.setColour (juce::TextEditor::textColourId, mb::ui::text);
        name_.setColour (juce::TextEditor::outlineColourId, mb::ui::border);
        name_.setColour (juce::TextEditor::focusedOutlineColourId, mb::ui::accent[0]);
        name_.setFont (mb::ui::font (12.0f, true));
        name_.setIndents (6, 0);
        addAndMakeVisible (name_);

        // 图标列表：首格「无图标」(空串)，其后为全部预设
        iconIds_.add (juce::String());
        for (const auto& id : mb::ui::ChannelIcon::allIds())
            iconIds_.add (id);
        currentIcon_ = initialIcon;
        selectedIcon_ = matchIcon (initialIcon);

        auto styleSlider = [this] (juce::Slider& s)
        {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 24);
            s.setRange (0.0, 255.0, 1.0);
            s.setColour (juce::Slider::textBoxTextColourId, mb::ui::text);
            s.setColour (juce::Slider::textBoxBackgroundColourId, mb::ui::slotBg);
            s.setColour (juce::Slider::textBoxOutlineColourId, mb::ui::border);
            s.setColour (juce::Slider::thumbColourId, mb::ui::accent[0]);
            addAndMakeVisible (s);
        };
        styleSlider (r_);
        styleSlider (g_);
        styleSlider (b_);

        auto styleButton = [this] (juce::TextButton& btn)
        {
            btn.setColour (juce::TextButton::buttonColourId, mb::ui::card);
            btn.setColour (juce::TextButton::textColourOffId, mb::ui::text);
            addAndMakeVisible (btn);
        };
        styleButton (okBtn_);
        styleButton (cancelBtn_);
        styleButton (resetBtn_);

        hex_.setJustification (juce::Justification::centred);
        hex_.setColour (juce::TextEditor::backgroundColourId, mb::ui::slotBg);
        hex_.setColour (juce::TextEditor::textColourId, mb::ui::text);
        hex_.setColour (juce::TextEditor::outlineColourId, mb::ui::border);
        hex_.setFont (mb::ui::font (12.0f, true));
        hex_.setInputRestrictions (7, mb::S("#0123456789abcdefABCDEF"));
        addAndMakeVisible (hex_);

        setSlidersFromColour();
        syncPresetSelection();

        const auto onSlide = [this]
        {
            colour_ = juce::Colour::fromRGB (static_cast<juce::uint8> (r_.getValue()),
                                            static_cast<juce::uint8> (g_.getValue()),
                                            static_cast<juce::uint8> (b_.getValue()));
            hex_.setText (hexString(), false);
            syncPresetSelection();
            repaint();
        };
        r_.onValueChange = onSlide;
        g_.onValueChange = onSlide;
        b_.onValueChange = onSlide;

        const auto onHex = [this]
        {
            auto t = hex_.getText().trim();
            if (t.startsWithChar ('#')) t = t.substring (1);
            if (t.length() != 6) { hex_.setText (hexString(), false); return; }
            const auto v = mb::S("#") + t;
            const auto rgb = static_cast<juce::uint32> (v.substring (1).getHexValue32());
            colour_ = juce::Colour::fromRGB (static_cast<juce::uint8> ((rgb >> 16) & 0xff),
                                            static_cast<juce::uint8> ((rgb >> 8) & 0xff),
                                            static_cast<juce::uint8> (rgb & 0xff));
            setSlidersFromColour();
            hex_.setText (hexString(), false);
            syncPresetSelection();
            repaint();
        };
        hex_.onReturnKey = onHex;
        hex_.onFocusLost = onHex;

        const auto apply = [this]
        {
            if (onApply) onApply (colour_, currentIcon_, name_.getText().trim());
        };
        okBtn_.onClick    = apply;
        name_.onReturnKey = apply;
        cancelBtn_.onClick = [this] { if (onCancel) onCancel(); };
        resetBtn_.onClick  = [this] { setColour (defaultColour_); };

        applyLanguage();
    }

    void applyLanguage()
    {
        okBtn_.setButtonText     (mb::tr ("确定", "OK"));
        cancelBtn_.setButtonText (mb::tr ("取消", "Cancel"));
        resetBtn_.setButtonText  (mb::tr ("恢复默认", "Reset to default"));
        repaint();
    }

    /** 移除前先断开回调：TextEditor 析构时可能触发 onFocusLost，
        那时本对象已在析构途中，回调里访问子控件会变成悬空引用。 */
    void dismiss()
    {
        r_.onValueChange = g_.onValueChange = b_.onValueChange = nullptr;
        hex_.onReturnKey = hex_.onFocusLost = nullptr;
        name_.onReturnKey = nullptr;
        okBtn_.onClick = cancelBtn_.onClick = resetBtn_.onClick = nullptr;
    }

    void paint (juce::Graphics& g) override
    {
        card_ = computeCard();
        g.fillAll (juce::Colours::black.withAlpha (0.58f));

        g.setColour (mb::ui::sectionBg);
        g.fillRoundedRectangle (card_, 10.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (card_, 10.0f, 1.0f);

        const int cardX = static_cast<int> (card_.getX());

        // ---- 顶部：通道名称（可编辑）----
        g.setFont (mb::ui::font (10.0f, true));
        g.setColour (mb::ui::subText);
        g.drawText (mb::tr ("名称", "Name"),
                    cardX + 20, static_cast<int> (card_.getY()) + kNameY, 32, 26,
                    juce::Justification::centredLeft, false);

        // ---- 上半部：图标 ----
        g.setFont (mb::ui::font (10.0f, true));
        g.setColour (mb::ui::subText);
        g.drawText (mb::tr ("图标", "Icon"),
                    cardX + 20, static_cast<int> (card_.getY()) + kIconLabelY,
                    kCardW - 40, 16, juce::Justification::centredLeft, false);

        for (int i = 0; i < iconIds_.size(); ++i)
        {
            const auto box = iconRect (i);
            g.setColour (mb::ui::slotBg);
            g.fillRoundedRectangle (box, 7.0f);
            g.setColour (mb::ui::border.withAlpha (0.7f));
            g.drawRoundedRectangle (box, 7.0f, 1.0f);

            const auto id = iconIds_[i];
            if (id.isEmpty())
            {
                // 「无图标」清除项：淡圈 + 「无」字，避免被误认成画坏的斜线图标
                g.setColour (mb::ui::border.withAlpha (0.8f));
                g.drawEllipse (box.reduced (8.0f), 1.0f);
                g.setFont (mb::ui::font (10.0f, true));
                g.setColour (mb::ui::subText);
                g.drawText (mb::tr ("无", "None"), box.toNearestInt(),
                            juce::Justification::centred, false);
            }
            else
            {
                mb::ui::ChannelIcon::draw (g, id, box.reduced (7.0f), mb::ui::text);
            }

            if (i == hoverIcon_)
            {
                g.setColour (juce::Colours::white.withAlpha (0.45f));
                g.drawRoundedRectangle (box.reduced (1.5f), 6.0f, 1.5f);
            }
            if (i == selectedIcon_)
            {
                g.setColour (juce::Colours::white.withAlpha (0.95f));
                g.drawRoundedRectangle (box.reduced (2.0f), 6.0f, 2.0f);
            }
        }

        // ---- 下半部：颜色 ----
        g.setFont (mb::ui::font (10.0f, true));
        g.setColour (mb::ui::subText);
        g.drawText (mb::tr ("颜色", "Colour"),
                    cardX + 20, static_cast<int> (card_.getY()) + kColourLabelY,
                    kCardW - 40, 16, juce::Justification::centredLeft, false);

        for (int i = 0; i < kPresetCount; ++i)
        {
            const auto box = swatchRect (i);
            const auto c   = mb::ui::accent[i];
            g.setColour (c);
            g.fillRoundedRectangle (box, 7.0f);
            g.setColour (juce::Colours::black.withAlpha (0.28f));
            g.drawRoundedRectangle (box, 7.0f, 1.0f);

            if (i == hoverPreset_)
            {
                g.setColour (juce::Colours::white.withAlpha (0.45f));
                g.drawRoundedRectangle (box.reduced (1.5f), 6.0f, 1.5f);
            }
            if (i == selectedPreset_)
            {
                g.setColour (juce::Colours::white.withAlpha (0.95f));
                g.drawRoundedRectangle (box.reduced (2.0f), 6.0f, 2.0f);

                juce::Path tick;
                tick.startNewSubPath (box.getCentreX() - 7.5f, box.getCentreY());
                tick.lineTo         (box.getCentreX() - 1.5f, box.getCentreY() + 6.0f);
                tick.lineTo         (box.getCentreX() + 9.0f, box.getCentreY() - 7.0f);
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.strokePath (tick, juce::PathStrokeType (4.0f));
                g.setColour (c.contrasting (0.9f));
                g.strokePath (tick, juce::PathStrokeType (2.0f));
            }
        }

        // ---- 同页下方的自定义区域 ----
        const float dividerY = card_.getY() + kDividerY;
        g.setColour (mb::ui::border);
        g.fillRect (card_.getX() + 20.0f, dividerY, static_cast<float> (kCardW - 40), 1.0f);

        const auto preview = previewRect();
        g.setColour (colour_);
        g.fillRoundedRectangle (preview, 6.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (preview, 6.0f, 1.0f);
        g.setFont (mb::ui::font (12.0f, true));
        g.setColour (colour_.contrasting (0.75f));
        g.drawText (hexString(), preview.toNearestInt(), juce::Justification::centred, false);

        g.setFont (mb::ui::font (11.0f, true));
        g.setColour (mb::ui::subText);
        const int labelY[] = { kRgbY[0], kRgbY[1], kRgbY[2] };
        const char* labels[] = { "R", "G", "B" };
        for (int i = 0; i < 3; ++i)
            g.drawText (mb::S(labels[i]),
                        cardX + 20,
                        static_cast<int> (card_.getY()) + labelY[i], 22, 28,
                        juce::Justification::centredLeft, false);

        g.setFont (mb::ui::font (10.0f, true));
        g.setColour (mb::ui::subText);
        g.drawText (mb::S("HEX"),
                    cardX + 20,
                    static_cast<int> (card_.getY()) + kHexY, 34, 26,
                    juce::Justification::centredLeft, false);
        g.setFont (mb::ui::font (10.0f));
        g.setColour (mb::ui::faintText);
        g.drawText (mb::tr ("点击色块 / 图标选择预设，或用下方滑块 / HEX 自定义颜色",
                            "Pick a swatch / icon, or customise the colour below"),
                    cardX + 20,
                    static_cast<int> (card_.getY()) + kHintY, kCardW - 40, 16,
                    juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        card_ = computeCard();
        const int x = static_cast<int> (card_.getX());
        const int y = static_cast<int> (card_.getY());
        name_.setBounds (x + 56, y + kNameY, kCardW - 76, 26);
        r_.setBounds (x + 48, y + kRgbY[0], 252, 28);
        g_.setBounds (x + 48, y + kRgbY[1], 252, 28);
        b_.setBounds (x + 48, y + kRgbY[2], 252, 28);
        hex_.setBounds (x + 58, y + kHexY, 110, 26);
        resetBtn_.setBounds (x + 180, y + kHexY, 120, 26);
        cancelBtn_.setBounds (x + 20,  y + kBtnY, 136, 30);
        okBtn_.setBounds     (x + 164, y + kBtnY, 136, 30);
    }

    void mouseMove (const juce::MouseEvent& ev) override
    {
        card_ = computeCard();
        const int hitC = presetAt (ev.position);
        const int hitI = iconAt (ev.position);
        if (hitC != hoverPreset_ || hitI != hoverIcon_)
        {
            hoverPreset_ = hitC;
            hoverIcon_   = hitI;
            repaint();
        }
        setMouseCursor ((hitC >= 0 || hitI >= 0) ? juce::MouseCursor::PointingHandCursor
                                                 : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverPreset_ = -1;
        hoverIcon_   = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        // 长按触控板被翻译成右键时，不要误选色块、更不要误关取色页
        if (ev.mods.isPopupMenu() || ev.mods.isRightButtonDown()) return;
        card_ = computeCard();
        const int hitC = presetAt (ev.position);
        if (hitC >= 0)
        {
            setColour (mb::ui::accent[hitC]);
            return;
        }
        const int hitI = iconAt (ev.position);
        if (hitI >= 0)
        {
            selectedIcon_ = hitI;
            currentIcon_  = iconIds_[hitI];
            repaint();
            return;
        }

        // 点击卡片外的遮罩 = 取消；延后到事件栈退出再触发，避免在
        // mouseDown 回调中销毁自己。
        if (! card_.contains (ev.position))
        {
            juce::Component::SafePointer<ColourPickerPanel> safe (this);
            juce::MessageManager::callAsync ([safe]
            {
                if (auto* self = safe.getComponent())
                    if (self->onCancel) self->onCancel();
            });
        }
    }

private:
    static constexpr int kCardW = 320, kCardH = 592;

    // 顶部通道名称行（可编辑）
    static constexpr int kNameY = 12;

    // 图标宫格：5 列（首格无图标 + 19 预设 = 5×4）
    static constexpr int kIconCols = 5;
    static constexpr float kIconBox = 32.0f, kIconGap = 8.0f;
    static constexpr int kIconLabelY = 46;
    static constexpr float kIconTop = 66.0f;

    // 颜色宫格：5 列 x 3 行 = 15 色
    static constexpr int kPresetCount = 15;
    static constexpr int kCols = 5;
    static constexpr float kSwatch = 32.0f, kGap = 8.0f;
    static constexpr int kColourLabelY = 230;
    static constexpr float kColourTop = 250.0f;

    // 自定义区纵向坐标（相对卡片顶）
    static constexpr int kDividerY = 370;
    static constexpr int kRgbY[3] = { 414, 444, 474 };
    static constexpr int kHexY = 504;
    static constexpr int kHintY = 532;
    static constexpr int kBtnY = 556;

    juce::Rectangle<float> computeCard() const
    {
        return { static_cast<float> ((getWidth()  - kCardW) / 2),
                 static_cast<float> (juce::jmax (0, (getHeight() - kCardH) / 2)),
                 static_cast<float> (kCardW), static_cast<float> (kCardH) };
    }

    juce::Rectangle<float> swatchRect (int i) const
    {
        const float totalW = static_cast<float> (kCols) * kSwatch
                           + static_cast<float> (kCols - 1) * kGap;
        const float x0 = card_.getX() + (static_cast<float> (kCardW) - totalW) / 2.0f;
        const float y0 = card_.getY() + kColourTop;
        const float col = static_cast<float> (i % kCols);
        const float row = static_cast<float> (i / kCols);
        return { x0 + col * (kSwatch + kGap), y0 + row * (kSwatch + kGap),
                 kSwatch, kSwatch };
    }

    juce::Rectangle<float> iconRect (int i) const
    {
        const float totalW = static_cast<float> (kIconCols) * kIconBox
                           + static_cast<float> (kIconCols - 1) * kIconGap;
        const float x0 = card_.getX() + (static_cast<float> (kCardW) - totalW) / 2.0f;
        const float y0 = card_.getY() + kIconTop;
        const float col = static_cast<float> (i % kIconCols);
        const float row = static_cast<float> (i / kIconCols);
        return { x0 + col * (kIconBox + kIconGap), y0 + row * (kIconBox + kIconGap),
                 kIconBox, kIconBox };
    }

    int presetAt (juce::Point<float> p) const
    {
        for (int i = 0; i < kPresetCount; ++i)
            if (swatchRect (i).contains (p)) return i;
        return -1;
    }

    int iconAt (juce::Point<float> p) const
    {
        for (int i = 0; i < iconIds_.size(); ++i)
            if (iconRect (i).contains (p)) return i;
        return -1;
    }

    int matchIcon (const juce::String& id) const
    {
        for (int i = 0; i < iconIds_.size(); ++i)
            if (iconIds_[i] == id) return i;
        return 0;   // 未知 / 空 → 「无图标」
    }

    juce::Rectangle<float> previewRect() const
    {
        return { card_.getX() + 20.0f, card_.getY() + 378.0f,
                 static_cast<float> (kCardW - 40), 26.0f };
    }

    juce::String hexString() const
    {
        return mb::S("#") + juce::String::toHexString (
            (static_cast<int> (colour_.getRed()) << 16)
          | (static_cast<int> (colour_.getGreen()) << 8)
          |  static_cast<int> (colour_.getBlue())).paddedLeft ('0', 6).toUpperCase();
    }

    void setColour (juce::Colour c)
    {
        colour_ = c.withAlpha (static_cast<juce::uint8> (255));
        setSlidersFromColour();
        syncPresetSelection();
        repaint();
    }

    void setSlidersFromColour()
    {
        r_.setValue (colour_.getRed(),   juce::dontSendNotification);
        g_.setValue (colour_.getGreen(), juce::dontSendNotification);
        b_.setValue (colour_.getBlue(),  juce::dontSendNotification);
        hex_.setText (hexString(), false);
    }

    /** 当前颜色恰好等于某个预设时，给对应色块画选中圈。 */
    void syncPresetSelection()
    {
        selectedPreset_ = -1;
        for (int i = 0; i < kPresetCount; ++i)
            if (mb::ui::accent[i].getARGB() == colour_.getARGB())
            {
                selectedPreset_ = i;
                break;
            }
    }

    juce::Colour colour_, defaultColour_;
    juce::StringArray iconIds_;
    juce::String currentIcon_;
    juce::Rectangle<float> card_;
    int selectedPreset_ = -1, hoverPreset_ = -1;
    int selectedIcon_ = 0, hoverIcon_ = -1;
    juce::Slider r_, g_, b_;
    juce::TextEditor hex_;
    juce::TextEditor name_;     // 顶部通道名称（可改名）
    juce::TextButton okBtn_, cancelBtn_, resetBtn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColourPickerPanel)
};
