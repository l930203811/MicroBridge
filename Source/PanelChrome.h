#pragma once

// 内置插件浮动面板公共骨架（FxPanel / MediaPlayerPanel 共用）
//
// · 卡片是主窗口内的自绘浮层（juce::Component 子控件），不是原生窗口：
//   绘制与鼠标事件都限于主窗口客户区，因此不会拖出主窗口、也不进任务栏。
// · 按住标题栏空白处可拖动；右下角手柄 / 右边缘 / 下边缘可自由调整大小
//   （不小于 minCardSize()、不大于主窗口剩余空间；折叠 / 最大化时禁用）。
// · 最小化 = 折叠为标题条，此时最小化键变成「展开」箭头，再点一次还原；
//   折叠条按 34px 的实际高度限位，可在主窗口内任意位置拖动。
// · 双击标题栏或点最大化键同样可还原。
// · 无论折叠还是展开，卡片都常驻主窗口上层；卡片以外区域的鼠标透传回
//   主窗口（hitTest 返回 false），主窗口其它位置照常操作，且点击卡片外
//   不会关闭面板；只有点卡片右上角的 × 才关闭。
// · 尺寸 / 位置 / 折叠 / 最大化由 cardState() / applyCardState() 序列化，
//   宿主负责写 settings.json，重开面板与重启应用都能恢复。

#include <JuceHeader.h>

#include "ModernMenu.h"

class FloatingCardPanel : public juce::Component
{
public:
    static constexpr int kTitleH = 34;

    std::function<void()> onClose;
    std::function<void()> onStateChanged;   // 尺寸 / 位置 / 折叠 / 最大化变化后触发（宿主写盘）

    FloatingCardPanel()
    {
        chrome_.addAndMakeVisible (minBtn_);
        chrome_.addAndMakeVisible (maxBtn_);
        chrome_.addAndMakeVisible (closeBtn_);

        minBtn_.onClick = [this] { setMinimised (! minimised_); };
        maxBtn_.onClick = [this] { setMaximised (! maximised_); };
        closeBtn_.onClick = [this] { if (onClose) onClose(); };

        addAndMakeVisible (chrome_);

        // 右下角缩放手柄：自己拦截鼠标并置顶，避免被铺满卡片的内容控件挡住；
        // 坐标换算回本组件后交给统一的 resize 逻辑。
        grip_.setAlwaysOnTop (true);
        grip_.setMouseCursor (juce::MouseCursor::BottomRightCornerResizeCursor);
        auto toOwner = [this] (const juce::MouseEvent& ev)
        {
            return ev.getEventRelativeTo (this);
        };
        grip_.onDown = [this, toOwner] (const juce::MouseEvent& ev)
        {
            beginResize (toOwner (ev).position.toFloat());
        };
        grip_.onDrag = [this, toOwner] (const juce::MouseEvent& ev)
        {
            updateResize (toOwner (ev).position.toFloat());
        };
        grip_.onUp = [this] (const juce::MouseEvent&) { endResize(); };
        addAndMakeVisible (grip_);
    }

    bool minimised() const noexcept { return minimised_; }
    bool maximised() const noexcept { return maximised_; }

    /** 宿主在其它浮层关闭 / 视图切换后调用，把面板重新抬回主窗口上层。 */
    void bringToFront() { toFront (false); }

    /** 可持久化的窗口状态（尺寸 / 位置 / 折叠 / 最大化）。 */
    struct CardState
    {
        bool hasSize = false, hasPos = false;
        double w = 0.0, h = 0.0, x = 0.0, y = 0.0;
        bool minimised = false, maximised = false;
    };

    CardState cardState() const
    {
        CardState s;
        s.hasSize = sizeSet_;
        s.w = sizeSet_ ? static_cast<double> (userSize_.x) : 0.0;
        s.h = sizeSet_ ? static_cast<double> (userSize_.y) : 0.0;
        s.hasPos = posSet_;
        s.x = posSet_ ? static_cast<double> (pos_.x) : 0.0;
        s.y = posSet_ ? static_cast<double> (pos_.y) : 0.0;
        s.minimised = minimised_;
        s.maximised = maximised_;
        return s;
    }

    /** 恢复上次状态；越界尺寸 / 位置仍由 cardBounds() 钳制。
        刻意不触发 onStateChanged，避免「恢复」又写一次盘。 */
    void applyCardState (const CardState& s)
    {
        sizeSet_ = false;
        posSet_  = false;
        if (s.hasSize && s.w > 0.0 && s.h > 0.0)
        {
            const float W = static_cast<float> (getWidth());
            const float H = static_cast<float> (getHeight());
            auto mn = minCardSize();
            mn.x = juce::jmin (mn.x, juce::jmax (1.0f, W));
            mn.y = juce::jmin (mn.y, juce::jmax (1.0f, H));
            userSize_ = { juce::jlimit (mn.x, juce::jmax (mn.x, W), static_cast<float> (s.w)),
                          juce::jlimit (mn.y, juce::jmax (mn.y, H), static_cast<float> (s.h)) };
            sizeSet_ = true;
        }
        if (s.hasPos)
        {
            pos_ = { static_cast<float> (s.x), static_cast<float> (s.y) };
            posSet_ = true;
        }
        maximised_ = s.maximised;
        minimised_ = s.minimised && ! maximised_;
        maxBtn_.setToggleState (maximised_, juce::dontSendNotification);
        minBtn_.setExpandArrow (minimised_);
        drag_ = DragNone;
        toFront (false);                       // 折叠 / 展开都常驻主窗口上层
        resized();
        repaint();
    }

    //==================================================================
    // 派生类接口
    //==================================================================
    /** 正常态卡片的默认宽高（位置 / 收起 / 最大化由基类统一处理）。 */
    virtual juce::Point<float> normalSize() const = 0;

    /** 折叠态卡片尺寸：宽度 <= 0 表示沿用正常态宽度（默认只收成标题条）；
        派生类可返回紧凑条尺寸（如音频播放器折叠成走带三键小条）。 */
    virtual juce::Point<float> collapsedSize() const { return { 0.0f, (float) kTitleH }; }

    /** 折叠态右上角保留的窗口按钮数（1=只留最小化/还原键，2=再去掉关闭，3=全保留）；
        展开态恒为 3。派生类可精简（如音频播放器折叠时只留还原键）。 */
    virtual int collapsedChromeButtons() const { return 3; }

    /** 卡片矩形：派生类 paint / resized 统一使用。
        用户拖过大小后优先用 userSize_；折叠态用 collapsedSize()（可覆盖），
        并且 y 限位按「折叠后的高度」计算，折叠条才能拖到窗口任意位置。 */
    juce::Rectangle<float> cardBounds() const
    {
        const float W = (float) getWidth(), H = (float) getHeight();
        if (maximised_) return { 0.0f, 0.0f, W, H };
        auto sz = sizeSet_ ? userSize_ : normalSize();
        if (minimised_)
        {
            const auto cs = collapsedSize();
            if (cs.x > 0.0f) sz.x = cs.x;
            sz.y = cs.y;
        }
        sz.x = juce::jmin (sz.x, W);
        sz.y = juce::jmin (sz.y, H);
        const float h = sz.y;                       // 折叠态 = collapsedSize().y（上方已写入）
        auto pos = posSet_ ? pos_
                           : juce::Point<float> ((W - sz.x) * 0.5f,
                                                 juce::jlimit (0.0f, juce::jmax (0.0f, H - h), 22.0f));
        pos.x = juce::jlimit (0.0f, juce::jmax (0.0f, W - sz.x), pos.x);
        pos.y = juce::jlimit (0.0f, juce::jmax (0.0f, H - h), pos.y);
        return { pos.x, pos.y, sz.x, h };
    }

    /** 派生类 resized() 里调用：摆放窗口按钮与缩放手柄；
        collapsed 时把内容藏到屏外只留标题条。 */
    void layoutChrome (const juce::Rectangle<float>& card, bool collapsed)
    {
        constexpr int bw = 34, bh = 26;
        if (collapsed)
            for (auto* c : getChildren())
                if (c != &chrome_) c->setBounds ({ -3000, -3000, 0, 0 });

        // 折叠态按派生类精简窗口按钮数（靠右排列，右缘 6px 边距）；展开态恒为 3
        const int n = collapsed ? juce::jlimit (1, 3, collapsedChromeButtons()) : 3;
        chrome_.setBounds (juce::Rectangle<int> ((int) card.getRight() - 6 - bw * n,
                                                 (int) card.getY() + 4, bw * n, bh));
        chrome_.toFront (false);
        minBtn_  .setBounds (0, 0, bw, bh);
        maxBtn_  .setBounds (bw, 0, bw, bh);
        closeBtn_.setBounds (bw * 2, 0, bw, bh);
        maxBtn_  .setVisible (n >= 2);      // 折叠态只留还原键时，最大化 / 关闭隐藏
        closeBtn_.setVisible (n >= 3);

        const bool showGrip = ! collapsed && ! maximised_;
        grip_.setVisible (showGrip);
        if (showGrip)
        {
            constexpr int g = 16;
            grip_.setBounds ((int) card.getRight() - g - 2,
                             (int) card.getBottom() - g - 2, g, g);
        }
    }

protected:
    /** 可缩放到的最小尺寸；派生类按内容覆盖。 */
    virtual juce::Point<float> minCardSize() const { return { 420.0f, 260.0f }; }

    void setMinimised (bool m)
    {
        if (m && maximised_)
        {
            maximised_ = false;
            maxBtn_.setToggleState (false, juce::dontSendNotification);
        }
        minimised_ = m;
        minBtn_.setExpandArrow (m);     // 折叠态显示展开箭头，再点一次还原
        drag_ = DragNone;
        toFront (false);                // 折叠 / 展开都保持在主窗口上层
        resized(); repaint();
        notifyStateChanged();
    }

    void setMaximised (bool mx)
    {
        maximised_ = mx;
        if (mx) minimised_ = false;
        maxBtn_.setToggleState (mx, juce::dontSendNotification);   // 还原态双框字形
        minBtn_.setExpandArrow (minimised_);
        drag_ = DragNone;
        resized(); repaint();
        notifyStateChanged();
    }

    //==================================================================
    /** 折叠态让卡片以外区域完全透传给下层控件：
        既不会挡住主窗口操作，也不会把「点空白处」当成关闭指令。 */
    bool hitTest (int x, int y) override
    {
        // 折叠 / 展开都只让卡片区域拦截鼠标，卡片外透传回主窗口，
        // 既不挡住其它控件，也不会把点空白处当成关闭指令。
        return cardBounds().contains (static_cast<float> (x), static_cast<float> (y));
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        if (ev.mods.isPopupMenu() || ev.mods.isRightButtonDown()) return;
        const auto card = cardBounds();
        if (! card.contains (ev.position))
            return;    // 卡片外已由 hitTest 透传回主窗口；不再点外部关闭面板

        const auto p = ev.position.toFloat();
        const bool canResize = ! minimised_ && ! maximised_;
        constexpr float kEdge = 8.0f;
        if (canResize && p.x >= card.getRight() - kEdge
                      && p.y >= card.getBottom() - kEdge)
        {
            beginResize (p);
            return;
        }
        if (canResize && p.x >= card.getRight() - kEdge)
        {
            beginResize (p, true, false);
            return;
        }
        if (canResize && p.y >= card.getBottom() - kEdge)
        {
            beginResize (p, false, true);
            return;
        }

        // 正常态只有顶部标题栏可拖；折叠态整条都是标题栏。
        const float titleBottom = card.getY()
                                + (minimised_ ? card.getHeight() : (float) kTitleH);
        if (! maximised_ && p.y < titleBottom)
        {
            drag_ = DragMove;
            dragStartMouse_ = p;
            dragStartPos_   = card.getPosition();
            dragStartSize_  = { card.getWidth(), card.getHeight() };
        }
    }

    void mouseDrag (const juce::MouseEvent& ev) override
    {
        if (drag_ == DragNone) return;
        const auto p = ev.position.toFloat();
        if (drag_ == DragMove)
        {
            posSet_ = true;
            pos_ = dragStartPos_ + (p - dragStartMouse_);   // cardBounds 内部限位
            resized(); repaint();
        }
        else
            updateResize (p);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag_ == DragNone) return;
        drag_ = DragNone;
        notifyStateChanged();       // 拖动结束才写盘，避免逐帧写文件
    }

    void mouseMove (const juce::MouseEvent& ev) override
    {
        const auto card = cardBounds();
        const auto p = ev.position.toFloat();
        if (! minimised_ && ! maximised_ && card.contains (p))
        {
            const bool r = p.x >= card.getRight() - 8.0f;
            const bool b = p.y >= card.getBottom() - 8.0f;
            if (r && b)      setMouseCursor (juce::MouseCursor::BottomRightCornerResizeCursor);
            else if (r)      setMouseCursor (juce::MouseCursor::RightEdgeResizeCursor);
            else if (b)      setMouseCursor (juce::MouseCursor::BottomEdgeResizeCursor);
            else             setMouseCursor (juce::MouseCursor::NormalCursor);
        }
        else
            setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseDoubleClick (const juce::MouseEvent& ev) override
    {
        const auto card = cardBounds();
        if (card.contains (ev.position)
            && ev.position.toFloat().y < card.getY() + (float) kTitleH)
            setMaximised (! maximised_);
    }

private:
    //==================================================================
    /** 卡片右下角的缩放手柄（自绘三条斜线，颜色随主题）。 */
    class ResizeGrip : public juce::Component
    {
    public:
        std::function<void (const juce::MouseEvent&)> onDown, onDrag, onUp;

        void mouseDown (const juce::MouseEvent& ev) override { if (onDown) onDown (ev); }
        void mouseDrag (const juce::MouseEvent& ev) override { if (onDrag) onDrag (ev); }
        void mouseUp   (const juce::MouseEvent& ev) override { if (onUp)   onUp (ev); }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (mb::ui::subText.withAlpha (0.60f));
            for (int i = 0; i < 3; ++i)
            {
                const float d = 4.0f + (float) i * 4.0f;
                g.drawLine (b.getRight() - d, b.getBottom(),
                            b.getRight(), b.getBottom() - d, 1.2f);
            }
        }
    };

    enum DragMode { DragNone = 0, DragMove, DragSizeBoth, DragSizeW, DragSizeH };

    void beginResize (juce::Point<float> p, bool horizontal = true, bool vertical = true)
    {
        drag_ = horizontal && vertical ? DragSizeBoth
                                       : (horizontal ? DragSizeW : DragSizeH);
        dragStartMouse_ = p;
        const auto card = cardBounds();
        dragStartPos_   = card.getPosition();
        dragStartSize_  = { card.getWidth(), card.getHeight() };
    }

    void updateResize (juce::Point<float> p)
    {
        if (drag_ == DragNone || drag_ == DragMove) return;
        const float W = (float) getWidth(), H = (float) getHeight();
        auto minSz = minCardSize();
        minSz.x = juce::jmin (minSz.x, W);
        minSz.y = juce::jmin (minSz.y, H);
        const float maxW = juce::jmax (minSz.x, W - dragStartPos_.x);
        const float maxH = juce::jmax (minSz.y, H - dragStartPos_.y);
        const float dx = p.x - dragStartMouse_.x, dy = p.y - dragStartMouse_.y;
        float w = dragStartSize_.x, h = dragStartSize_.y;
        if (drag_ == DragSizeBoth || drag_ == DragSizeW)
            w = juce::jlimit (minSz.x, maxW, dragStartSize_.x + dx);
        if (drag_ == DragSizeBoth || drag_ == DragSizeH)
            h = juce::jlimit (minSz.y, maxH, dragStartSize_.y + dy);
        userSize_ = { w, h };
        sizeSet_  = true;
        resized(); repaint();
    }

    void endResize()
    {
        if (drag_ == DragNone) return;
        drag_ = DragNone;
        notifyStateChanged();
    }

    void notifyStateChanged()
    {
        if (onStateChanged) onStateChanged();
    }

    //==================================================================
    juce::Component chrome_;    // 只承载三个窗口按钮（透明容器）
    ResizeGrip grip_;
    NativeWindowButton minBtn_  { juce::DocumentWindow::minimiseButton };
    NativeWindowButton maxBtn_  { juce::DocumentWindow::maximiseButton };
    NativeWindowButton closeBtn_{ juce::DocumentWindow::closeButton };
    bool minimised_ = false, maximised_ = false;
    bool posSet_ = false, sizeSet_ = false;
    DragMode drag_ = DragNone;
    juce::Point<float> pos_, userSize_;
    juce::Point<float> dragStartMouse_, dragStartPos_, dragStartSize_;

    JUCE_DECLARE_NON_COPYABLE (FloatingCardPanel)
};
