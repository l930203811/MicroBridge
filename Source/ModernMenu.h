#pragma once

#include <JuceHeader.h>

#include "Theme.h"

//==============================================================================
// 原生风格标题栏按钮：无背景，扁平字形（– □ ✕），颜色随主题文字色，
// 最大化后显示「还原」双框字形。替代 JUCE V4 的彩色圆形按钮。
//==============================================================================
class NativeWindowButton : public juce::Button
{
public:
    explicit NativeWindowButton (int type) : juce::Button ({}), type_ (type)
    {
        setWantsKeyboardFocus (false);
    }

    /** 内置插件浮层折叠态：最小化键改画「展开」箭头，再点一次还原。
        主窗口标题栏的按钮不调用本方法，保持原有减号字形。 */
    void setExpandArrow (bool on)
    {
        if (expandArrow_ != on) { expandArrow_ = on; repaint(); }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        juce::ignoreUnused (down);
        const auto b = getLocalBounds().toFloat();
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float u = juce::jmin (b.getWidth(), b.getHeight()) * 0.155f; // 字形半径
        const float t = 1.2f;                                              // 线宽
        g.setColour (mb::ui::text.withAlpha (highlighted ? 1.0f : 0.72f));

        if (type_ == juce::DocumentWindow::minimiseButton)
        {
            if (expandArrow_)
            {
                // 向下的 V 形展开箭头；线宽 / 颜色与减号字形同源，随主题。
                g.drawLine (cx - u, cy - u * 0.35f, cx, cy + u * 0.45f, t);
                g.drawLine (cx, cy + u * 0.45f, cx + u, cy - u * 0.35f, t);
            }
            else
                g.drawLine (cx - u, cy, cx + u, cy, t);
            return;
        }
        if (type_ == juce::DocumentWindow::closeButton)
        {
            g.drawLine (cx - u, cy - u, cx + u, cy + u, t);
            g.drawLine (cx + u, cy - u, cx - u, cy + u, t);
            return;
        }

        // 最大化 / 还原
        const float s = u * 2.0f;
        juce::Path p;
        p.addRectangle (cx - u, cy - u, s, s);
        g.strokePath (p, juce::PathStrokeType (t));
        if (getToggleState())                        // 还原：补一个错位前框
        {
            juce::Path front;
            front.addRectangle (cx - u + s * 0.3f, cy - u + s * 0.3f, s, s);
            g.setColour (mb::ui::topBar);            // 前框先用底色遮住后框内段
            g.fillPath (front);
            g.setColour (mb::ui::text.withAlpha (highlighted ? 1.0f : 0.72f));
            g.strokePath (front, juce::PathStrokeType (t));
        }
    }

private:
    int type_;
    bool expandArrow_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NativeWindowButton)
};

//==============================================================================
// 全局默认 LookAndFeel：只接管「弹出菜单外观」与「可交互控件光标」，其余
// 全部沿用 LookAndFeel_V4（改造前 JUCE 默认就是 V4），因此不影响按钮、推子、
// 对话框等既有外观。在 MicroBridgeApplication::initialise 里安装一次即可，
// 应用内所有 PopupMenu（含 ComboBox 下拉、AlertWindow 菜单）统一生效。
//==============================================================================
class MicroBridgeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MicroBridgeLookAndFeel() { refreshThemeColours(); }

    /** 主题切换后重刷缓存颜色（弹出菜单色在 setColour 时被缓存，不会自动跟随）。 */
    void refreshThemeColours()
    {
        setColour (juce::PopupMenu::backgroundColourId,            mb::ui::card);
        setColour (juce::PopupMenu::textColourId,                  mb::ui::text);
        setColour (juce::PopupMenu::headerTextColourId,            mb::ui::accent[0]);
        setColour (juce::PopupMenu::highlightedBackgroundColourId,
                   mb::ui::isDarkTheme() ? juce::Colour (0xff343b46)
                                         : juce::Colour (0xffb9cdf5));
        setColour (juce::PopupMenu::highlightedTextColourId,
                   mb::ui::isDarkTheme() ? juce::Colours::white : mb::ui::text);
    }

    juce::Font getPopupMenuFont() override { return mb::ui::font (11.5f); }

    /** 自绘标题栏（窗口改用非原生边框后启用）：
        底色=主题 topBar，左侧状态文字，右侧留出最小化/最大化/关闭按钮区。
        颜色直接读全局主题，深浅切换后重绘即跟随。 */
    void drawDocumentWindowTitleBar (juce::DocumentWindow& window, juce::Graphics& g,
                                     int w, int h, int titleSpaceX, int titleSpaceW,
                                     const juce::Image* icon, bool) override
    {
        juce::ignoreUnused (titleSpaceX);
        g.fillAll (mb::ui::topBar);
        g.setColour (mb::ui::divider);
        g.fillRect (0, h - 1, w, 1);                      // 与内容的分隔线

        // 左上角应用图标 + 文字右移
        int textX = titleSpaceX;
        if (icon != nullptr && icon->isValid())
        {
            const int size = juce::jmin (h - 14, 22);
            g.drawImage (*icon, 8, (h - size) / 2, size, size,
                         0, 0, icon->getWidth(), icon->getHeight());
            textX = 8 + size + 7;
        }

        g.setColour (mb::ui::text);
        g.setFont (mb::ui::font (12.5f));
        g.drawText (window.getName(), textX, 0,
                    juce::jmax (40, titleSpaceX + titleSpaceW - textX), h,
                    juce::Justification::centredLeft);
    }

    /** 标题栏按钮：原生风格的扁平字形按钮（无边框背景）。 */
    juce::Button* createDocumentWindowButton (int buttonType) override
    {
        return new NativeWindowButton (buttonType);
    }

    /** 按钮排布与原生窗口一致：贴顶贴右、全标题栏高、无缝相邻（关闭在最右）。 */
    void positionDocumentWindowButtons (juce::DocumentWindow&,
                                        int titleBarX, int titleBarY,
                                        int titleBarW, int titleBarH,
                                        juce::Button* minimise, juce::Button* maximise,
                                        juce::Button* close, bool) override
    {
        const int bw = 46;
        int x = titleBarX + titleBarW;
        if (close != nullptr)    { x -= bw; close->setBounds    (x, titleBarY, bw, titleBarH); }
        if (maximise != nullptr) { x -= bw; maximise->setBounds (x, titleBarY, bw, titleBarH); }
        if (minimise != nullptr) { x -= bw; minimise->setBounds (x, titleBarY, bw, titleBarH); }
    }

    int getPopupMenuBorderSize() override { return 8; }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
        g.setColour (juce::Colours::white.withAlpha (0.045f));
        g.fillRect (0, 0, width, 1);
        g.setColour (mb::ui::border);
        g.drawRect (0, 0, width, height, 1);
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillRect (0, height - 1, width, 1);
    }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int standardItemHeight,
                                    int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth  = 40;
            idealHeight = 9;
            return;
        }
        idealHeight = standardItemHeight > 0
                    ? juce::jmax (30, standardItemHeight) : 30;
        idealWidth  = juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 88;
    }

    void getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& text,
                                                        int,
                                                        int& idealWidth, int& idealHeight,
                                                        const juce::PopupMenu::Options&) override
    {
        idealHeight = 30;
        idealWidth  = juce::GlyphArrangement::getStringWidthInt (
                          getPopupMenuFont().withHeight (13.0f).boldened(), text) + 48;
    }

    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area,
                                     const juce::String& sectionName) override
    {
        // 标题不高亮：无色条、无强调色，仅比菜单项字号大一号，避免看起来像按钮
        auto r = area.reduced (12, 0);
        g.setFont (getPopupMenuFont().withHeight (13.0f).boldened());
        g.setColour (mb::ui::text);
        g.drawFittedText (sectionName, r, juce::Justification::centredLeft, 1);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                            bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText,
                            const juce::Drawable* icon,
                            const juce::Colour* textColourToUse) override
    {
        if (isSeparator)
        {
            g.setColour (mb::ui::divider);
            g.fillRect (area.reduced (12, 0).withHeight (1).withY (area.getCentreY()));
            return;
        }

        auto r = area.reduced (5, 1);
        if (isHighlighted && isActive)
        {
            g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
            g.fillRoundedRectangle (r.toFloat(), 5.0f);
            // 悬停项左侧蓝色指示条
            g.setColour (mb::ui::accent[0]);
            g.fillRoundedRectangle (static_cast<float> (r.getX() + 2),
                                    static_cast<float> (r.getY() + 7),
                                    2.5f, static_cast<float> (r.getHeight() - 14), 1.2f);
        }

        juce::Colour textColour = textColourToUse != nullptr
                                ? *textColourToUse
                                : findColour (juce::PopupMenu::textColourId);
        if (! isActive)                  textColour = textColour.withAlpha (0.34f);
        else if (isHighlighted)          textColour = findColour (juce::PopupMenu::highlightedTextColourId);

        // 左侧 22px 图标 / 勾选槽
        auto iconArea = r.removeFromLeft (22);
        if (isTicked)
        {
            const auto box = juce::Rectangle<float> (
                static_cast<float> (iconArea.getCentreX()) - 7.0f,
                static_cast<float> (area.getCentreY()) - 7.0f, 14.0f, 14.0f);
            g.setColour (mb::ui::accent[0].withAlpha (isActive ? 0.92f : 0.3f));
            g.fillRoundedRectangle (box, 4.0f);
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 3.0f, box.getCentreY() + 0.5f);
            tick.lineTo           (box.getX() + 5.8f, box.getBottom() - 4.0f);
            tick.lineTo           (box.getRight() - 2.8f, box.getY() + 4.0f);
            g.setColour (juce::Colours::white.withAlpha (isActive ? 1.0f : 0.5f));
            g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }
        else if (icon != nullptr)
        {
            icon->drawWithin (g, iconArea.reduced (3).toFloat(),
                              juce::RectanglePlacement::centred,
                              isActive ? 1.0f : 0.35f);
        }

        // 子菜单箭头
        if (hasSubMenu)
        {
            r.removeFromRight (14);
            const float cx = static_cast<float> (r.getRight()) + 6.0f;
            const float cy = static_cast<float> (area.getCentreY());
            juce::Path p;
            p.startNewSubPath (cx - 2.0f, cy - 4.0f);
            p.lineTo          (cx + 2.5f, cy);
            p.lineTo          (cx - 2.0f, cy + 4.0f);
            g.setColour (textColour.withAlpha (0.75f));
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }

        if (shortcutKeyText.isNotEmpty())
        {
            auto srFont = getPopupMenuFont().withHeight (9.0f);
            auto sr = r.removeFromRight (
                juce::GlyphArrangement::getStringWidthInt (srFont, shortcutKeyText) + 12);
            g.setFont (srFont);
            g.setColour (textColour.withAlpha (0.6f));
            g.drawFittedText (shortcutKeyText, sr, juce::Justification::centredRight, 1);
        }

        g.setFont (getPopupMenuFont());
        g.setColour (textColour);
        g.drawFittedText (text, r, juce::Justification::centredLeft, 1);
    }

    //==========================================================================
    // JUCE 原生 Button / ComboBox / Slider 默认都返回箭头光标，与「可点击」直觉
    // 相悖。这里是全局唯一的游标决策点，统一改为手型；组件若已通过
    // setMouseCursor 显式指定（拖动换位、调高、文本输入等），优先尊重组件自身。
    juce::MouseCursor getMouseCursorFor (juce::Component& c) override
    {
        if (c.getMouseCursor() != juce::MouseCursor (juce::MouseCursor::NormalCursor))
            return juce::LookAndFeel_V4::getMouseCursorFor (c);

        if (dynamic_cast<juce::Button*> (&c) != nullptr
            || dynamic_cast<juce::ComboBox*> (&c) != nullptr
            || dynamic_cast<juce::Slider*> (&c) != nullptr)
            return juce::MouseCursor (juce::MouseCursor::PointingHandCursor);

        return juce::LookAndFeel_V4::getMouseCursorFor (c);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicroBridgeLookAndFeel)
};

namespace mb
{
    /** 进程级唯一的全局 LnF（Main.cpp 安装；主题切换后由此刷新缓存色）。 */
    inline MicroBridgeLookAndFeel& sharedLookAndFeel()
    {
        static MicroBridgeLookAndFeel lf;
        return lf;
    }

    namespace ui
    {
        /** 取宿主窗口的内容组件（菜单挂载点）。挂进窗口内容组件后，弹出菜单的
            定位会被钳制在内容区内，过高时自动滚动，物理上不可能超出窗口顶部。 */
        inline juce::Component* hostParentFor (const juce::Component& anchor)
        {
            juce::Component* parent = anchor.getTopLevelComponent();
            if (auto* doc = dynamic_cast<juce::DocumentWindow*> (parent))
                if (auto* content = doc->getContentComponent())
                    parent = content;
            return parent;
        }

        /** 右键/按钮弹出菜单的统一入口：锚定在组件上，并把菜单窗口挂到宿主窗口
            内容组件内（withParentComponent）。JUCE 默认只对屏幕做钳制，通道靠上
            且菜单较高时会向上翻出窗口顶部、盖住自绘标题栏。 */
        inline juce::PopupMenu::Options menuOptionsFor (juce::Component& anchor)
        {
            return juce::PopupMenu::Options()
                       .withTargetComponent (anchor)
                       .withParentComponent (hostParentFor (anchor));
        }
    }
}
