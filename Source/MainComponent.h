#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "AppSettings.h"
#include "AudioEngine.h"
#include "ChannelLinkOverlay.h"
#include "ChannelLinks.h"
#include "ChannelStrip.h"
#include "ColourPicker.h"
#include "DeviceSetup.h"
#include "DiagnosticsPanel.h"
#include "FxPanel.h"
#include "GlobalHotkey.h"
#include "I18n.h"
#include "MediaPlayerPanel.h"
#include "ModernMenu.h"
#include "NodeView.h"
#include "PluginPanel.h"
#include "PluginWindow.h"
#include "RouteGraph.h"
#include "SessionStore.h"
#include "SettingsPanel.h"
#include "SourcePicker.h"
#include "Theme.h"
#include "WelcomeOverlay.h"

//==============================================================================
// 通道条容器
//==============================================================================
class StripContainer : public juce::Component
{
public:
    // 分隔区几何常量：每条斑马带恒为 kZebraW 宽，各距相邻通道边框 kZebraPad。
    // 最小间距 kMinGap = 2*(kZebraW + kZebraPad)：此时两条斑马带恰好紧靠，
    // 窗口再窄也不会压缩斑马带（容器改由横向滚动消化），也不与通道边框折叠。
    static constexpr int kZebraW   = 10;
    static constexpr int kZebraPad = 3;
    static constexpr int kMinGap   = 2 * (kZebraW + kZebraPad);   // 26

    // 由 layoutStrips 注入：未对齐组与右对齐组之间的空隙范围
    bool groupSplit = false;
    int splitX0 = 0, splitX1 = 0;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (mb::ui::background);
        if (! groupSplit || splitX1 <= splitX0) return;

        const float x0 = static_cast<float> (splitX0);
        const float x1 = static_cast<float> (splitX1);
        const float h  = static_cast<float> (getHeight());
        const float bandW = x1 - x0;

        // 整条间隙填低对比底色：这是一个「分隔区」，不是一条细线
        g.setColour (mb::ui::topBar.withAlpha (0.55f));
        g.fillRect (x0, 0.0f, bandW, h);

        // 两条斑马带：宽度恒定 kZebraW，各距相邻通道边框 kZebraPad。
        // 最小间距下 zx1 == zx0 + zw（紧靠无留白）；窗口拉宽时中间自然留白。
        const float pad = static_cast<float> (kZebraPad);
        const float zw  = juce::jmin (static_cast<float> (kZebraW),
                                      juce::jmax (0.0f, (bandW - 2.0f * pad) * 0.5f));
        if (zw <= 0.0f) return;
        const float zx0 = x0 + pad;
        const float zx1 = x1 - pad - zw;

        // 分隔区收边画在斑马带外侧、离通道边框留 pad 距离，不与通道边框折叠
        g.setColour (mb::ui::border.withAlpha (0.85f));
        g.fillRect (zx0 - 1.0f, 0.0f, 1.0f, h);
        g.fillRect (zx1 + zw, 0.0f, 1.0f, h);

        auto zebra = [&] (float zx)
        {
            g.saveState();
            g.reduceClipRegion (juce::Rectangle<int> (juce::roundToInt (zx), 0,
                                                      juce::jmax (1, juce::roundToInt (zw)),
                                                      getHeight()));
            g.setColour (mb::ui::background.withAlpha (0.92f));   // 深（黑）底
            g.fillRect (zx, 0.0f, zw, h);
            for (float y = -zw - 12.0f; y < h + zw; y += 14.0f)
            {
                juce::Path p;
                p.startNewSubPath (zx, y);
                p.lineTo (zx + zw, y - zw);             // 45° 斜纹
                p.lineTo (zx + zw, y - zw + 6.0f);
                p.lineTo (zx, y + 6.0f);
                p.closeSubPath();
                g.setColour (mb::ui::subText.withAlpha (0.28f));    // 灰纹
                g.fillPath (p);
            }
            g.restoreState();
        };
        zebra (zx0);
        zebra (zx1);
    }
};

//==============================================================================
// 通道拖拽视觉层：跟随鼠标的浮起卡片 + 目标槽位插入线 + 落位闪光/移动轨迹。
// 只画不改：不拦截鼠标、不动任何通道条的 bounds / transform，所以不会干扰
// mouseDrag 的坐标换算与原有「即时数据换位」逻辑。由 ChannelStrip 的拖拽回调驱动。
//==============================================================================
class DragOverlay : public juce::Component,
                    private juce::Timer
{
public:
    DragOverlay()
    {
        setInterceptsMouseClicks (false, false);
        setAlwaysOnTop (true);
    }

    void begin (juce::Colour c, const juce::String& name,
                float x, float y, int slot)
    {
        colour_ = c;
        name_ = name;
        fingerX_ = x;
        fingerY_ = y;
        slot_ = slot;
        fromSlot_ = toSlot_ = slot;
        flash_ = 0.0f;
        phase_ = 0.0f;
        active_ = true;
        setVisible (true);
        toFront (false);
        startTimerHz (30);
        repaint();
    }

    void move (float x, float y, int slot)
    {
        fingerX_ = x;
        fingerY_ = y;
        slot_ = slot;
        if (active_) repaint();
    }

    /** 每次实际换位后触发：从旧槽位到新槽位画一段渐隐轨迹 + 光点。 */
    void flash (int fromSlot, int toSlot)
    {
        fromSlot_ = fromSlot;
        toSlot_ = toSlot;
        flash_ = 1.0f;
        if (active_) repaint();
    }

    void finish()
    {
        active_ = false;
        stopTimer();
        setVisible (false);
    }

    void paint (juce::Graphics& g) override
    {
        if (! active_ || ! isVisible()) return;

        const float w = static_cast<float> (getWidth());
        const float h = static_cast<float> (getHeight());
        const float sw = static_cast<float> (mb::ui::kStripWidth);
        const float pulse = 0.5f + 0.5f * std::sin (phase_);
        const float lineX = juce::jlimit (0.0f, juce::jmax (0.0f, w - 2.0f),
                                         static_cast<float> (slot_) * sw);

        // 目标槽位整列呼吸高亮：让「要放到哪」一眼可见
        g.setColour (colour_.withAlpha (0.08f + 0.09f * pulse));
        g.fillRect (lineX, 0.0f, juce::jmin (sw, w - lineX), h);

        // 插入竖线：四层外发光 + 中心实线
        for (int k = 4; k >= 1; --k)
        {
            g.setColour (colour_.withAlpha (0.11f / static_cast<float> (k)));
            g.fillRect (lineX - static_cast<float> (k * 3), 0.0f,
                        static_cast<float> (k * 6), h);
        }
        g.setColour (colour_.withAlpha (0.95f));
        g.fillRect (lineX - 1.5f, 0.0f, 3.0f, h);

        juce::Path top, bottom;
        top.addTriangle    (lineX - 7.0f, 0.0f, lineX + 7.0f, 0.0f, lineX, 10.0f);
        bottom.addTriangle (lineX - 7.0f, h,     lineX + 7.0f, h,     lineX, h - 10.0f);
        g.setColour (colour_);
        g.fillPath (top);
        g.fillPath (bottom);

        // 换位瞬间：旧位置 → 新位置的渐隐光带、连线和飞出的光点
        if (flash_ > 0.01f)
        {
            const float fx = static_cast<float> (fromSlot_) * sw + sw * 0.5f;
            const float tx = static_cast<float> (toSlot_)   * sw + sw * 0.5f;
            const float y  = 24.0f + 2.0f * std::sin (phase_);
            const float left = juce::jmin (fx, tx) - sw * 0.5f;
            const float wide = std::abs (tx - fx) + sw;

            g.setColour (colour_.withAlpha (0.22f * flash_));
            g.fillRoundedRectangle (left, 0.0f, wide, h, 6.0f);
            g.setColour (colour_.withAlpha (0.65f * flash_));
            g.drawLine (fx, y, tx, y, 2.0f);

            const float t = 1.0f - flash_;
            const float dotX = fx + (tx - fx) * t;
            g.setColour (juce::Colours::white.withAlpha (0.95f * flash_));
            g.fillEllipse (dotX - 5.0f, y - 5.0f, 10.0f, 10.0f);
        }

        // 跟随鼠标的「浮起卡片」：投影 + 通道色卡 + 名称，带轻微上下浮动
        const float cw = juce::jmin (96.0f, sw + 12.0f);
        const float ch = 42.0f;
        const float cx = juce::jlimit (2.0f, juce::jmax (2.0f, w - cw - 2.0f),
                                      fingerX_ - cw * 0.5f);
        const float cy = juce::jlimit (2.0f, juce::jmax (2.0f, h - ch - 2.0f),
                                      fingerY_ - ch - 18.0f + 2.5f * std::sin (phase_));
        const auto card = juce::Rectangle<float> (cx, cy, cw, ch);

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (card.translated (2.0f, 4.0f), 8.0f);
        g.setColour (colour_.withAlpha (0.94f));
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawRoundedRectangle (card, 8.0f, 1.5f);
        g.setFont (mb::ui::font (11.0f, true));
        g.setColour (colour_.contrasting (0.85f));
        g.drawText (name_, card.reduced (4.0f, 2.0f).toNearestInt(),
                    juce::Justification::centred, false);
    }

private:
    void timerCallback() override
    {
        phase_ += 0.22f;
        if (flash_ > 0.0f) flash_ = juce::jmax (0.0f, flash_ - 0.075f);
        repaint();
    }

    bool active_ = false;
    int slot_ = 0, fromSlot_ = 0, toSlot_ = 0;
    float fingerX_ = 0.0f, fingerY_ = 0.0f;
    float phase_ = 0.0f, flash_ = 0.0f;
    juce::Colour colour_ { juce::Colours::white };
    juce::String name_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DragOverlay)
};

//==============================================================================
// 系统托盘图标：左键显示主窗口，右键弹「显示 / 退出」菜单。
// JUCE 要求不要 addAndMakeVisible、不要 setVisible，创建后直接 setIconImage。
//==============================================================================
class TrayIcon : public juce::SystemTrayIconComponent
{
public:
    std::function<void()> onShow, onQuit, onMenu;   // onMenu = 右键完整菜单（常用设置项）

    void mouseDown (const juce::MouseEvent& ev) override
    {
        if (ev.mods.isPopupMenu())
        {
            if (onMenu) { onMenu(); return; }
            juce::PopupMenu m;
            m.addItem (1, mb::tr ("显示主窗口", "Show main window"));
            m.addSeparator();
            m.addItem (2, mb::tr ("退出", "Quit"));
            m.showMenuAsync (juce::PopupMenu::Options(), [this] (int r)
            {
                if (r == 1) { if (onShow) onShow(); }
                else if (r == 2) { if (onQuit) onQuit(); }
            });
        }
        else if (onShow) onShow();
    }
};

//==============================================================================
// 通道区顶部走带条：录所有已布防（R）的通道，停止后逐通道保存 WAV。
// 横跨整个通道区宽度，位于窗口顶（原生标题栏之下）、通道条之上。
//==============================================================================
class RecorderBar : public juce::Component
{
public:
    std::function<void()> onRec, onStop, onPause, onOpenDir;

    int armedCount = 0, activeCount = 0;
    bool paused = false;                       // 录音暂停中（MainComponent 每帧刷新）
    juce::String elapsed, lastFile;
    float level = 0.0f;                        // 输入电平 0..1（MainComponent 每帧刷新）

    static constexpr int kHeight = 34;

    void paint (juce::Graphics& g) override
    {
        const int w = getWidth(), h = getHeight();
        g.setColour (mb::ui::ribbon);
        g.fillRect (0, 0, w, h);
        g.setColour (mb::ui::border);
        g.fillRect (0, h - 1, w, 1);

        const bool recording = activeCount > 0;

        // 主按钮（开始 / 暂停 / 继续三态一体）：空闲=录音圆点，
        // 录音中=暂停双竖条，暂停中=继续三角。按钮本体保持中性底色，
        // 仅字形变色，避免空闲时看起来像已开录。
        const auto rec = recRect();
        g.setColour (mb::ui::card);
        g.fillRoundedRectangle (rec, 4.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (rec, 4.0f, 0.8f);
        const auto c = rec.getCentre();
        if (recording && ! paused)                      // 暂停双竖条
        {
            g.setColour (mb::ui::meterRed);
            g.fillRoundedRectangle (c.x - 4.0f, c.y - 5.0f, 3.0f, 10.0f, 1.0f);
            g.fillRoundedRectangle (c.x + 1.0f, c.y - 5.0f, 3.0f, 10.0f, 1.0f);
        }
        else if (recording)                             // 继续录音三角
        {
            juce::Path tri;
            tri.addTriangle (c.x - 4.0f, c.y - 5.5f, c.x - 4.0f, c.y + 5.5f, c.x + 5.0f, c.y);
            g.setColour (mb::ui::meterRed);
            g.fillPath (tri);
        }
        else                                            // 录音圆点
        {
            g.setColour (mb::ui::meterRed);
            g.fillEllipse (c.x - 4.0f, c.y - 4.0f, 8.0f, 8.0f);
        }

        const auto stop = stopRect();
        g.setColour (mb::ui::card);
        g.fillRoundedRectangle (stop, 4.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (stop, 4.0f, 0.8f);
        g.setColour (mb::ui::text);
        g.fillRoundedRectangle (stop.getCentreX() - 4.0f, stop.getCentreY() - 4.0f,
                                8.0f, 8.0f, 1.2f);

        g.setFont (mb::ui::font (10.0f, true));
        g.setColour (recording ? (paused ? mb::ui::meterYellow : mb::ui::meterRed)
                               : mb::ui::faintText);
        g.drawText (elapsed.isEmpty() ? mb::S("00:00") : elapsed,
                    68, 0, 42, h, juce::Justification::centredLeft, false);

        juce::String info = mb::tr ("布防 ", "Armed ") + juce::String (armedCount)
                          + mb::tr (" 通道", " ch");
        if (recording)
            info = (paused ? mb::tr ("已暂停 ", "Paused ") : mb::tr ("录制 ", "Rec "))
                 + juce::String (activeCount) + mb::S("  ·  ") + info;
        if (lastFile.isNotEmpty())
            info = mb::tr ("最近：", "Last: ") + lastFile + mb::S("  ·  ") + info;

        constexpr int bars = 16;
        constexpr float barW = 3.0f, barGap = 2.0f;
        const float meterX = 116.0f;                 // 电平表紧跟在计时器右侧
        g.setFont (mb::ui::font (8.5f));
        g.setColour (mb::ui::subText);
        g.drawText (info, 204, 0, juce::jmax (20, static_cast<int> (openRect().getX()) - 212), h,
                    juce::Justification::centredLeft, true);

        const int lit = juce::roundToInt (level * static_cast<float> (bars));
        for (int i = 0; i < bars; ++i)
        {
            const float tt = static_cast<float> (i) / static_cast<float> (bars - 1);
            juce::Colour c = tt < 0.62f ? mb::ui::meterGreen
                           : (tt < 0.84f ? mb::ui::meterYellow : mb::ui::meterRed);
            if (i >= lit) c = mb::ui::meterOff;
            g.setColour (c);
            g.fillRoundedRectangle (meterX + static_cast<float> (i) * (barW + barGap),
                                    static_cast<float> ((h - 14) / 2),
                                    barW, 14.0f, 1.0f);
        }

        g.setFont (mb::ui::font (8.5f, true));
        g.setColour (mb::ui::isDarkTheme() ? juce::Colours::white : juce::Colours::black);
        g.drawText (mb::tr ("打开目录", "Folder"), openRect().toNearestInt(),
                    juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        // 触控板长按 = 右键，不能让「录音 / 停止 / 打开目录」被误触发
        if (ev.mods.isPopupMenu() || ev.mods.isRightButtonDown()) return;
        if (recRect().contains (ev.position))
        {
            // 三态一体主按钮：空闲开始录音，录音/暂停中切换暂停与继续
            if (activeCount > 0) { if (onPause) onPause(); }
            else                 { if (onRec) onRec(); }
        }
        else if (stopRect().contains (ev.position)) { if (onStop) onStop(); }
        else if (openRect().contains (ev.position)) { if (onOpenDir) onOpenDir(); }
    }

    void mouseMove (const juce::MouseEvent& ev) override
    {
        const auto p = ev.position;
        const bool hit = recRect().contains (p) || stopRect().contains (p)
                      || openRect().contains (p);
        setMouseCursor (hit ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::NormalCursor);
    }

    juce::Rectangle<float> recRect()   const { return { 10.0f, static_cast<float> ((getHeight() - 20) / 2), 26.0f, 20.0f }; }
    juce::Rectangle<float> stopRect()  const { return { 40.0f, static_cast<float> ((getHeight() - 20) / 2), 22.0f, 20.0f }; }
    juce::Rectangle<float> openRect()  const { return { static_cast<float> (getWidth() - 88), 0.0f, 80.0f, static_cast<float> (getHeight()) }; }
};

//==============================================================================
// 主界面（配线架）
//
// 顶栏：已并入原生窗口标题栏（品牌名跟随语言 + 驱动 / 设备 / 延迟 / 连接数；
// 临时消息 2.6 s 内顶替标题尾部）
// 通道区顶部：录音机走带条（横跨全部通道条）
// 左侧抽屉：添加通道 / 保存预设 / 导入导出 / 清除，底部「设置」
// 主区：横向通道条，每条 = 头部（颜色点）+ 输入源区 + 插件区 + 输出目标区 + 推子卡 + 名字条
//
// 无「启动 / 停止」：设备确认后音频引擎自动运行。
//==============================================================================

//==============================================================================
// 抽屉按钮：线性矢量小图标 + 文本；收纳模式（iconOnly）下只绘制图标。
//==============================================================================
class DrawerButton : public juce::TextButton
{
public:
    enum Icon { Add, NewDoc, Save, Import, Export, Clear, Settings,
                Nodes, Mixer, Collapse, Expand, ThemeSun, ThemeMoon,
                RecShow, RecHide, Fold };

    Icon icon = Add;
    bool iconOnly = false;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        // 选中态（如当前处于节点视图）：用主题强调色填充 + 白字白图标，区别于普通键帽
        const bool on = getToggleState();
        // 物理键帽：底部投影（按下下沉）+ 立体渐变帽体 + 顶缘高光 + 边框
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = 6.0f;
        g.setColour (juce::Colours::black.withAlpha (down ? 0.22f : 0.38f));
        g.fillRoundedRectangle (r.translated (0.0f, down ? 0.8f : 1.8f), rad);

        const auto base = on ? mb::ui::accent[0] : mb::ui::card;
        const auto top = over ? base.brighter (0.18f) : base.brighter (0.08f);
        const auto bot = on ? base.darker (0.40f) : mb::ui::slotBg.darker (down ? 0.45f : 0.28f);
        g.setGradientFill (juce::ColourGradient (top, 0.0f, r.getY(),
                                                 bot, 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle (r, rad);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (down ? 0.05f : 0.12f));
        g.drawLine (r.getX() + rad, r.getY() + 1.0f, r.getRight() - rad, r.getY() + 1.0f);

        const auto c = (on ? juce::Colours::white : findColour (textColourOffId))
                          .withAlpha (down ? 0.75f : 1.0f);
        const auto area = getLocalBounds().toFloat();
        const float cy = area.getCentreY();

        if (iconOnly)
        {
            drawIcon (g, icon, juce::Rectangle<float> (16.0f, 16.0f).withCentre (area.getCentre()), c);
            return;
        }

        // 实测文本宽度，图标 + 文本整体居中，保证中英文标题完整显示
        const auto f = mb::ui::font (10.5f);
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, getButtonText(), 0.0f, 0.0f);
        const float textW = ga.getNumGlyphs() > 0
                              ? ga.getBoundingBox (0, ga.getNumGlyphs(), true).getWidth() : 0.0f;
        const float total = 16.0f + 6.0f + textW;
        const float x = juce::jmax (5.0f, (area.getWidth() - total) * 0.5f);

        drawIcon (g, icon, juce::Rectangle<float> (x, cy - 8.0f, 16.0f, 16.0f), c);
        g.setFont (f);
        g.setColour (c);
        const int tx = (int) (x + 22.0f);
        g.drawText (getButtonText(), tx, 0, juce::jmax (10, getWidth() - tx - 2), getHeight(),
                    juce::Justification::centredLeft, false);
    }

    // 16x16 线性图标，1.6px 圆头描边
    static void drawIcon (juce::Graphics& g, Icon kind, juce::Rectangle<float> r, juce::Colour c)
    {
        const float cx = r.getCentreX(), cy = r.getCentreY();
        const float shX = 0.7f, shY = 1.2f;          // 暗影偏移（物理浮雕感）

        const juce::PathStrokeType st (1.7f, juce::PathStrokeType::curved,
                                       juce::PathStrokeType::rounded);

        auto stroked = [&] (const juce::Path& p)
        {
            juce::Path s = p;
            s.applyTransform (juce::AffineTransform::translation (shX, shY));
            g.setColour (juce::Colours::black.withAlpha (0.30f));
            g.strokePath (s, st);
            g.setColour (c);
            g.strokePath (p, st);
        };
        auto filled = [&] (const juce::Path& p, bool glossy = true)
        {
            juce::Path s = p;
            s.applyTransform (juce::AffineTransform::translation (shX, shY));
            g.setColour (juce::Colours::black.withAlpha (0.30f));
            g.fillPath (s);
            g.setColour (c);
            g.fillPath (p);
            if (glossy)
            {
                g.saveState();
                g.reduceClipRegion (p);
                g.setColour (juce::Colours::white.withAlpha (0.42f));
                g.fillEllipse (cx - 3.2f, cy - 4.6f, 6.2f, 4.0f);   // 左上方高光
                g.restoreState();
            }
        };

        switch (kind)
        {
            case ThemeSun:  // 太阳（点击后进入浅色模式）
            {
                juce::Path rays;
                for (int i = 0; i < 8; ++i)
                {
                    const float a = i * 0.7853982f;
                    rays.startNewSubPath (cx + std::cos (a) * 4.4f, cy + std::sin (a) * 4.4f);
                    rays.lineTo           (cx + std::cos (a) * 6.3f, cy + std::sin (a) * 6.3f);
                }
                stroked (rays);
                juce::Path disc;  disc.addEllipse (cx - 2.8f, cy - 2.8f, 5.6f, 5.6f);
                filled (disc);
                break;
            }
            case ThemeMoon: // 月牙（点击后进入深色模式）
            {
                juce::Path moon;
                moon.addCentredArc (cx, cy, 5.0f, 5.0f, 0.0f, -1.9f, 1.9f, false);
                moon.addCentredArc (cx - 1.8f, cy, 4.0f, 4.0f, 0.0f, 1.9f, -1.9f, false);
                moon.closeSubPath();
                filled (moon);
                break;
            }
            case Add:       // 加号
            {
                juce::Path p;
                p.startNewSubPath (cx - 5.0f, cy);  p.lineTo (cx + 5.0f, cy);
                p.startNewSubPath (cx, cy - 5.0f);  p.lineTo (cx, cy + 5.0f);
                stroked (p);
                break;
            }
            case NewDoc:    // 空白文档 + 折角（新建预设）
            {
                juce::Path page;
                page.startNewSubPath (cx - 4.5f, cy - 6.0f);
                page.lineTo (cx + 1.5f, cy - 6.0f);
                page.lineTo (cx + 4.5f, cy - 3.0f);
                page.lineTo (cx + 4.5f, cy + 6.0f);
                page.lineTo (cx - 4.5f, cy + 6.0f);
                page.closeSubPath();
                stroked (page);
                juce::Path fold;
                fold.startNewSubPath (cx + 1.5f, cy - 6.0f);
                fold.lineTo (cx + 1.5f, cy - 3.0f);
                fold.lineTo (cx + 4.5f, cy - 3.0f);
                stroked (fold);
                break;
            }
            case Save:      // 软盘
            {
                juce::Path body;  body.addRoundedRectangle (cx - 5.5f, cy - 5.5f, 11.0f, 11.0f, 1.5f);
                stroked (body);
                juce::Path metal; metal.addRectangle (cx - 2.5f, cy - 0.5f, 5.0f, 6.0f);
                stroked (metal);
                juce::Path notch;
                notch.startNewSubPath (cx - 2.0f, cy - 5.5f);  notch.lineTo (cx - 2.0f, cy - 3.0f);
                notch.lineTo (cx + 2.0f, cy - 3.0f);  notch.lineTo (cx + 2.0f, cy - 5.5f);
                stroked (notch);
                break;
            }
            case Import:    // 向下箭头 + 托盘
            {
                juce::Path p;
                p.startNewSubPath (cx, cy - 6.0f);  p.lineTo (cx, cy + 1.5f);
                p.startNewSubPath (cx - 3.0f, cy - 1.5f);  p.lineTo (cx, cy + 1.5f);
                p.startNewSubPath (cx + 3.0f, cy - 1.5f);  p.lineTo (cx, cy + 1.5f);
                p.startNewSubPath (cx - 5.0f, cy + 5.0f);  p.lineTo (cx + 5.0f, cy + 5.0f);
                stroked (p);
                break;
            }
            case Export:    // 向上箭头 + 托盘
            {
                juce::Path p;
                p.startNewSubPath (cx, cy + 1.5f);  p.lineTo (cx, cy - 6.0f);
                p.startNewSubPath (cx - 3.0f, cy - 3.0f);  p.lineTo (cx, cy - 6.0f);
                p.startNewSubPath (cx + 3.0f, cy - 3.0f);  p.lineTo (cx, cy - 6.0f);
                p.startNewSubPath (cx - 5.0f, cy + 5.0f);  p.lineTo (cx + 5.0f, cy + 5.0f);
                stroked (p);
                break;
            }
            case Clear:     // 叉号
            {
                juce::Path p;
                p.startNewSubPath (cx - 4.0f, cy - 4.0f);  p.lineTo (cx + 4.0f, cy + 4.0f);
                p.startNewSubPath (cx + 4.0f, cy - 4.0f);  p.lineTo (cx - 4.0f, cy + 4.0f);
                stroked (p);
                break;
            }
            case Settings:  // 齿轮
            {
                juce::Path ring;  ring.addEllipse (cx - 3.2f, cy - 3.2f, 6.4f, 6.4f);
                stroked (ring);
                juce::Path teeth;
                for (int i = 0; i < 8; ++i)
                {
                    const float a = (float) i * (juce::MathConstants<float>::pi / 4.0f);
                    const float s = std::sin (a), co = std::cos (a);
                    teeth.startNewSubPath (cx + 4.6f * co, cy + 4.6f * s);
                    teeth.lineTo           (cx + 6.4f * co, cy + 6.4f * s);
                }
                stroked (teeth);
                break;
            }
            case Nodes:     // 节点连接：两点一线
            {
                juce::Path line;
                line.startNewSubPath (cx - 3.4f, cy - 3.4f);
                line.lineTo (cx + 3.4f, cy + 3.4f);
                stroked (line);
                juce::Path dot1;  dot1.addEllipse (cx - 6.8f, cy - 6.8f, 4.4f, 4.4f);
                juce::Path dot2;  dot2.addEllipse (cx + 2.4f, cy + 2.4f, 4.4f, 4.4f);
                filled (dot1);
                filled (dot2);
                break;
            }
            case Mixer:     // 经典视图（调音台）：三根竖推子，手柄错位
            {
                const float xs[3] = { cx - 4.6f, cx, cx + 4.6f };
                const float ys[3] = { cy + 1.6f, cy - 3.0f, cy + 4.0f };
                for (int i = 0; i < 3; ++i)
                {
                    juce::Path rail;
                    rail.startNewSubPath (xs[i], cy - 6.0f);
                    rail.lineTo (xs[i], cy + 6.0f);
                    stroked (rail);
                    juce::Path cap;
                    cap.startNewSubPath (xs[i] - 2.6f, ys[i]);
                    cap.lineTo (xs[i] + 2.6f, ys[i]);
                    stroked (cap);
                }
                break;
            }
            case Collapse:  // 双左箭头 «（点击折叠抽屉）
            {
                juce::Path p;
                p.startNewSubPath (cx - 0.5f, cy - 4.0f);  p.lineTo (cx - 4.0f, cy);
                p.lineTo (cx - 0.5f, cy + 4.0f);
                p.startNewSubPath (cx + 4.5f, cy - 4.0f);  p.lineTo (cx + 1.0f, cy);
                p.lineTo (cx + 4.5f, cy + 4.0f);
                stroked (p);
                break;
            }
            case Expand:    // 双右箭头 »（点击展开抽屉）
            {
                juce::Path p;
                p.startNewSubPath (cx - 4.5f, cy - 4.0f);  p.lineTo (cx - 1.0f, cy);
                p.lineTo (cx - 4.5f, cy + 4.0f);
                p.startNewSubPath (cx + 0.5f, cy - 4.0f);  p.lineTo (cx + 4.0f, cy);
                p.lineTo (cx + 0.5f, cy + 4.0f);
                stroked (p);
                break;
            }
            case RecShow:   // 录音圆点（点击显示录音区）
            {
                juce::Path dot;  dot.addEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
                filled (dot);
                break;
            }
            case RecHide:   // 圆点加斜杠（点击隐藏录音区）
            {
                juce::Path ring;  ring.addEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
                stroked (ring);
                juce::Path slash;
                slash.startNewSubPath (cx - 3.2f, cy + 3.2f);
                slash.lineTo (cx + 3.2f, cy - 3.2f);
                stroked (slash);
                break;
            }
            case Fold:       // 向下收起（卷起窗口，只留底部控制带）
            {
                // 窗口外框 + 底部加粗条，示意「向下收成一横条」
                juce::Path box;  box.addRoundedRectangle (cx - 5.5f, cy - 6.0f, 11.0f, 9.0f, 1.5f);
                stroked (box);
                g.setColour (c);
                g.drawLine (cx - 5.0f, cy + 2.6f, cx + 5.0f, cy + 2.6f, 2.0f);
                // 底部向下小三角
                juce::Path tri;
                tri.startNewSubPath (cx - 2.6f, cy + 3.6f);
                tri.lineTo (cx, cy + 6.2f);
                tri.lineTo (cx + 2.6f, cy + 3.6f);
                stroked (tri);
                break;
            }
        }
    }

};

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::KeyListener   // 全局快捷键：通道链接触发
{
public:
    MainComponent()
        : engine_ (graph_),
          nodeCanvas_ (graph_, engine_)
    {
        settings_ = mb::AppSettings::load();
        mb::setLanguageMode (settings_.languageMode);      // 0=跟随系统 1=中文 2=English
        settings_.chinese = mb::isZh();                    // 回写解析结果，保存时字段一致
        firstLaunch_ = ! mb::AppSettings::file().existsAsFile();
        // 图标长度运行时查询：换图标后即使本翻译单元未重编也不会越界读资源。
        int iconSize = 0;
        const char* iconData = BinaryData::getNamedResource ("app_icon_png", iconSize);
        appIcon_ = iconData != nullptr && iconSize > 0
                 ? juce::ImageCache::getFromMemory (iconData, iconSize)
                 : juce::Image();
        mb::ui::channelBorderMode() = settings_.channelBorder;
        mb::AppSettings::applyAutoStart (settings_.autoStart);
        recorderVisible_ = settings_.recorderVisible;   // 恢复顶部录音区显隐
        applyRecorderSettings (settings_);          // 录音模式 / 格式 / 位深 / 目录

        graph_.resize (6, engine_.catalog().inputPortCount(),
                          engine_.catalog().outputPortCount());
        // 独奏豁免：「向右对齐」的监听/输出总线通道不参与独奏剪枝——
        // 独奏任何通道都不应把输出到耳机的总线整条静默。
        graph_.soloExempt = [] (int ch) { return mb::ui::isChannelRightAligned (ch); };
        engine_.prepareBuffers();

        viewport_.setViewedComponent (&stripContainer_, false);
        // 竖向滚动条常关：常规布局内容高=视口高不溢出；折叠卷起时改由代码滚到底部
        viewport_.setScrollBarsShown (false, false);
        addAndMakeVisible (viewport_);

        // 拖拽特效层挂在通道容器上（与通道条同一坐标系），初始隐藏、始终置顶
        stripContainer_.addChildComponent (dragOverlay_);

        setupButton (addBtn_,        DrawerButton::Add,      mb::tr ("添加通道", "Add"),    [this] { addChannel(); });
        setupButton (newBtn_,        DrawerButton::NewDoc,   mb::tr ("新建预设", "New"),    [this] { newPreset(); });
        setupButton (savePresetBtn_, DrawerButton::Save,     mb::tr ("保存预设", "Save"),   [this] { saveCurrentPreset(); });
        setupButton (importBtn_,     DrawerButton::Import,   mb::tr ("导入预设", "Import"), [this] { importPreset(); });
        setupButton (exportBtn_,     DrawerButton::Export,   mb::tr ("导出预设", "Export"), [this] { exportPreset(); });
        setupButton (clearBtn_,      DrawerButton::Clear,    mb::tr ("清除连接", "Clear"),  [this] { confirmClearRoutes(); });
        setupButton (settingsBtn_,   DrawerButton::Settings, mb::tr ("设置", "Settings"),   [this] { showSettings(); });

        // 收纳切换：显示图标 + 标题（展开时收窄抽屉）
        toggleBtn_.icon = DrawerButton::Collapse;
        toggleBtn_.setButtonText (mb::tr ("收纳抽屉", "Collapse drawer"));
        toggleBtn_.setTooltip (mb::tr ("收纳抽屉", "Collapse drawer"));
        toggleBtn_.setColour (juce::TextButton::buttonColourId,  mb::ui::card);
        toggleBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::text);
        toggleBtn_.onClick = [this] { toggleDrawer(); };
        addAndMakeVisible (toggleBtn_);

        // 录音区显隐：控制顶部走带条可见性，隐藏时调音台通道顶满主区
        setupButton (recBtn_, recorderVisible_ ? DrawerButton::RecHide : DrawerButton::RecShow,
                     recBtnText(), [this] { toggleRecorderVisible(); });

        // 节点连接视图：与调音台共用同一张 RouteGraph 的另一种投影
        setupButton (viewBtn_, DrawerButton::Nodes,
                     mb::tr ("节点视图", "Node view"), [this] { toggleNodeView(); });

        // 折叠窗口：卷起主窗口只保留通道底部控制带（与标题栏最小化箭头互为入口）
        setupButton (foldBtn_, DrawerButton::Fold,
                     mb::tr ("折叠窗口", "Fold window"), [this] { setFolded (! folded_); });

        // 深浅模式切换：与设置页同一开关的第二个入口（图标/文字随状态同步）
        setupButton (themeBtn_, DrawerButton::ThemeMoon, themeBtnText(),
                     [this] { toggleLightTheme(); });
        updateThemeButton();
        nodeViewport_.setViewedComponent (&nodeCanvas_, false);
        // 窄窗口时画布有最小宽度 620，保留水平滚动兜底；宽窗口下画布已铺满，不会出现横条
        nodeViewport_.setScrollBarsShown (true, true);
        nodeViewport_.setVisible (false);
        addAndMakeVisible (nodeViewport_);
        nodeCanvas_.onMessage = [this] (const juce::String& m) { setMessage (m); };
        nodeCanvas_.onChanged = [this]
        {
            engine_.syncDevices();
            refreshStripsFromGraph();         // C→C 建线后对方通道立即显示输入源
            updateSummary();
        };
        nodeCanvas_.onAddPlugin    = [this] (int ch)          { showPluginPicker (ch); };
        nodeCanvas_.onOpenPlugin   = [this] (int ch, int idx) { openPluginEditor (ch, idx); };
        nodeCanvas_.onRemovePlugin = [this] (int ch, int idx) { removePluginAt (ch, idx); };
        nodeCanvas_.onOpenSource   = [this] (int ch, bool input) { showSourcePicker (ch, input); };
        applyDrawerState (settings_.drawerCollapsed);   // 恢复抽屉收纳状态（先于窗口定宽）
        applyViewMode (settings_.nodeView);

        addAndMakeVisible (recorderBar_);
        recorderBar_.onRec = [this] { toggleRecording(); };
        recorderBar_.onStop = [this] { stopRecording(); };
        recorderBar_.onPause = [this] { toggleRecordingPause(); };
        recorderBar_.onOpenDir = [this]
        {
            // 「打开目录」按钮：直接打开当前录音目录
            // （自定义目录入口保留在设置页 / 诊断面板）
            engine_.recorder().directory().createDirectory();
            engine_.recorder().directory().startAsProcess();
        };

        const auto saved = engine_.savedChoice();
        bool restoredSession = false;
        if (saved.valid)
        {
            if (engine_.applySetup (saved, false))
                restoredSession = tryRestoreSession();
            else
                showSetup (true, engine_.error());
        }
        if (! saved.valid)
        {
            applyDefaultRoutes();
            if (firstLaunch_)
                showWelcome();                    // 首次开箱：先选语言，再进设备设置
            else
                showSetup (true);
        }

        // 兜底：会话文件不可用时，回退加载「保存预设」的当前预设
        if (! restoredSession && settings_.currentPreset.isNotEmpty())
        {
            const auto pv = mb::SessionStore::load (juce::File (settings_.currentPreset));
            if (pv.isObject() && mb::SessionStore::fromJson (pv, graph_, engine_,
                    engine_.catalog().inputPortCount(),
                    engine_.catalog().outputPortCount()))
            {
                nodeCanvas_.layoutFromJson (pv.getProperty ("nodeLayout", juce::var()));
                engine_.prepareBuffers();
                restoredSession = true;
            }
        }

        if (restoredSession)
        {
            for (auto& s : strips_) s->syncFromGraph();
            updateSummary();
            setMessage (mb::tr ("已恢复上次会话", "Session restored"));
            reportDroppedRoutes (mb::tr ("已恢复上次会话", "Session restored"));
        }
        engine_.syncDevices();

        syncChannelNames();                  // 初始 / 恢复会话后补齐通道名
        syncAlignmentVector();               // 右对齐标记与通道数对齐
        rebuildStrips();
        applyLanguage();
        startTimerHz (20);
        setSize (preferredWidth(), mb::ui::kDefaultWindowHeight);

        // 构造 / 恢复会话触发的 updateSummary 不算「未保存改动」
        presetDirty_ = false;
        // 「不保存」退出时的回滚基准：本次启动恢复到的状态
        savedSnapshot_ = mb::SessionStore::toJson (graph_, engine_, nodeCanvas_.layoutToJson());
        // 锁定属于本次运行的状态：重启后桌面歌词窗口不会自动重建，
        // 残留 locked=true 会造出一个「没有窗口可解锁」的幽灵锁定，启动即清掉。
        if (settings_.desktopLyricsLocked)
        {
            settings_.desktopLyricsLocked = false;
            mb::AppSettings::save (settings_);
        }
        syncTrayIcon();
        refreshGlobalHotkeys();              // 注册隐藏窗口 / 解锁歌词 / 所有通道快捷键

        // 全局快捷键触发：可见 → 隐藏（托盘 / 完全隐藏）；隐藏 → 恢复
        hotkeyEngine_.onTrigger = [this]
        {
            // 用顶层窗口的 isVisible() 判断，比 isShowing() 更直接：
            // 窗口隐藏后依然有消息循环，快捷键能把它唤回来
            auto* top = getTopLevelComponent();
            if (top != nullptr && ! top->isVisible())
            {
                if (onShowWindow) onShowWindow();
                syncTrayIcon();                 // 完全隐藏后恢复时补回托盘图标
                return;
            }
            saveSessionQuietly();            // 当前可见 → 落盘后隐藏
            if (onHideWindow) onHideWindow();
            if (settings_.hideMode == 0)
            {
                syncTrayIcon();              // 托盘模式：图标常驻，点击即可恢复
                if (tray_ != nullptr)
                    tray_->showInfoBubble (mb::S("MicroBridge"),
                        mb::tr ("已隐藏到托盘，按快捷键或点击托盘图标恢复",
                                "Hidden to tray; press the hotkey or click the tray icon"));
            }
            else
                tray_.reset();               // 完全隐藏：托盘图标一并移除，只能靠快捷键找回
        };

        // 通道链接：效果参数变化联动钩子（装载完成后安装，
        // 避免会话恢复期间的批量写入触发联动风暴）。键盘钩子在
        // parentHierarchyChanged 里挂到顶层窗口（JUCE8 无 Desktop 全局键盘监听）。
        engine_.onFxParamsChanged = [this] (int ch) { handleFxLinkChanged (ch); };
        mb::links::resize (graph_.channelCount());
    }

    ~MainComponent() override
    {
        if (linkKeyHooked_ != nullptr)
            linkKeyHooked_->removeKeyListener (this);
        stopTimer();
    }

    // 由 MainWindow 绑定：托盘显隐主窗口
    std::function<void()> onHideWindow, onShowWindow;

    /** 设置页「窗口始终置顶」：改顶层窗口 Z 序（Windows HWND_TOPMOST）。
        桌面歌词窗口同时刷新（主窗口置顶时它也置顶）。由 MainWindow 启动 /
        设置页改动两处调用。 */
    void applyAlwaysOnTop()
    {
        if (auto* peer = getPeer())
            peer->setAlwaysOnTop (settings_.alwaysOnTop);
        if (mediaPanel_ != nullptr)
            mediaPanel_->applyTopMost();
    }

    /** 由 MainWindow 绑定：标题文本变化（状态 / 临时消息 / 语言）时更新原生标题栏。 */
    std::function<void (const juce::String&)> onTitleChanged;

    /** 由 MainWindow 绑定：可见通道数 / 抽屉宽度变化时自动贴合窗口宽度。 */
    std::function<void (int width)> onPreferredWidthChanged;

    /** 由 MainWindow 绑定：窗口卷起状态变化（true=折叠只留底部，false=展开）。 */
    std::function<void (bool)> onFoldChanged;

    /** 理想窗口宽度 = 抽屉 + 可见通道条 + 2px 边框；不再固定 1280 留白。
        折叠（卷起）态无抽屉，宽度只贴合通道条，避免右侧大片空白。 */
    int preferredWidth() const
    {
        const int strips = juce::jmax (1, visibleSlotCount()) * mb::ui::kStripWidth;
        if (folded_)
            return juce::jlimit (260, mb::ui::kMaxWindowWidth, strips + 4);
        return juce::jlimit (mb::ui::kMinWindowWidth, mb::ui::kMaxWindowWidth,
                             drawerW_ + strips + 2);
    }

    /** 通道数量变化（新增 / 删除 / 隐藏 / 显示）后，按当前可见通道数重算理想宽度，
     *  自动拉伸或收缩窗口宽度，高度保持用户当前值。窗口可自由拖动缩放，
     *  这里只在「可见通道数变化」时接管宽度，避免与用户手动拖拽打架。
     *  DocumentWindow 构造时已设 resizeToFitWhenContentChangesSize=true，
     *  所以对内容组件 setSize 即会带动整个窗口。 */
    void refreshWindowWidth()
    {
        const int target = preferredWidth();
        if (target == getWidth()) return;
        setSize (target, getHeight());
    }

    /** 状态变化后刷新原生窗口标题（composeWindowTitle 有去重，重复调用无副作用）。 */
    void refreshWindowTitle()
    {
        if (onTitleChanged == nullptr) return;      // MainWindow 尚未接线，别缓存结果
        const auto t = composeWindowTitle();
        if (t == lastWindowTitle_) return;
        lastWindowTitle_ = t;
        onTitleChanged (t);
    }

    //==========================================================================
    // 节点连接视图 / 调音台视图切换：同一张 RouteGraph，两个投影互相同步
    void applyViewMode (bool nodeView)
    {
        nodeViewMode_ = nodeView;
        settings_.nodeView = nodeView;
        mb::AppSettings::save (settings_);

        viewport_.setVisible (! nodeView);
        recorderBar_.setVisible (! nodeView);
        nodeViewport_.setVisible (nodeView);
        if (nodeView) nodeCanvas_.updateSize();

        // 文案随视图切换（节点视图 ↔ 经典视图），不做高亮填充，保持键帽观感统一
        refreshDrawerTexts();
        resized();
    }

    void toggleNodeView() { applyViewMode (! nodeViewMode_); }

    /** 卷起折叠高度 = 通道底部控制带（按钮格 + 名称带）。窗口标题栏另算。 */
    int foldedContentHeight() const
    {
        return mb::ui::kNameBandHeight + 56;   // 56 = ChannelStrip::kButtonGridH
    }

    bool isFolded() const { return folded_; }

    /** 切换窗口卷起状态：true=只留通道底部控制带，false=展开。
        窗口尺寸由 MainWindow 经 onFoldChanged 接管，这里只管内容与按钮态。 */
    void setFolded (bool f)
    {
        if (folded_ == f) return;
        folded_ = f;
        saveSessionQuietly();              // 折叠态也顺手落盘一次，避免遗漏
        refreshDrawerTexts();              // 折叠窗口 ↔ 展开窗口
        // 先经 MainWindow 收放窗口高度（同时放宽 / 还原最小尺寸限制），
        // 再按折叠态理想宽度贴合通道条，去掉右侧空白；最后整体重排。
        if (onFoldChanged) onFoldChanged (folded_);
        if (f) refreshWindowWidth();
        resized();
    }

    /** 关闭按钮入口：始终接管。开启「关闭隐藏到托盘」时直接隐藏（不弹未保存提示，
        改动已随会话自动保存）；只有真正退出（关闭退出 / 托盘退出）才弹窗询问。 */
    bool handleCloseRequest()
    {
        if (quitPromptOpen_) return true;
        if (settings_.closeToTray)
        {
            saveSessionQuietly();              // 落盘一次，改动不丢
            if (onHideWindow) onHideWindow();
            return true;
        }
        if (! presetDirty_)
        {
            finishQuit (true);
            return true;
        }
        promptUnsavedThen ([this] (int choice) { finishQuit (true, choice == 2); });
        return true;
    }

    /** 静默保存一次会话（不弹任何提示）。 */
    void saveSessionQuietly()
    {
        if (engine_.isReady())
            mb::SessionStore::save (mb::SessionStore::sessionFile(),
                                    mb::SessionStore::toJson (graph_, engine_, nodeCanvas_.layoutToJson()));
    }

    //==========================================================================
    // 另起 / 重启：moreThanOneInstanceAllowed() 已返回 true，JUCE 不会把命令行
    // 转发给已有实例，因此 startAsProcess 启动的是真正独立的新进程（可多开）。
    static juce::File appExecutable()
    {
        return juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    }

    void launchNewInstance()
    {
        const auto exe = appExecutable();
        if (! exe.existsAsFile() || ! exe.startAsProcess ("--show-window"))
        {
            setMessage (mb::tr ("启动新实例失败", "Failed to start a new instance"));
            return;
        }
        setMessage (mb::tr ("已另起一个微音桥（独立进程）",
                            "New MicroBridge instance launched"));
    }

    void restartApplication()
    {
        const auto exe = appExecutable();
        if (! exe.existsAsFile())
        {
            setMessage (mb::tr ("重启失败：找不到程序文件", "Restart failed: exe not found"));
            return;
        }
        saveSessionQuietly();                 // 先把当前会话落盘，避免新旧进程抢写
        presetDirty_ = false;                 // 不再弹「未保存改动」
        if (! exe.startAsProcess ("--restart-wait"))
        {
            setMessage (mb::tr ("重启失败：无法启动新进程", "Restart failed: cannot launch"));
            return;
        }
        // 延后到本次点击回调返回后再退出，避免在按钮 onClick 栈里销毁主窗口。
        juce::MessageManager::callAsync ([]
        {
            if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
        });
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (mb::ui::background);
        if (! folded_) drawDrawer (g);   // 折叠时抽屉整体隐藏，不画左侧栏背景
    }

    void resized() override
    {
        const int w = getWidth();
        const int drawerW = drawerW_;
        const int recH = RecorderBar::kHeight;

        // 左侧抽屉：工具按钮竖排（顶栏已并入原生标题栏，从窗口顶开始）；「设置」固定在底部
        int by = 6;
        const int bw = drawerW - 10;
        addBtn_.setBounds        (5, by, bw, 30); by += 34;
        newBtn_.setBounds        (5, by, bw, 30); by += 34;
        savePresetBtn_.setBounds (5, by, bw, 30); by += 34;
        importBtn_.setBounds     (5, by, bw, 30); by += 34;
        exportBtn_.setBounds     (5, by, bw, 30); by += 34;
        clearBtn_.setBounds      (5, by, bw, 30); by += 34;
        settingsBtn_.setBounds   (5, getHeight() - 40, bw, 30);
        toggleBtn_.setBounds     (5, getHeight() - 74, bw, 30);
        foldBtn_.setBounds       (5, getHeight() - 108, bw, 30);
        recBtn_.setBounds        (5, getHeight() - 142, bw, 30);
        viewBtn_.setBounds       (5, getHeight() - 176, bw, 30);
        themeBtn_.setBounds      (5, getHeight() - 210, bw, 30);

        // 折叠时抽屉整体隐藏（展开入口改为标题栏下拉箭头），展开时恢复
        for (auto* b : { &addBtn_, &newBtn_, &savePresetBtn_, &importBtn_, &exportBtn_,
                         &clearBtn_, &settingsBtn_, &toggleBtn_, &viewBtn_,
                         &themeBtn_, &recBtn_, &foldBtn_ })
            b->setVisible (! folded_);

        if (folded_)
        {
            // 卷起折叠：隐藏走带条 / 节点视图，视口铺满全宽，并滚到底只露
            // 通道底部 M/S/R/喇叭 + 名称带（高度见 foldedContentHeight）。
            recorderBar_.setVisible (false);
            nodeViewport_.setBounds (0, 0, 0, 0);
            viewport_.setBounds (0, 0, w, getHeight());
            layoutStrips();
            // 通道条高于视口，把底部控制带滚入可视区
            const int over = viewport_.getViewedComponent()->getHeight() - viewport_.getHeight();
            viewport_.setViewPosition (0, juce::jmax (0, over));
        }
        else if (nodeViewMode_)
        {
            // 节点视图：走带条与通道区隐藏，整个主区交给三列 patchbay（可纵向滚动）
            recorderBar_.setBounds (0, 0, 0, 0);
            recorderBar_.setVisible (false);
            viewport_.setBounds (0, 0, 0, 0);
            nodeViewport_.setBounds (drawerW, 0, w - drawerW, getHeight());
            // 窗口宽度随通道数自动变化：让三列按主区宽度重排，输出列右侧不再留大片空白
            nodeCanvas_.setAvailableWidth (w - drawerW);
        }
        else
        {
            nodeViewport_.setBounds (0, 0, 0, 0);
            if (recorderVisible_)
            {
                // 录音机：通道区顶部横条，横跨主区宽度
                recorderBar_.setVisible (true);
                recorderBar_.setBounds (drawerW, 0, w - drawerW, recH);
                // 通道条区域下移，避开走带条
                viewport_.setBounds (getLocalBounds()
                                      .withTrimmedLeft (drawerW)
                                      .withTrimmedTop (recH));
            }
            else
            {
                // 隐藏录音区：走带条收起，调音台通道顶满主区
                recorderBar_.setVisible (false);
                viewport_.setBounds (getLocalBounds().withTrimmedLeft (drawerW));
            }
            viewport_.setViewPosition (0, 0);   // 展开时回到顶部，避免残留折叠时的底部滚动
            layoutStrips();
        }

        if (welcome_ != nullptr) welcome_->setBounds (getLocalBounds());
        if (setup_ != nullptr) setup_->setBounds (getLocalBounds());
        if (linkOverlay_ != nullptr) linkOverlay_->setBounds (getLocalBounds());
        if (sourcePicker_ != nullptr) sourcePicker_->setBounds (getLocalBounds());
        if (pluginPicker_ != nullptr) pluginPicker_->setBounds (getLocalBounds());
        if (fxPanel_ != nullptr) fxPanel_->setBounds (getLocalBounds());
        if (mediaPanel_ != nullptr) mediaPanel_->setBounds (getLocalBounds());
        if (diagPanel_ != nullptr) diagPanel_->setBounds (getLocalBounds());
        if (settingsPanel_ != nullptr) settingsPanel_->setBounds (getLocalBounds());
        if (colourPicker_ != nullptr) colourPicker_->setBounds (getLocalBounds());
    }

    void timerCallback() override
    {
        for (auto& s : strips_) s->repaintMeters();  // LED 窄条重绘 + 静默通道跳过
        if (nodeViewMode_ && ! folded_) nodeCanvas_.repaint();  // 节点连线电平光点（折叠时隐藏）
        refreshWindowTitle();            // 状态 / 临时消息到期 / 语言变化 → 原生标题跟随（内部去重）

        // 通道快捷键 / 设置页隐藏热键可能从多处被改动（弹窗确定、会话载入、
        // 通道增删与重排），用指纹统一检测，变化即重建全局钩子绑定。
        if (currentHotkeyDigest() != hotkeyDigest_)
            refreshGlobalHotkeys();

        // 录音机走带条
        engine_.pluginBank().flushRetired();       // 安全清理退役插件实例
        refreshRecorderBar();

        // 参考主流机架：周期性重设音频线程亲和性（约 11 s 一次）
        if (++schedTick_ >= 220)
        {
            schedTick_ = 0;
            engine_.rebalanceAudioThread();
        }

        // 插件内部状态变化（S-Rack 导入歌曲 / 改播放列表）不会通知宿主，
        // 每 5 s 取一次状态指纹，变化即让会话自动保存写出新的 state chunk。
        if (engine_.isReady() && ++stateTick_ >= 100)
        {
            stateTick_ = 0;
            const auto digest = engine_.pluginBank().stateDigest();
            if (digest != lastPluginDigest_)
            {
                lastPluginDigest_ = digest;
                dirty_ = true;
            }
        }

        // 会话自动保存：改动后约 2 s 落盘
        if (dirty_ && engine_.isReady())
        {
            if (++saveTick_ >= 40)
            {
                saveTick_ = 0;
                dirty_ = false;
                mb::SessionStore::save (mb::SessionStore::sessionFile(),
                                    mb::SessionStore::toJson (graph_, engine_, nodeCanvas_.layoutToJson()));
            }
        }
    }

private:
    //==========================================================================
    void setupButton (DrawerButton& b, DrawerButton::Icon icon, const juce::String& text, std::function<void()> fn)
    {
        b.icon = icon;
        b.setButtonText (text);
        b.setTooltip (text);
        b.setColour (juce::TextButton::buttonColourId,   mb::ui::card);
        b.setColour (juce::TextButton::buttonOnColourId, mb::ui::accent[0]);
        b.setColour (juce::TextButton::textColourOffId,  mb::ui::text);
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        b.onClick = std::move (fn);
        addAndMakeVisible (b);
    }

    /** 抽屉全部按钮的「显示文字 + 悬停提示」统一按当前语言重建。
        语言切换 / 抽屉收纳 / 主题切换都走这里，避免只有再次点击才更新。 */
    void refreshDrawerTexts()
    {
        auto set = [] (DrawerButton& b, const juce::String& t)
        {
            b.setButtonText (t);
            b.setTooltip (t);
        };
        set (addBtn_,        mb::tr ("添加通道", "Add"));
        set (newBtn_,        mb::tr ("新建预设", "New"));
        set (savePresetBtn_, mb::tr ("保存预设", "Save"));
        set (importBtn_,     mb::tr ("导入预设", "Import"));
        set (exportBtn_,     mb::tr ("导出预设", "Export"));
        set (clearBtn_,      mb::tr ("清除连接", "Clear"));
        set (settingsBtn_,   mb::tr ("设置", "Settings"));
        // 节点视图按钮：处于节点视图时文案变为「经典视图」（点击返回调音台），
        // 图标随视图切换：节点视图态显示调音台推子图标（Mixer），调音台态显示节点图标（Nodes）
        viewBtn_.icon = nodeViewMode_ ? DrawerButton::Mixer : DrawerButton::Nodes;
        set (viewBtn_,       nodeViewMode_ ? mb::tr ("经典视图", "Classic view")
                                           : mb::tr ("节点视图", "Node view"));
        set (themeBtn_,      themeBtnText());
        set (recBtn_,         recBtnText());
        set (toggleBtn_,     drawerCollapsed_ ? mb::tr ("展开抽屉", "Expand drawer")
                                              : mb::tr ("收纳抽屉", "Collapse drawer"));
        set (foldBtn_,       folded_ ? mb::tr ("展开窗口", "Expand window")
                                     : mb::tr ("折叠窗口", "Fold window"));
    }

    //==========================================================================
    // 内置插件浮层状态持久化（settings.json；重开面板 / 重启应用都恢复）
    //==========================================================================
    static mb::CardWindowState toSettingsCard (const FloatingCardPanel::CardState& s)
    {
        mb::CardWindowState c;
        c.hasSize = s.hasSize;  c.hasPos = s.hasPos;
        c.w = s.w;  c.h = s.h;  c.x = s.x;  c.y = s.y;
        c.minimised = s.minimised;  c.maximised = s.maximised;
        return c;
    }

    static FloatingCardPanel::CardState fromSettingsCard (const mb::CardWindowState& c)
    {
        FloatingCardPanel::CardState s;
        s.hasSize = c.hasSize;  s.hasPos = c.hasPos;
        s.w = c.w;  s.h = c.h;  s.x = c.x;  s.y = c.y;
        s.minimised = c.minimised;  s.maximised = c.maximised;
        return s;
    }

    void saveFxCardState()
    {
        if (fxPanel_ == nullptr) return;
        settings_.fxCard = toSettingsCard (fxPanel_->cardState());
        mb::AppSettings::save (settings_);
    }

    void saveMediaCardState()
    {
        if (mediaPanel_ == nullptr) return;
        settings_.mediaCard = toSettingsCard (mediaPanel_->cardState());
        mb::AppSettings::save (settings_);
    }

    void drawDrawer (juce::Graphics& g)
    {
        const int drawerW = drawerW_;
        const int h = getHeight();

        g.setColour (mb::ui::ribbon);
        g.fillRect (0, 0, drawerW, h);
        g.setColour (mb::ui::border);
        g.fillRect (drawerW - 1, 0, 1, h);
    }

    /** 应用抽屉收纳状态（启动恢复 / 手动切换共用，不含窗口宽度联动）。 */
    void applyDrawerState (bool collapsed)
    {
        drawerCollapsed_ = collapsed;
        drawerW_ = drawerCollapsed_ ? 44 : mb::ui::kDrawerWidth;
        toggleBtn_.icon = drawerCollapsed_ ? DrawerButton::Expand : DrawerButton::Collapse;
        toggleBtn_.iconOnly = drawerCollapsed_;   // 44px 窄抽屉只显示图标，避免文字溢出
        for (auto* b : { &addBtn_, &newBtn_, &savePresetBtn_, &importBtn_, &exportBtn_,
                         &clearBtn_, &settingsBtn_, &viewBtn_, &themeBtn_, &recBtn_,
                         &foldBtn_ })
            b->iconOnly = drawerCollapsed_;
        refreshDrawerTexts();       // 折叠/展开抽屉时文案与 tooltip 一并跟随语言
    }

    /** 收纳 / 展开抽屉：收纳时只显示按钮图标并收窄宽度。 */
    void toggleDrawer()
    {
        applyDrawerState (! drawerCollapsed_);
        settings_.drawerCollapsed = drawerCollapsed_;   // 持久化：重启恢复收纳状态
        mb::AppSettings::save (settings_);

        // 窗口宽度跟随抽屉：收纳变窄、展开变宽（preferredWidth 含 drawerW_）。
        // refreshWindowWidth 内部 setSize 只在宽度变化时触发 resized()，
        // 所以这里保留手动 resized() 兜底宽度被 clamp 住没变的情况。
        refreshWindowWidth();
        resized();
        repaint (0, 0, drawerW_ + 10, getHeight());
    }

    //==========================================================================
    // 原生窗口标题：品牌名（跟随语言）+ 驱动 / 设备 / 延迟 / 连接数。
    // 临时消息（2.6 s 内）直接顶替标题尾部，到期自动回落到状态摘要。
    juce::String composeWindowTitle() const
    {
        if (juce::Time::getMillisecondCounter() < messageExpiry_ && message_.isNotEmpty())
            return mb::appName() + mb::S("  —  ") + message_;

        const auto driver = engine_.driverTypeName();
        const double sr = engine_.sampleRate();
        const int bs = engine_.bufferSizeSamples();
        const double latencyMs = sr > 0.0 ? static_cast<double> (bs) * 1000.0 / sr : 0.0;

        // deviceInfo() 自带「设备名 · 采样率 · 缓冲帧」，这里只补延迟，避免重复
        juce::String t = mb::appName() + mb::S("  —  ")
                       + (driver.isEmpty() ? mb::tr ("未设置", "NOT SET") : driver)
                       + mb::S("  ·  ") + engine_.deviceInfo();
        if (sr > 0.0)
            t << mb::S("  ·  ") << juce::String (latencyMs, 1) << mb::S(" ms");
        t << mb::S("  ·  ") << mb::tr ("连接", "EDGES") << mb::S(" ")
          << juce::String (graph_.plan()->edgeCount());
        const int underruns = engine_.auxUnderruns();
        if (underruns > 0)
            t << mb::S("  ·  ") << mb::tr ("欠载", "UNDERRUN") << mb::S(" ")
              << juce::String (underruns);
        return t;
    }

    //==========================================================================
    // 端口对：目录里每台设备固定提供 L / R 两个端口，成对使用才是立体声。
    struct PortPair
    {
        int l = -1, r = -1, pair = -1;
        bool valid() const noexcept { return l >= 0 && r >= 0; }
    };

    std::vector<PortPair> collectPortPairs (bool input) const
    {
        const auto& ports = input ? engine_.catalog().inputPorts()
                                  : engine_.catalog().outputPorts();
        std::vector<PortPair> out;
        for (size_t i = 0; i + 1 < ports.size(); ++i)
        {
            if (ports[i].input == input && ports[i + 1].input == input
                && ports[i].deviceIndex == ports[i + 1].deviceIndex
                && ports[i].channelIndex % 2 == 0
                && ports[i + 1].channelIndex == ports[i].channelIndex + 1)
            {
                out.push_back ({ static_cast<int> (i), static_cast<int> (i + 1),
                                static_cast<int> (out.size()) });
                ++i;
            }
        }
        return out;
    }

    /** 按设备名关键字挑一对未占用的端口；requireKey=true 时匹配不到就不连，
        避免把不相干的驱动误当麦克风 / 队友语音 / 语音软件输出。 */
    PortPair pickPortPair (const std::vector<PortPair>& pairs,
                           std::vector<bool>& used,
                           bool input,
                           const std::vector<const char*>& keys,
                           bool requireKey) const
    {
        const auto& ports = input ? engine_.catalog().inputPorts()
                                  : engine_.catalog().outputPorts();
        for (const auto* key : keys)
        {
            const auto needle = mb::S (key);
            for (const auto& p : pairs)
                if (! used[static_cast<size_t> (p.pair)]
                    && ports[static_cast<size_t> (p.l)].deviceName.containsIgnoreCase (needle))
                {
                    used[static_cast<size_t> (p.pair)] = true;
                    return p;
                }
        }
        if (requireKey) return {};
        for (const auto& p : pairs)
            if (! used[static_cast<size_t> (p.pair)])
            {
                used[static_cast<size_t> (p.pair)] = true;
                return p;
            }
        return {};
    }

    //==========================================================================
    /** 直播 / 游戏默认配线：点「重置路由」、全新安装（无设备存档）时走这里。
        注意：更换主驱动不再走这里 —— 换驱动后通道保持空白，不自动选输入/输出。
        三条总线物理隔离，是「不卡麦」的关键：
          · CH7 直播总线 = 游戏声 + 队友语音 + 处理后人声 → 直播捕获设备
          · CH8 语音总线 = 只有麦克风                  → 游戏/语音软件麦克风
          · CH9 监听     = 游戏声 + 队友语音（不含自己麦克风，防啸叫）
        队友语音绝不回到语音总线，游戏声也不经过任何人声效果链。 */
    void applyDefaultRoutes()
    {
        const int inputs  = engine_.catalog().inputPortCount();
        const int outputs = engine_.catalog().outputPortCount();

        // 这套配线需要 9 条通道；全新安装默认只有 6 条，补齐并重建通道条。
        if (graph_.channelCount() < 9)
        {
            graph_.resize (9, inputs, outputs);
            syncChannelNames();
            syncAlignmentVector();
            if (! strips_.empty()) rebuildStrips();
        }
        else
        {
            graph_.clearEdges();
            graph_.setPortCounts (inputs, outputs);
        }

        // 预设体验 5a：语义通道名只随这套默认配线显式写入。channelName 本身
        // 只给 CH 编号，用户自建空白预设不会看到「系统/伴奏/麦克风」语义名。
        {
            auto& names = mb::customChannelNames();
            const int n = juce::jmin (9, graph_.channelCount());
            while (names.size() < n) names.add (juce::String());
            for (int c = 0; c < n; ++c)
                names.set (c, mb::defaultLiveChannelName (c));
        }

        const auto inPairs  = collectPortPairs (true);
        const auto outPairs = collectPortPairs (false);
        std::vector<bool> usedIn (inPairs.size(), false), usedOut (outPairs.size(), false);

        // 端口 L/R 配对同步给路由内核：立体声通道据此把 (L 端口,R 端口) 映射到
        // (L 缓冲,R 缓冲)，否则两路会被误判为「居中」丢失声像。
        std::vector<std::pair<int, int>> rawIn, rawOut;
        rawIn.reserve (inPairs.size());
        rawOut.reserve (outPairs.size());
        for (const auto& p : inPairs)  rawIn.push_back ({ p.l, p.r });
        for (const auto& p : outPairs) rawOut.push_back ({ p.l, p.r });
        graph_.setPortPairs (rawIn, rawOut);

        // 四个独立输入源：按设备名找，找不到宁可留空也不乱接（避免杂音）。
        const auto game = pickPortPair (inPairs, usedIn, true,
            { "B1 Output", "Voicemeeter", "游戏", "Game", "系统", "System" }, false);
        const auto mic = pickPortPair (inPairs, usedIn, true,
            { "麦克风", "Microphone", "Mic", "话筒" }, true);
        const auto chat = pickPortPair (inPairs, usedIn, true,
            { "B2", "A2", "Discord", "队友", "聊天", "Chat", "Voice", "CABLE Output" }, true);
        const auto music = pickPortPair (inPairs, usedIn, true,
            { "CABLE", "伴奏", "音乐", "Music", "Backing", "媒体" }, true);

        // 输出：先锁定耳机/扬声器做监听，再给直播与语音软件分配独立设备。
        const auto monitor = pickPortPair (outPairs, usedOut, false,
            { "扬声器", "Speaker", "Headphone", "耳机", "Wuying" }, false);
        const auto live = pickPortPair (outPairs, usedOut, false,
            { "Voicemeeter Input", "CABLE Input", "B2", "Stream", "OBS", "直播", "Virtual" }, false);
        const auto voice = pickPortPair (outPairs, usedOut, false,
            { "Voicemeeter", "B1", "CABLE", "语音", "Chat", "Voice", "Discord" }, true);

        std::string why;
        auto feed = [&] (int ch, const PortPair& p, float gain)
        {
            if (ch < 0 || ch >= graph_.channelCount() || ! p.valid()) return;
            graph_.tryAddEdge ({ p.l, ch, mb::EdgeKind::InputToChannel, gain }, why);
            graph_.tryAddEdge ({ p.r, ch, mb::EdgeKind::InputToChannel, gain }, why);
        };
        auto routeOut = [&] (int ch, const PortPair& p, float gain)
        {
            if (ch < 0 || ch >= graph_.channelCount() || ! p.valid()) return;
            graph_.tryAddEdge ({ ch, p.l, mb::EdgeKind::ChannelToOutput, gain }, why);
            graph_.tryAddEdge ({ ch, p.r, mb::EdgeKind::ChannelToOutput, gain }, why);
        };
        auto send = [&] (int from, int to, float gain)
        {
            if (from >= 0 && to >= 0 && from < graph_.channelCount()
                && to < graph_.channelCount())
                graph_.tryAddEdge ({ from, to, mb::EdgeKind::ChannelToChannel, gain }, why);
        };

        // ① 四个独立输入源（0-based：0 系统 / 1 伴奏 / 2 麦克风 / 3 聊天）
        feed (0, game,  1.0f);     // 游戏 / 系统声
        feed (1, music, 0.8f);     // 伴奏（可选，找不到就留空）
        feed (2, mic,   1.0f);     // 麦克风
        feed (3, chat,  0.9f);     // 队友语音

        // ② 游戏声：直送直播总线与本地监听，完全不经过人声效果链
        send (0, 6, 1.0f);
        send (0, 8, 1.0f);
        // ③ 队友语音：观众能听到、自己能听到，但绝不回送语音软件（不卡麦）
        send (3, 6, 0.9f);
        send (3, 8, 0.9f);
        // ④ 麦克风：一路干净送队友，一路进人声链给观众
        send (2, 7, 1.0f);
        send (2, 4, 1.0f);
        send (1, 4, 0.7f);
        send (4, 5, 1.0f);
        send (5, 6, 0.9f);

        // ⑤ 三条总线各输出到不同设备；只有一套输出设备时，直播总线兼作
        //    监听（云电脑里 OBS 抓的就是它），不再把同一路声音写第二遍。
        const auto liveOut = live.valid() ? live : monitor;
        routeOut (6, liveOut, 1.0f);
        routeOut (7, voice,  1.0f);
        if (live.valid() && monitor.valid() && monitor.l != live.l)
            routeOut (8, monitor, 0.9f);

        engine_.syncDevices();
        for (auto& s : strips_) s->syncFromGraph();
        updateSummary();

        juce::String note = mb::tr ("已恢复直播默认配线 · 游戏/队友/麦克风三线隔离",
                                    "Live-game default patch · 3 isolated buses");
        if (! mic.valid() || ! chat.valid() || ! voice.valid())
            note += mb::tr ("（部分设备未识别，请手动补齐输入/输出）",
                            " (some devices not found, patch manually)");
        setMessage (note);
    }

    /** 图结构变化后（节点视图建/删线、通道菜单 C→C 送出/接收等）刷新全部
        通道条：连接列表 / 按钮状态随图重读，静态层按需重建。 */
    void refreshStripsFromGraph()
    {
        for (auto& s : strips_) s->syncFromGraph();
    }

    void clearRoutes()
    {
        graph_.clearEdges();
        engine_.syncDevices();
        for (auto& s : strips_) s->syncFromGraph();
        updateSummary();
        setMessage (mb::tr ("已清除全部连接", "All connections cleared"));
    }

    void rebuildStrips()
    {
        for (auto& s : strips_)
            stripContainer_.removeChildComponent (s.get());
        strips_.clear();

        for (int i = 0; i < graph_.channelCount(); ++i)
        {
            auto s = std::make_unique<ChannelStrip> (i, graph_, engine_);
            s->onMessage = [this] (const juce::String& m) { setMessage (m); };
            s->onChanged = [this]
            {
                engine_.syncDevices();
                refreshStripsFromGraph();     // 本通道送出 C→C 后对方条立即刷新
                updateSummary();
            };
            s->onShowFx = [this] (int ch) { showFx (ch); };
            s->onOpenPlugin = [this] (int ch, int idx)
            {
                // 自带插件 → 打开该效果自己的面板（音频播放 → 播放器面板）；
                // 第三方 → 打开插件编辑器窗口
                if (const auto bid = engine_.pluginBank().pluginBuiltInId (ch, idx); bid > 0)
                {
                    if (bid == 9) { showMediaPlayer (ch); return; }
                    showFx (ch, bid);
                    return;
                }
                openPluginEditor (ch, idx);
            };
            s->onPluginOffline = [this] (int ch, int idx)
            {
                // 离线前钩子：实例即将退役，编辑器窗口持有裸指针必须先关
                closePluginEditor (ch, idx);
            };
            s->onRemovePlugin = [this] (int ch, int idx)
            {
                // 自带插件：移除时同步关闭内置效果链对应模块
                const auto bid = engine_.pluginBank().pluginBuiltInId (ch, idx);
                if (bid > 0)
                {
                    if (bid == 9)
                    {
                        destroyMediaPlayer();
                        engine_.mediaPlayers().clearPlayer (ch);
                    }
                    else
                    {
                        auto params = engine_.fxParams (ch);
                        mb::applyBuiltInToParams (params, bid, false);
                        engine_.setFxParams (ch, params);
                        closeFxPanel();
                    }
                }
                closePluginEditor (ch, idx);
                engine_.pluginBank().removePlugin (ch, idx);
            };
            s->onReorder = [this] (int from, int to) { reorderChannels (from, to); };
            wireDragCallbacks (s.get());
            s->onDeleteChannel = [this] (int ch) { deleteChannel (ch); };
            s->onDuplicateChannel = [this] (int ch) { duplicateChannel (ch); };
            s->onSetRightAligned = [this] (int ch, bool on) { applyRightAligned (ch, on); };
            s->onLinkSetState = [this] (int ch, int action, bool on) { applyLinkState (ch, action, on); };
            s->onLinkVolume   = [this] (int ch, float gain) { applyLinkVolume (ch, gain); };
            s->onLinkChannel  = [this] (int ch, int defAction) { showChannelLinkDialog (ch, defAction); };
            s->onSetHidden = [this] (int ch, bool hidden) { setChannelHidden (ch, hidden); };
            s->onShowAllChannels = [this] { showAllChannels(); };
            s->onActivateSlot = [this] (int ch) { activateSlot (ch); };
            s->onPickCustomColour = [this] (int ch) { showColourPicker (ch); };
            s->onOpenSource = [this] (int ch, bool input) { showSourcePicker (ch, input); };
            s->onOpenPlugins = [this] (int ch) { showPluginPicker (ch); };
            s->onSlotToChannel = [this] (int slot) { return slotToChannel (slot); };
            s->onVisibleCount = [this] { return visibleSlotCount(); };
            stripContainer_.addAndMakeVisible (s.get());
            strips_.push_back (std::move (s));
        }
        layoutStrips();
        nodeCanvas_.updateSize();            // 通道数变化 → 节点画布高度重算
        updateSummary();
    }

    void layoutStrips()
    {
        const int viewH = juce::jmax (300, viewport_.getHeight() - 2);
        const int vw = viewport_.getWidth();

        // 折叠态统一在这里同步（覆盖切换折叠、新建/重建通道等所有入口）：
        // 通道条只露底部控制带，不画推子卡与电平表。
        for (auto& s : strips_) s->setFoldedCompact (folded_);

        // 统计可见通道与右对齐组规模
        int shownCount = 0, rightShown = 0;
        for (size_t i = 0; i < strips_.size(); ++i)
            if (mb::ui::slotState (static_cast<int> (i)) == mb::ui::kSlotNormal)
            {
                ++shownCount;
                if (mb::ui::isChannelRightAligned (static_cast<int> (i))) ++rightShown;
            }
        const int leftShown = shownCount - rightShown;
        const int needed = juce::jmax (1, shownCount) * mb::ui::kStripWidth;

        // 两组并存时的最小间距 = 两条斑马带紧靠 + 两侧安全留白（26px）。
        // 窗口宽度不足时**不压缩间距**，而是把容器加宽产生横向滚动，
        // 保证斑马带永不被通道条覆盖、也不与通道边框折叠；宽度富余
        // （拉宽 / 最大化窗口）时未对齐组靠左、右对齐组贴右缘，间距随窗口拉大。
        const int kGroupGapMin = StripContainer::kMinGap;
        const bool both = rightShown > 0 && leftShown > 0;
        layPinned_ = both && vw >= needed + kGroupGapMin;
        layLeftCount_ = leftShown;
        layRightX0_ = ! both ? leftShown * mb::ui::kStripWidth
                     : layPinned_ ? vw - rightShown * mb::ui::kStripWidth
                                  : leftShown * mb::ui::kStripWidth + kGroupGapMin;

        int li = 0, ri = 0;
        for (size_t i = 0; i < strips_.size(); ++i)
        {
            auto* s = strips_[i].get();
            // 隐藏通道与删除后留下的空槽都不占可见位置：右侧通道自动紧靠
            // 左侧通道，中间不再出现空白区域。空槽仍保留在数据中，供「+通道」复用。
            const bool shown = mb::ui::slotState (static_cast<int> (i)) == mb::ui::kSlotNormal;
            s->setVisible (shown);
            if (! shown) { s->setBounds (0, 0, 0, 0); continue; }
            // 右对齐通道恒在序列末尾（重排保证）：pinned 时贴右缘，否则顺排
            if (layPinned_ && mb::ui::isChannelRightAligned (static_cast<int> (i)))
            {
                s->setBounds (layRightX0_ + ri * mb::ui::kStripWidth, 0,
                              mb::ui::kStripWidth, viewH);
                ++ri;
            }
            else
            {
                s->setBounds (li * mb::ui::kStripWidth, 0, mb::ui::kStripWidth, viewH);
                ++li;
            }
        }
        // 分隔区几何：左组右缘 → 右组左缘，交给容器层画斑马线
        stripContainer_.groupSplit = both;
        stripContainer_.splitX0 = layLeftCount_ * mb::ui::kStripWidth;
        stripContainer_.splitX1 = layRightX0_;
        stripContainer_.setSize (juce::jmax (vw, needed + (both ? kGroupGapMin : 0)), viewH);
        stripContainer_.repaint();
        dragOverlay_.setBounds (0, 0, stripContainer_.getWidth(), viewH);
    }

    // layoutStrips 的分组几何缓存（拖拽槽位映射用）
    int layLeftCount_ = 0, layRightX0_ = 0;
    bool layPinned_ = false;

    //==========================================================================
    // 拖拽特效：通道条本体高亮 + 浮层动画。数据换位仍在 reorderChannels 里完成。
    int visibleSlotFromX (float x) const
    {
        // 与 layoutStrips 的分组几何一致：pinned 时越过组间距再换算右组槽位
        const float xf = juce::jmax (0.0f, x);
        int slot;
        if (! layPinned_ || xf < static_cast<float> (layLeftCount_ * mb::ui::kStripWidth))
            slot = juce::roundToInt (xf / static_cast<float> (mb::ui::kStripWidth));
        else
            slot = layLeftCount_
                 + juce::roundToInt ((xf - static_cast<float> (layRightX0_))
                                     / static_cast<float> (mb::ui::kStripWidth));
        return juce::jlimit (0, visibleSlotCount() - 1, slot);
    }

    void clearDragHighlight()
    {
        for (auto& s : strips_) s->setDragging (false);
        dragChannel_ = -1;
    }

    /** 拖拽特效回调集中接线：rebuildStrips 只需一行，便于并行编辑时恢复。 */
    void wireDragCallbacks (ChannelStrip* s)
    {
        s->onDragBegin = [this] (int ch, float x, float y, int slot)
        {
            beginStripDrag (ch, x, y, slot);
        };
        s->onDragMove = [this] (float x, float y)
        {
            dragOverlay_.move (x, y, visibleSlotFromX (x));
        };
        s->onDragFlash = [this] (int fromSlot, int toSlot)
        {
            dragOverlay_.flash (fromSlot, toSlot);
        };
        s->onDragEnd = [this] { endStripDrag(); };
    }

    void beginStripDrag (int ch, float x, float y, int slot)
    {
        if (ch < 0 || ch >= static_cast<int> (strips_.size())) return;
        clearDragHighlight();
        strips_[static_cast<size_t> (ch)]->setDragging (true);
        dragChannel_ = ch;
        dragOverlay_.begin (mb::ui::stripColour (ch), mb::channelDisplayName (ch),
                            x, y, slot);
    }

    void endStripDrag()
    {
        clearDragHighlight();
        dragOverlay_.finish();
    }

    //==========================================================================
    // 通道拖动重排：from 位置移到 to。路由 / 插件链 / 名字 / 颜色 / 布防 /
    // 内置效果器参数全部跟随移动，通道条只改索引不重建。
    void reorderChannels (int from, int to)
    {
        const int n = graph_.channelCount();
        if (from == to || from < 0 || from >= n || to < 0 || to >= n) return;

        // 右对齐组不能与普通通道交叉：组内可自由左右拖动调整顺序，
        // 普通通道只在组的左侧移动。这样「新建通道永远排在右对齐组左边」恒成立。
        const int boundary = firstRightAlignedIndex();
        if (mb::ui::isChannelRightAligned (from))
            to = juce::jlimit (boundary, n - 1, to);
        else
            to = juce::jlimit (0, juce::jmax (0, boundary - 1), to);
        if (from == to) return;

        // order[newPos] = oldIdx：把 from 抽出再插入 to
        std::vector<int> order;
        order.reserve (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i) order.push_back (i);
        order.erase (order.begin() + from);
        order.insert (order.begin() + to, from);

        applyChannelOrder (order);

        // 数据是「搬运」而非「组件移动」：高亮要跟着通道实体落到新槽位，
        // 否则拖动中会出现「框留在原地、内容跑掉」的错觉。
        if (dragChannel_ >= 0 && to < static_cast<int> (strips_.size()))
        {
            for (auto& s : strips_) s->setDragging (false);
            strips_[static_cast<size_t> (to)]->setDragging (true);
            dragChannel_ = to;
        }
        setMessage (mb::tr ("通道已移动", "Channel moved"));
    }

    /** order[newPos] = oldIdx：把图、插件链、名字、颜色、布防、内置效果器参数、
        槽位状态与通道条索引按同一置换整体重排。删除通道后的自动补位也走这里，
        避免出现「右侧通道看起来被删掉」的错位。 */
    void applyChannelOrder (const std::vector<int>& order)
    {
        const int n = graph_.channelCount();
        if (static_cast<int> (order.size()) != n
            || static_cast<int> (strips_.size()) != n) return;

        graph_.reorderChannels (order);
        engine_.pluginBank().reorderChannels (order);
        engine_.mediaPlayers().reorderChannels (order);

        // 名字 / 颜色 / 布防 / 内置效果器参数跟随
        juce::StringArray names;
        for (int i = 0; i < n; ++i)
        {
            const int old = order[static_cast<size_t> (i)];
            names.add (old < mb::customChannelNames().size()
                       ? mb::customChannelNames()[old] : juce::String());
        }
        mb::customChannelNames() = names;

        // 图标与名字同路径跟随通道实体移动
        juce::StringArray icons;
        for (int i = 0; i < n; ++i)
        {
            const int old = order[static_cast<size_t> (i)];
            icons.add (old < mb::customChannelIcons().size()
                       ? mb::customChannelIcons()[old] : juce::String());
        }
        mb::customChannelIcons() = icons;

        // 颜色随通道实体移动：写入「该通道当前显示色」（自定义优先，否则默认），
        // 而非按槽位编号取色，拖动换位后颜色牢牢跟随通道，不会错位到新位置。
        std::vector<juce::Colour> cols (static_cast<size_t> (n), juce::Colour());
        for (int i = 0; i < n; ++i)
            cols[static_cast<size_t> (i)] = mb::ui::stripColour (order[static_cast<size_t> (i)]);
        mb::ui::customChannelColours() = cols;

        std::vector<std::uint8_t> arm (static_cast<size_t> (n), 0);
        {
            const auto& src = mb::ui::channelArmState();
            for (int i = 0; i < n; ++i)
                if (order[static_cast<size_t> (i)] < static_cast<int> (src.size()))
                    arm[static_cast<size_t> (i)] = src[static_cast<size_t> (order[static_cast<size_t> (i)])];
        }
        mb::ui::channelArmState() = arm;

        // 「向右对齐」标记跟随通道实体移动，重排后分组不会错位
        std::vector<std::uint8_t> aligned (static_cast<size_t> (n), 0);
        {
            const auto& src = mb::ui::rightAlignedState();
            for (int i = 0; i < n; ++i)
                if (order[static_cast<size_t> (i)] < static_cast<int> (src.size()))
                    aligned[static_cast<size_t> (i)] =
                        src[static_cast<size_t> (order[static_cast<size_t> (i)])];
        }
        mb::ui::rightAlignedState() = aligned;

        // 「对齐前位置」记忆跟随通道实体移动；未对齐通道的记忆清空
        std::vector<int> origins (static_cast<size_t> (n), -1);
        {
            const auto& src = mb::ui::rightAlignedOrigin();
            for (int i = 0; i < n; ++i)
                if (order[static_cast<size_t> (i)] < static_cast<int> (src.size()))
                    origins[static_cast<size_t> (i)] = src[static_cast<size_t> (order[static_cast<size_t> (i)])];
            for (int i = 0; i < n; ++i)
                if (aligned[static_cast<size_t> (i)] == 0) origins[static_cast<size_t> (i)] = -1;
        }
        mb::ui::rightAlignedOrigin() = origins;

        // 通道制式（单/立体声）跟随通道实体移动，电平表条数不会错位
        std::vector<std::uint8_t> stereoFlags (static_cast<size_t> (n), 0);
        {
            const auto& src = mb::ui::channelStereoState();
            for (int i = 0; i < n; ++i)
                if (order[static_cast<size_t> (i)] < static_cast<int> (src.size()))
                    stereoFlags[static_cast<size_t> (i)] =
                        src[static_cast<size_t> (order[static_cast<size_t> (i)])];
        }
        mb::ui::channelStereoState() = stereoFlags;

        // 空槽 / 隐藏状态跟随通道实体移动，避免拖动后出现「数据跑到空槽里」
        std::vector<std::uint8_t> slots (static_cast<size_t> (n), 0);
        for (int i = 0; i < n; ++i)
            slots[static_cast<size_t> (i)] = mb::ui::slotState (order[static_cast<size_t> (i)]);
        for (int i = 0; i < n; ++i)
            mb::ui::setSlotState (i, slots[static_cast<size_t> (i)]);

        using FxParamsT = std::remove_cv_t<std::remove_reference_t<decltype (engine_.fxParams (0))>>;
        std::vector<FxParamsT> fx;
        fx.reserve (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            fx.push_back (engine_.fxParams (order[static_cast<size_t> (i)]));

        // 通道链接码跟随通道实体移动；重排/装载期间抑制 FX 联动钩子
        std::vector<mb::links::LinkSet> links (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            links[static_cast<size_t> (i)] = mb::links::linkSetOf (order[static_cast<size_t> (i)]);

        {
            juce::ScopedValueSetter<bool> guard (linkSyncing_, true);
            for (int i = 0; i < n; ++i)
                engine_.setFxParams (i, fx[static_cast<size_t> (i)]);
        }
        mb::links::table() = links;

        for (int i = 0; i < n; ++i)
            strips_[static_cast<size_t> (i)]->setChannelIndex (i);

        layoutStrips();
        for (auto& s : strips_) s->refreshSlot();
        engine_.syncDevices();
        updateSummary();
    }

    /** 把右对齐标记数组补齐 / 截断到当前通道数（会话恢复、导入预设后调用）。 */
    void syncAlignmentVector()
    {
        auto& v = mb::ui::rightAlignedState();
        const int n = graph_.channelCount();
        v.resize (static_cast<size_t> (juce::jmax (0, n)), 0);
    }

    int firstRightAlignedIndex() const
    {
        for (int i = 0; i < graph_.channelCount(); ++i)
            if (mb::ui::isChannelRightAligned (i)) return i;
        return graph_.channelCount();
    }

    /** 把 ch 插到「右对齐组」的左边缘（没有右对齐通道时就是末尾）。
        新建通道、复制通道、把某条通道改为 / 取消右对齐都走这里：
        右对齐的通道恒在最右，后来加入的排在该组左边缘。返回 ch 的新索引。 */
    int insertChannelAtBoundary (int ch)
    {
        const int n = graph_.channelCount();
        if (ch < 0 || ch >= n || static_cast<int> (strips_.size()) != n) return ch;

        std::vector<int> order;
        order.reserve (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            if (i != ch) order.push_back (i);

        int pos = static_cast<int> (order.size());
        for (size_t i = 0; i < order.size(); ++i)
            if (mb::ui::isChannelRightAligned (order[static_cast<size_t> (i)]))
            {
                pos = static_cast<int> (i);
                break;
            }
        order.insert (order.begin() + pos, ch);

        bool same = true;
        for (int i = 0; i < n; ++i)
            if (order[static_cast<size_t> (i)] != i) { same = false; break; }
        if (! same) applyChannelOrder (order);
        return pos;
    }

    void applyRightAligned (int ch, bool on)
    {
        if (ch < 0 || ch >= graph_.channelCount()) return;
        if (on)
        {
            mb::ui::setChannelRightAlignedOrigin (ch, ch);   // 记住对齐前位置，取消时恢复
            mb::ui::setChannelRightAligned (ch, true);
            insertChannelAtBoundary (ch);
        }
        else
        {
            mb::ui::setChannelRightAligned (ch, false);
            restoreAlignedPosition (ch);                     // 回到对齐前的位置
        }
        for (auto& s : strips_) s->refreshSlot();
        updateSummary();
        setMessage (on ? mb::tr ("通道已向右对齐 · 新建通道会排在它左侧",
                                 "Channel right-aligned · new channels go to its left")
                       : mb::tr ("已取消向右对齐，通道回到原位",
                                 "Right alignment removed · channel moved back"));
    }

    /** 取消右对齐后把通道放回「对齐前记住的位置」；记忆缺失（-1）时回退到
        右对齐组左边缘。恢复位置不会越过剩余的右对齐通道。 */
    void restoreAlignedPosition (int ch)
    {
        const int n = graph_.channelCount();
        if (ch < 0 || ch >= n || static_cast<int> (strips_.size()) != n) return;
        const int origin = mb::ui::channelRightAlignedOrigin (ch);
        mb::ui::setChannelRightAlignedOrigin (ch, -1);       // 用完即清

        std::vector<int> order;
        order.reserve (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            if (i != ch) order.push_back (i);

        // 目标槽位：记忆位置，但不越过剩余右对齐组
        int boundary = static_cast<int> (order.size());
        for (size_t i = 0; i < order.size(); ++i)
            if (mb::ui::isChannelRightAligned (order[i])) { boundary = static_cast<int> (i); break; }
        const int pos = origin >= 0 ? juce::jlimit (0, boundary, origin) : boundary;
        order.insert (order.begin() + pos, ch);

        bool same = true;
        for (int i = 0; i < n; ++i)
            if (order[static_cast<size_t> (i)] != i) { same = false; break; }
        if (! same) applyChannelOrder (order);
    }

    /** 复制通道：推子 / 静音 / 独奏 / 激活 / 布防 / 名字 / 颜色 / 内置效果参数 /
        插件链（含第三方私有状态）/ 输入输出连接全部复制；C→C 互送也复制，
        成环的会被 tryAddEdge 拒绝。新通道落在右对齐组左边缘。 */
    void duplicateChannel (int src)
    {
        const int n = graph_.channelCount();
        if (src < 0 || src >= n) return;

        int dst = -1;
        for (int i = 0; i < n; ++i)
            if (mb::ui::slotState (i) == mb::ui::kSlotEmpty) { dst = i; break; }
        const bool appended = dst < 0;
        if (appended)
        {
            if (n >= 16)
            {
                setMessage (mb::tr ("已达通道上限 16，无法复制", "Channel limit (16) reached"));
                return;
            }
            dst = graph_.addChannel();
        }

        // 复用空槽时先清空该槽的残留数据
        for (int i = engine_.pluginBank().pluginCount (dst) - 1; i >= 0; --i)
            closePluginEditor (dst, i);
        int guard = 64;
        while (--guard >= 0 && engine_.pluginBank().pluginCount (dst) > 0)
            if (! engine_.pluginBank().removePlugin (dst, 0)) break;

        std::vector<mb::RouteEdge> drop;
        for (const auto& e : graph_.edges())
        {
            const bool hit = e.kind == mb::EdgeKind::InputToChannel  ? e.dst == dst
                           : e.kind == mb::EdgeKind::ChannelToOutput ? e.src == dst
                           : (e.src == dst || e.dst == dst);
            if (hit) drop.push_back (e);
        }
        for (const auto& e : drop)
            graph_.removeEdge (e.src, e.dst, e.kind);

        graph_.setFader   (dst, graph_.fader (src));
        graph_.setMuted   (dst, graph_.isMuted (src));
        graph_.setSolo    (dst, graph_.isSolo (src));
        graph_.setEnabled (dst, graph_.isEnabled (src));
        mb::ui::setChannelArmed (dst, mb::ui::isChannelArmed (src));
        mb::ui::setSlotState (dst, mb::ui::kSlotNormal);
        mb::ui::setChannelRightAligned (dst, mb::ui::isChannelRightAligned (src));

        auto& names = mb::customChannelNames();
        while (names.size() <= dst) names.add (mb::channelName (static_cast<int> (names.size())));
        names.set (dst, mb::channelDisplayName (src) + mb::tr (" 副本", " Copy"));
        auto& cols = mb::ui::customChannelColours();
        if (static_cast<int> (cols.size()) <= dst)
            cols.resize (static_cast<size_t> (dst + 1));
        cols[static_cast<size_t> (dst)] = mb::ui::stripColour (src);

        auto& icons = mb::customChannelIcons();
        while (icons.size() <= dst) icons.add (juce::String());
        icons.set (dst, mb::channelIcon (src));

        engine_.setFxParams (dst, engine_.fxParams (src));
        mb::links::setLinkSet (dst, mb::links::linkSetOf (src));   // 复制链接绑定（全部动作）
        engine_.mediaPlayers().copyConfig (src, dst);        // 复制播放列表与设置

        auto& bank = engine_.pluginBank();
        const int plugins = bank.pluginCount (src);
        for (int i = 0; i < plugins; ++i)
        {
            const int  builtIn = bank.pluginBuiltInId (src, i);
            const bool bypass = bank.pluginBypass (src, i);
            if (builtIn > 0)
            {
                bank.addBuiltIn (dst, builtIn);
                bank.setBypass (dst, bank.pluginCount (dst) - 1, bypass);
                copyPluginMeta (dst, bank.pluginCount (dst) - 1, src, i);
                continue;
            }
            const auto id = bank.pluginIdentifier (src, i);
            auto desc = engine_.pluginCatalog().findByIdentifier (id);
            if (desc.fileOrIdentifier.isEmpty())
                desc = engine_.pluginCatalog().findByName (bank.pluginName (src, i));
            if (desc.fileOrIdentifier.isEmpty()) continue;
            juce::String err;
            auto inst = engine_.pluginCatalog().create (desc, err,
                                                       engine_.sampleRate(),
                                                       engine_.bufferSizeSamples());
            if (inst == nullptr) continue;
            auto state = bank.pluginState (src, i);
            bank.addPlugin (dst, std::move (inst), engine_.sampleRate(),
                            engine_.bufferSizeSamples(),
                            state.getSize() > 0 ? &state : nullptr, id);
            bank.setBypass (dst, bank.pluginCount (dst) - 1, bypass);
            copyPluginMeta (dst, bank.pluginCount (dst) - 1, src, i);
        }

        const std::vector<mb::RouteEdge> snapshot (graph_.edges().begin(), graph_.edges().end());
        for (const auto& e : snapshot)
        {
            std::string why;
            if (e.kind == mb::EdgeKind::InputToChannel && e.dst == src)
                graph_.tryAddEdge ({ e.src, dst, e.kind, e.gain }, why);
            else if (e.kind == mb::EdgeKind::ChannelToOutput && e.src == src)
                graph_.tryAddEdge ({ dst, e.dst, e.kind, e.gain }, why);
            else if (e.kind == mb::EdgeKind::ChannelToChannel && e.dst == src)
                graph_.tryAddEdge ({ e.src, dst, e.kind, e.gain }, why);
            else if (e.kind == mb::EdgeKind::ChannelToChannel && e.src == src)
                graph_.tryAddEdge ({ dst, e.dst, e.kind, e.gain }, why);
        }

        syncChannelNames();
        engine_.prepareBuffers();
        engine_.syncDevices();
        mb::ui::setChannelStereo (dst, mb::ui::isChannelStereo (src));   // 复制通道制式
        graph_.setChannelStereo (dst, mb::ui::isChannelStereo (src));    // 路由内核同步
        if (appended || dst >= static_cast<int> (strips_.size())) rebuildStrips();
        else
        {
            strips_[static_cast<size_t> (dst)]->setChannelIndex (dst);
            strips_[static_cast<size_t> (dst)]->syncFromGraph();
            strips_[static_cast<size_t> (dst)]->refreshSlot();
        }
        layoutStrips();
        insertChannelAtBoundary (dst);
        refreshWindowWidth();
        updateSummary();
        setMessage (mb::tr ("已复制通道：", "Channel duplicated: ")
                    + mb::channelDisplayName (src));
    }

    void updateSummary()
    {
        dirty_ = true;                       // 任何布局/状态变化都触发会话自动保存
        presetDirty_ = true;                 // 相对「上次保存预设」的改动，关闭时提示
        refreshWindowTitle();                // 统计（连接数 / 欠载）已并入原生窗口标题
    }

    void setMessage (const juce::String& m)
    {
        message_ = m;
        messageExpiry_ = juce::Time::getMillisecondCounter() + 2600;
        refreshWindowTitle();                // 临时消息 2.6 s 内顶替标题尾部，到期回落
    }

    void toggleLanguage()
    {
        mb::setLanguageMode (mb::isZh() ? 2 : 1);   // 手动切换后固定为对应语言
        settings_.chinese = mb::isZh();
        settings_.languageMode = mb::languageMode();
        applyLanguage();
        setMessage (mb::tr ("已切换语言", "Language switched"));
    }

    void applyLanguage()
    {
        refreshDrawerTexts();       // 含主题 / 收纳按钮的文字与全部 tooltip
        if (fxPanel_ != nullptr) fxPanel_->applyLanguage();       // 折叠态面板可常驻，需即时换语言
        if (mediaPanel_ != nullptr) mediaPanel_->applyLanguage();
        if (diagPanel_ != nullptr) diagPanel_->applyLanguage();
        for (auto& s : strips_) s->applyLanguage();
        if (setup_ != nullptr) setup_->applyLanguage();
        if (settingsPanel_ != nullptr) settingsPanel_->applyLanguage();
        if (colourPicker_ != nullptr) colourPicker_->applyLanguage();
        updateSummary();
        repaint();
    }

    //==========================================================================
    // 可见槽位映射：隐藏通道与空槽都不占位，拖动时把「可见序号」映射回真实通道索引
    int visibleSlotCount() const
    {
        int n = 0;
        for (int i = 0; i < graph_.channelCount(); ++i)
            if (mb::ui::slotState (i) == mb::ui::kSlotNormal) ++n;
        return juce::jmax (1, n);
    }

    int slotToChannel (int slot) const
    {
        int n = 0;
        for (int i = 0; i < graph_.channelCount(); ++i)
        {
            if (mb::ui::slotState (i) != mb::ui::kSlotNormal) continue;
            if (n == slot) return i;
            ++n;
        }
        return -1;
    }

    // 隐藏 / 恢复通道：只从视图收起，不删除任何数据
    void setChannelHidden (int ch, bool hidden)
    {
        if (ch < 0 || ch >= graph_.channelCount()) return;
        if (mb::ui::slotState (ch) == mb::ui::kSlotEmpty) return;

        if (hidden)
        {
            int visible = 0;
            for (int i = 0; i < graph_.channelCount(); ++i)
                if (mb::ui::slotState (i) == mb::ui::kSlotNormal) ++visible;
            if (visible <= 1)
            {
                setMessage (mb::tr ("至少保留一个可见通道", "Keep at least one visible channel"));
                return;
            }
            if (fxPanel_ != nullptr) closeFxPanel();
        }

        mb::ui::setSlotState (ch, hidden ? mb::ui::kSlotHidden : mb::ui::kSlotNormal);
        layoutStrips();
        refreshWindowWidth();
        for (auto& s : strips_) s->refreshSlot();
        updateSummary();
        setMessage (hidden
            ? mb::tr ("已隐藏通道 · 右键「显示已隐藏通道」可恢复", "Channel hidden · right-click to restore")
            : mb::tr ("已显示通道", "Channel shown"));

        // 插槽链接：手动隐藏 / 显示也同步同码通道并触发同字母互斥
        const auto l = mb::links::linkOf (ch, mb::links::kSlot);
        if (l.bound() && ! linkSyncing_)
            applyLinkState (ch, mb::links::kSlot, ! hidden);
    }

    void showAllChannels()
    {
        for (int i = 0; i < graph_.channelCount(); ++i)
            if (mb::ui::slotState (i) == mb::ui::kSlotHidden)
                mb::ui::setSlotState (i, mb::ui::kSlotNormal);
        layoutStrips();
        refreshWindowWidth();
        for (auto& s : strips_) s->refreshSlot();
        updateSummary();
        setMessage (mb::tr ("已显示全部通道", "All channels shown"));
    }

    //==========================================================================
    // 自定义 RGB 浮层
    void showColourPicker (int ch)
    {
        if (colourPicker_ != nullptr || ch < 0
            || ch >= static_cast<int> (strips_.size())) return;

        auto p = std::make_unique<ColourPickerPanel> (mb::ui::stripColour (ch),
                                                      mb::ui::channelColour (ch),
                                                      mb::channelIcon (ch),
                                                      mb::channelDisplayName (ch));
        juce::Component::SafePointer<MainComponent> safe (this);
        auto deferClose = [safe]
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr) safe->closeColourPicker();
            });
        };
        p->onApply = [this, ch, deferClose] (juce::Colour c, const juce::String& icon,
                                             const juce::String& name)
        {
            if (ch < static_cast<int> (strips_.size()))
            {
                strips_[static_cast<size_t> (ch)]->applyCustomColour (c);
                strips_[static_cast<size_t> (ch)]->applyCustomIcon (icon);
            }
            // 名称随「确定」一并回写：留空回退基础名（与双击重命名同一张名字表）
            auto& names = mb::customChannelNames();
            while (names.size() <= ch) names.add (juce::String());
            names.set (ch, name.isNotEmpty() ? name : mb::channelName (ch));
            if (ch < static_cast<int> (strips_.size()))
                strips_[static_cast<size_t> (ch)]->invalidate();
            deferClose();          // 延后销毁：此刻正处于按钮 onClick 回调栈中
        };
        p->onCancel = deferClose;
        colourPicker_ = std::move (p);
        addAndMakeVisible (colourPicker_.get());
        colourPicker_->setBounds (getLocalBounds());
        colourPicker_->toFront (false);
    }

    void closeColourPicker()
    {
        if (colourPicker_ == nullptr) return;
        colourPicker_->dismiss();
        removeChildComponent (colourPicker_.get());
        colourPicker_.reset();
        raisePluginPanels();
    }

    //==========================================================================
    // 新建通道：优先复用空槽，其次追加；再选单声道 / 立体声与输入设备
    void addChannel()
    {
        int slot = -1;
        for (int i = 0; i < graph_.channelCount(); ++i)
            if (mb::ui::slotState (i) == mb::ui::kSlotEmpty) { slot = i; break; }

        if (slot < 0 && graph_.channelCount() >= 16)
        {
            setMessage (mb::tr ("已达通道上限 16", "Channel limit reached (16)"));
            return;
        }
        pendingSlot_ = slot;
        pendingCount_ = 1;                  // 每次新建默认 1 条，可在下一步改
        showNewChannelFormatMenu();
    }

    void activateSlot (int ch)
    {
        if (ch < 0 || ch >= graph_.channelCount()
            || mb::ui::slotState (ch) != mb::ui::kSlotEmpty) return;
        pendingSlot_ = ch;
        pendingCount_ = 1;
        showNewChannelFormatMenu();
    }

    void showNewChannelFormatMenu()
    {
        juce::PopupMenu top;
        top.addSectionHeader (mb::tr ("新建通道 · 选择制式", "New channel · format"));
        top.addItem (1, mb::tr ("单声道", "Mono"));
        top.addItem (2, mb::tr ("立体声", "Stereo"));
        juce::Component::SafePointer<MainComponent> safe (this);
        top.showMenuAsync (juce::PopupMenu::Options(), [safe] (int r)
        {
            if (safe == nullptr) return;
            if (r == 1)      safe->showNewChannelCountMenu (false);
            else if (r == 2) safe->showNewChannelCountMenu (true);
            else             safe->pendingSlot_ = -1;
        });
    }

    /** 制式选完 → 选数量（1–10，默认 1），选完再进输入设备选择。 */
    void showNewChannelCountMenu (bool stereo)
    {
        juce::PopupMenu m;
        m.addSectionHeader (mb::tr ("选择通道数量", "Number of channels"));
        for (int n = 1; n <= 10; ++n)
            m.addItem (n, juce::String (n), true, n == pendingCount_);
        juce::Component::SafePointer<MainComponent> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe, stereo] (int r)
        {
            if (safe == nullptr) return;
            if (r >= 1 && r <= 10)
            {
                safe->pendingCount_ = r;
                safe->showNewChannelInputMenu (stereo);
                return;
            }
            safe->pendingSlot_ = -1;
        });
    }

    void showNewChannelInputMenu (bool stereo)
    {
        const auto& ports = engine_.catalog().inputPorts();
        juce::PopupMenu m;
        // 多条时提示用户接下来会一次创建 N 条（每条都接同一输入设备）
        m.addSectionHeader (pendingCount_ > 1
            ? mb::tr ("选择输入设备 · 将添加 ", "Choose input device · adding ")
              + juce::String (pendingCount_) + mb::tr (" 条", " channels")
            : mb::tr ("选择输入设备", "Choose input device"));
        int added = 0;
        for (int p = 0; p < static_cast<int> (ports.size()); ++p)
        {
            if (! engine_.catalog().portVisible (true, p)) continue;   // 未勾选的辅助设备不列出
            if (stereo)
            {
                if (p % 2 != 0 || p + 1 >= static_cast<int> (ports.size())
                    || ! engine_.catalog().portVisible (true, p + 1)) continue;
                m.addItem (8000 + p, ports[static_cast<size_t> (p)].name
                                   + mb::S(" + ") + ports[static_cast<size_t> (p + 1)].name);
            }
            else
            {
                m.addItem (1000 + p, ports[static_cast<size_t> (p)].name);
            }
            ++added;
        }
        if (added == 0)
            m.addItem (-1, mb::tr ("（无可用输入设备）", "(no input devices)"), false);
        m.addSeparator();
        m.addItem (1, mb::tr ("暂不连接输入", "Leave input unconnected"));
        juce::Component::SafePointer<MainComponent> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe, stereo] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1) { safe->finishAddChannel (stereo, -1); return; }
            if (result < 1000) { safe->pendingSlot_ = -1; return; }
            // 立体声条目 id = 8000 + p，单声道 = 1000 + p；按段还原端口基址
            safe->finishAddChannel (stereo,
                                    result >= 8000 ? result - 8000 : result - 1000);
        });
    }

    // 让通道名成为「数据」而非位置回退：把 customChannelNames 补齐到通道数
    void syncChannelNames()
    {
        auto& names = mb::customChannelNames();
        while (names.size() < graph_.channelCount())
            names.add (mb::channelName (static_cast<int> (names.size())));
    }

    /** 创建 / 复用一条通道并接线（finishAddChannel 的单条实现）。
        slot >= 0 时复用该空槽，否则在末尾追加；返回右对齐边界
        插入后的最终通道索引。 */
    int createOneChannel (int slot, bool stereo, int port)
    {
        int ch;
        if (slot >= 0 && slot < graph_.channelCount())
        {
            ch = slot;                          // 复用空槽：不新增通道，其它通道不动
            mb::ui::setSlotState (ch, mb::ui::kSlotNormal);
            graph_.setEnabled (ch, true);
            graph_.setFader (ch, 1.0f);
            graph_.setMuted (ch, false);
            graph_.setSolo (ch, false);
            mb::ui::setChannelArmed (ch, false);
            syncChannelNames();
        }
        else
        {
            graph_.addChannel();
            syncChannelNames();
            ch = graph_.channelCount() - 1;
            mb::ui::setSlotState (ch, mb::ui::kSlotNormal);
        }
        mb::ui::setChannelStereo (ch, stereo);   // 制式跟随通道实体（电平表单/双条）
        graph_.setChannelStereo (ch, stereo);    // 路由内核同步（compile 展开 L/R 缓冲边）

        std::string why;
        if (port >= 0)
        {
            graph_.tryAddEdge ({ port, ch, mb::EdgeKind::InputToChannel, 1.0f }, why);
            if (stereo && port + 1 < graph_.inputPortCount())
                graph_.tryAddEdge ({ port + 1, ch, mb::EdgeKind::InputToChannel, 1.0f }, why);
        }
        engine_.prepareBuffers();
        engine_.syncDevices();

        if (ch >= static_cast<int> (strips_.size())) rebuildStrips();
        else
        {
            strips_[static_cast<size_t> (ch)]->setChannelIndex (ch);
            strips_[static_cast<size_t> (ch)]->syncFromGraph();
            strips_[static_cast<size_t> (ch)]->refreshSlot();
        }
        layoutStrips();

        // 右对齐分组：新建 / 复用空槽得到的通道必须排在右对齐组左侧。
        return insertChannelAtBoundary (ch);
    }

    void finishAddChannel (bool stereo, int port)
    {
        int slot = pendingSlot_;
        pendingSlot_ = -1;
        const int count = juce::jlimit (1, 10, pendingCount_);
        pendingCount_ = 1;

        // 一次创建 N 条：第一条可复用空槽，其余追加；16 上限内逐条创建，
        // 超出上限时停下并提示（已创建的保留）。
        int first = -1, added = 0;
        for (int i = 0; i < count; ++i)
        {
            if (slot < 0 && graph_.channelCount() >= 16)
            {
                setMessage (mb::tr ("已达通道上限 16", "Channel limit reached (16)"));
                break;
            }
            const int ch = createOneChannel (slot, stereo, port);
            slot = -1;                       // 只有第一条可复用空槽
            if (first < 0) first = ch;
            ++added;
        }
        if (added <= 0) return;

        // 命名框只弹第一条（后续通道保持 CH 编号预填，可双击改名）。
        // 不自动命名：创建后立即弹出空白命名框（回车确认 / Esc 跳过）
        if (first >= 0 && first < static_cast<int> (strips_.size()))
        {
            juce::Component::SafePointer<ChannelStrip> sp (strips_[static_cast<size_t> (first)].get());
            juce::MessageManager::callAsync ([sp]
            {
                if (sp != nullptr) sp->promptRename();
            });
        }
        if (added == 1)
            setMessage (stereo ? mb::tr ("已添加立体声通道", "Stereo channel added")
                               : mb::tr ("已添加单声道通道", "Mono channel added"));
        else
            setMessage (mb::tr ("已添加 ", "") + juce::String (added)
                        + mb::tr (" 条通道", " channels added"));
        refreshWindowWidth();
    }

    // 右键「删除通道」：真删除 —— 图连接、插件链、内置效果参数与全部随通道
    // 状态（名字/颜色/布防/右对齐/制式/槽位/链接）整体左移补位，通道总数收缩。
    // 不再产生「空槽」：旧版为保持通道数固定而把删除标记为空槽沉到末尾，
    // 节点视图会留下占位空通道，不符合直觉。允许删到 0 条。
    void deleteChannel (int ch)
    {
        const int n = graph_.channelCount();
        if (ch < 0 || ch >= n) return;

        // 被删通道的插件编辑器窗口持有实例指针，实例即将退役，必须先关
        for (int idx = engine_.pluginBank().pluginCount (ch) - 1; idx >= 0; --idx)
            closePluginEditor (ch, idx);

        graph_.removeChannel (ch);            // 边 + 推子/静音/独奏/激活一并前移
        engine_.pluginBank().removeChannel (ch);
        engine_.mediaPlayers().removeChannel (ch);

        // 内置效果参数前移一位（参数仓无删除接口，手动搬移；尾部自然空出）
        for (int i = ch; i < n - 1; ++i)
            engine_.setFxParams (i, engine_.fxParams (i + 1));

        auto& names = mb::customChannelNames();
        if (ch < names.size()) names.removeRange (ch, 1);
        auto& cols = mb::ui::customChannelColours();
        if (ch < static_cast<int> (cols.size())) cols.erase (cols.begin() + ch);
        auto eraseAt = [ch] (std::vector<std::uint8_t>& v)
        {
            if (ch < static_cast<int> (v.size())) v.erase (v.begin() + ch);
        };
        eraseAt (mb::ui::channelArmState());
        eraseAt (mb::ui::rightAlignedState());
        eraseAt (mb::ui::channelStereoState());
        eraseAt (mb::ui::channelSlots());
        // 「对齐前位置」记忆同步删除（vector<int>，与 eraseAt 的 uint8 版本不同型）
        {
            auto& origins = mb::ui::rightAlignedOrigin();
            if (ch < static_cast<int> (origins.size())) origins.erase (origins.begin() + ch);
        }
        auto& links = mb::links::table();
        if (ch < static_cast<int> (links.size())) links.erase (links.begin() + ch);

        // 指向通道的浮层索引已失效，一律关闭
        if (fxPanel_ != nullptr) closeFxPanel();
        if (linkOverlay_ != nullptr) closeLinkOverlay();
        if (colourPicker_ != nullptr) closeColourPicker();

        rebuildStrips();                      // 通道数变化，整体重建
        engine_.prepareBuffers();
        engine_.syncDevices();

        setMessage (mb::tr ("已删除通道", "Channel deleted"));
        refreshWindowWidth();
    }

    //==========================================================================
    // 首次开箱欢迎页：选语言（跟随系统 / 中文 / English）→ 自动进入设备设置
    //==========================================================================
    void showWelcome()
    {
        if (welcome_ != nullptr) return;
        welcome_ = std::make_unique<WelcomeOverlay>();
        welcome_->onLanguagePicked = [this] (int mode)
        {
            mb::setLanguageMode (mode);
            settings_.languageMode = mode;
            settings_.chinese = mb::isZh();
            mb::AppSettings::save (settings_);
            closeWelcome();
            applyLanguage();                  // 全部文案按新语言重建
            showSetup (true, engine_.error());
        };
        addAndMakeVisible (welcome_.get());
        welcome_->setBounds (getLocalBounds());
        welcome_->toFront (false);
    }

    void closeWelcome()
    {
        if (welcome_ == nullptr) return;
        removeChildComponent (welcome_.get());
        welcome_.reset();
        raisePluginPanels();
    }

    //==========================================================================
    // 主题切换（设置页「浅色模式」）：全局色即时翻转 + 常驻控件重刷
    //==========================================================================
    void setLightTheme (bool light)
    {
        mb::ui::applyTheme (! light);
        mb::sharedLookAndFeel().refreshThemeColours();
        // 自绘标题栏直接读全局主题色，重绘即换色（不再依赖 DWM 属性）
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->repaint();
        refreshChromeColours();               // 抽屉按钮 / 收纳切换 / 节点视图按钮
        rebuildStrips();                      // 通道条整建，控件色随新主题
        if (fxPanel_ != nullptr) fxPanel_->applyTheme();          // 内置效果器推子卡
        if (mediaPanel_ != nullptr) mediaPanel_->applyTheme();    // 音频播放插件弹窗
        if (settingsPanel_ != nullptr) settingsPanel_->refreshThemeColours();
        if (setup_ != nullptr) setup_->applyLanguage();   // 设备页顺带重刷文案
        updateSummary();
        updateThemeButton();                  // 抽屉主题入口与设置页保持同步
        repaint();
        setMessage (light ? mb::tr ("已切换到浅色模式", "Light theme enabled")
                          : mb::tr ("已切换到深色模式", "Dark theme enabled"));
    }

    /** 抽屉主题按钮：只显示目标模式名（深色模式 / 浅色模式）。 */
    juce::String themeBtnText() const
    {
        return settings_.lightTheme
                 ? mb::tr ("深色模式", "Dark mode")
                 : mb::tr ("浅色模式", "Light mode");
    }

    void toggleLightTheme()
    {
        settings_.lightTheme = ! settings_.lightTheme;   // 必须写回，否则下次翻转取的还是旧值
        mb::AppSettings::save (settings_);
        setLightTheme (settings_.lightTheme);
        if (settingsPanel_ != nullptr) settingsPanel_->syncLightTheme (settings_.lightTheme);
    }

    void updateThemeButton()
    {
        themeBtn_.icon = settings_.lightTheme ? DrawerButton::ThemeMoon
                                              : DrawerButton::ThemeSun;
        refreshDrawerTexts();
    }

    /** 录音显隐按钮文案：跟随当前录音区显示状态（动作导向，与收纳抽屉一致）。 */
    juce::String recBtnText() const
    {
        return recorderVisible_
                 ? mb::tr ("隐藏录音", "Hide recording")
                 : mb::tr ("显示录音", "Show recording");
    }

    void updateRecButton()
    {
        recBtn_.icon = recorderVisible_ ? DrawerButton::RecHide : DrawerButton::RecShow;
        refreshDrawerTexts();
    }

    /** 切换顶部录音区显隐：隐藏时调音台通道顶满主区，状态持久化、重启恢复。 */
    void toggleRecorderVisible()
    {
        recorderVisible_ = ! recorderVisible_;
        settings_.recorderVisible = recorderVisible_;
        mb::AppSettings::save (settings_);
        updateRecButton();
        resized();   // 重排：录音条显隐 → 通道区顶满 / 收回
    }

    /** 主题切换后重刷常驻按钮的构造期颜色（通道条由 rebuildStrips 重建）。 */
    void refreshChromeColours()
    {
        for (auto* b : { &addBtn_, &newBtn_, &savePresetBtn_, &importBtn_, &exportBtn_,
                         &clearBtn_, &settingsBtn_, &toggleBtn_, &themeBtn_, &recBtn_,
                         &viewBtn_, &foldBtn_ })
        {
            b->setColour (juce::TextButton::buttonColourId,  mb::ui::card);
            b->setColour (juce::TextButton::textColourOffId, mb::ui::text);
        }
    }

    //==========================================================================
    // 通道链接联动引擎
    // 同码 = 同步组（一起开/关/联动）；同字母不同码 = 互斥组（激活即关其他，
    // 允许全关）。手动点击与全局快捷键都走 applyLinkState，行为一致。
    //==========================================================================
    /** 通道某动作的当前组状态（on 语义：开关=激活 独奏=亮 效果=任一启用（含 VST3） 显示=可见）。 */
    bool linkChannelState (int ch, int action)
    {
        switch (action)
        {
            case mb::links::kPower: return graph_.isEnabled (ch);
            case mb::links::kSolo:  return graph_.isSolo (ch);
            case mb::links::kFx:
            {
                // 「效果」= 整条插件链：内置效果链任一模块启用，
                // 或任一在链 VST3 插件未旁通，即视为启用。
                const auto& p = engine_.fxParams (ch);
                if (p.hpfOn || p.gateOn || p.compOn || p.reverbOn || p.limitOn) return true;
                auto& bank = engine_.pluginBank();
                for (int i = 0; i < bank.pluginCount (ch); ++i)
                    if (! bank.pluginOffline (ch, i) && ! bank.pluginBypass (ch, i))
                        return true;
                return false;
            }
            case mb::links::kSlot:  return mb::ui::slotState (ch) == mb::ui::kSlotNormal;
            default: break;
        }
        return false;
    }

    /** 把动作状态写入单条通道。返回是否真的发生了变化。 */
    bool setLinkChannelState (int ch, int action, bool on)
    {
        if (ch < 0 || ch >= graph_.channelCount()) return false;
        switch (action)
        {
            case mb::links::kPower:
                if (graph_.isEnabled (ch) == on) return false;
                graph_.setEnabled (ch, on);
                return true;
            case mb::links::kSolo:
                if (graph_.isSolo (ch) == on) return false;
                graph_.setSolo (ch, on);
                return true;
            case mb::links::kFx:
            {
                auto p = engine_.fxParams (ch);
                const bool cur = p.hpfOn || p.gateOn || p.compOn || p.reverbOn || p.limitOn;
                auto& bank = engine_.pluginBank();
                bool vstOn = false;
                for (int i = 0; i < bank.pluginCount (ch); ++i)
                    if (! bank.pluginOffline (ch, i) && ! bank.pluginBypass (ch, i))
                    { vstOn = true; break; }
                if (cur == on && vstOn == on) return false;

                // 内置效果链：模块开关全开 / 全关（自带插件条目的旁通标志
                // 由 setBypass 同步，音频实际由 FxParams 驱动）；
                // VST3：全部旁通 / 全部启用。离线条目跳过。
                p.hpfOn = p.gateOn = p.compOn = p.reverbOn = p.limitOn = on;
                engine_.setFxParams (ch, p);
                for (int i = 0; i < bank.pluginCount (ch); ++i)
                    if (! bank.pluginOffline (ch, i))
                        bank.setBypass (ch, i, ! on);
                return true;
            }
            case mb::links::kSlot:
            {
                const auto cur = mb::ui::slotState (ch);
                const auto want = on ? mb::ui::kSlotNormal : mb::ui::kSlotHidden;
                if (cur == want || cur == mb::ui::kSlotEmpty) return false;
                if (! on)   // 至少保留一个可见通道
                {
                    int visible = 0;
                    for (int i = 0; i < graph_.channelCount(); ++i)
                        if (mb::ui::slotState (i) == mb::ui::kSlotNormal) ++visible;
                    if (visible <= 1) return false;
                }
                mb::ui::setSlotState (ch, want);
                return true;
            }
            default: break;
        }
        return false;
    }

    /** 联动核心：src 通道的 action 变为 on → 同码全组跟随 + 同字母互斥。 */
    void applyLinkState (int srcCh, int action, bool on)
    {
        if (linkSyncing_) return;
        const auto src = mb::links::linkOf (srcCh, action);

        // 未绑定链接码的通道：直接写本通道状态（按钮点击的普通路径）
        if (! src.bound())
        {
            if (setLinkChannelState (srcCh, action, on))
                refreshAfterLinkSync();
            return;
        }

        juce::ScopedValueSetter<bool> guard (linkSyncing_, true);

        const int n = graph_.channelCount();
        bool changed = false;

        // 1) 同码同步组 → 全组置为 on
        for (int c = 0; c < n; ++c)
        {
            const auto l = mb::links::linkOf (c, action);
            if (! l.bound()) continue;
            if (l.letter == src.letter && l.index == src.index)
                changed |= setLinkChannelState (c, action, on);
        }

        // 2) 激活时同字母其他码全部关闭（互斥；音量只联动不互斥）
        if (on && action != mb::links::kVolume)
        {
            for (int c = 0; c < n; ++c)
            {
                const auto l = mb::links::linkOf (c, action);
                if (! l.bound()) continue;
                if (l.letter == src.letter && l.index != src.index)
                    changed |= setLinkChannelState (c, action, false);
            }
        }

        if (changed) refreshAfterLinkSync();
    }

    /** 联动写回后统一刷新通道条 / 节点视图，并标记会话待存。 */
    void refreshAfterLinkSync()
    {
        for (auto& s : strips_) { s->syncFromGraph(); s->refreshSlot(); }
        layoutStrips();                      // 插槽联动会改变可见槽位布局
        refreshWindowWidth();
        nodeCanvas_.repaint();
        engine_.syncDevices();
        updateSummary();
        dirty_ = true;                       // 2 秒后会话自动保存
    }

    /** 音量链接：拖动推子时，同码通道的推子等值跟随（无互斥语义）。 */
    void applyLinkVolume (int srcCh, float gain)
    {
        const auto src = mb::links::linkOf (srcCh, mb::links::kVolume);
        if (! src.bound() || linkSyncing_) return;
        juce::ScopedValueSetter<bool> guard (linkSyncing_, true);

        bool changed = false;
        for (int c = 0; c < graph_.channelCount(); ++c)
        {
            if (c == srcCh) continue;
            const auto l = mb::links::linkOf (c, mb::links::kVolume);
            if (! l.bound()) continue;
            if (l.letter != src.letter || l.index != src.index) continue;
            if (std::abs (graph_.fader (c) - gain) < 1.0e-4f) continue;
            graph_.setFader (c, gain);
            if (c < static_cast<int> (strips_.size()))
                strips_[static_cast<size_t> (c)]->syncFaderFromGraph();
            changed = true;
        }
        if (changed) { dirty_ = true; nodeCanvas_.repaint(); }
    }

    /** 全局快捷键触发：所有录入该快捷键的通道按动作整组翻转
        （任一开→全关，否则全开）。返回是否命中（命中即消费按键）。 */
    bool triggerLinkHotkey (const juce::KeyPress& key)
    {
        if (strips_.empty()) return false;
        const int n = graph_.channelCount();
        bool consumed = false;

        for (int action = 0; action < mb::links::kActionCount; ++action)
        {
            if (action == mb::links::kVolume) continue;   // 音量无翻转语义（仅推子联动）

            std::vector<int> members;                     // 录入该快捷键且动作匹配的通道
            bool anyOn = false;
            for (int c = 0; c < n; ++c)
            {
                const auto l = mb::links::linkOf (c, action);
                if (! l.hasHotkey()) continue;
                if (! mb::links::matchesHotkey (l, key)) continue;
                members.push_back (c);
                if (linkChannelState (c, action)) anyOn = true;
            }
            if (members.empty()) continue;
            consumed = true;

            // 同码去重：同一链接码只触发一次整组翻转
            for (size_t i = 0; i < members.size(); ++i)
            {
                const auto code = mb::links::linkOf (members[i], action);
                bool first = true;
                for (size_t j = 0; j < i && first; ++j)
                    first = mb::links::linkOf (members[j], action) != code;
                if (first) applyLinkState (members[i], action, ! anyOn);
            }
        }
        return consumed;
    }

    /** 效果参数被任何 UI 改动（效果面板 / 插件旁通等）后的联动入口。 */
    void handleFxLinkChanged (int ch)
    {
        if (linkSyncing_) return;
        if (static_cast<int> (strips_.size()) != graph_.channelCount()) return;  // 装载中
        const auto l = mb::links::linkOf (ch, mb::links::kFx);
        if (! l.bound()) return;
        applyLinkState (ch, mb::links::kFx, linkChannelState (ch, mb::links::kFx));
    }

    /** 打开「快捷键与链接」弹窗。defaultAction：通道尚未绑定任何链接/快捷键时
        预选的动作（如插件菜单入口传 kFx）；-1 = 用系统默认「开关」。
        每个动作各有一条独立配置（链接码与快捷键随动作分开保存），弹窗内
        点动作行切换编辑对象。
        弹窗内列出 同码同步组 / 同字母互斥组 通道；同码组的快捷键全组共享。 */
    void showChannelLinkDialog (int ch, int defaultAction = -1)
    {
        closeLinkOverlay();
        auto cur = mb::links::linkSetOf (ch);
        int initialAction = 0;
        if (defaultAction >= 0 && ! mb::links::anyBound (ch))
            initialAction = defaultAction;                 // 全未配置：按入口意图预选动作
        else
            for (int a = 0; a < mb::links::kActionCount; ++a)
                if (cur[a].bound() || cur[a].hasHotkey()) { initialAction = a; break; }

        // 同码/互斥分组名单与快捷键继承全部在弹窗内实时计算
        // （linkFor 每次读取各通道当前链接，弹窗内换码立即刷新，不用打开时快照）
        linkOverlay_ = std::make_unique<ChannelLinkOverlay> (ch, cur, initialAction);
        linkOverlay_->channelCount = [this] { return graph_.channelCount(); };
        linkOverlay_->linkFor      = [] (int c, int a) { return mb::links::linkOf (c, a); };
        // 录入通道快捷键期间放行按键，否则系统级钩子会吞掉 / 误触发正在录的组合键
        linkOverlay_->onHotkeyCapture = [this] (bool capturing)
        {
            hotkeyEngine_.setSuppressed (capturing);
        };
        linkOverlay_->onApply = [this] (int ch2, const mb::links::LinkSet& set)
        {
            for (int a = 0; a < mb::links::kActionCount; ++a)
                mb::links::setLink (ch2, a, set[a]);

            // 同码组快捷键同步（按动作）：修改/清除快捷键时，该动作下同码
            // （字母+序号相同）的通道一并写入，保证整组共用同一快捷键。
            int synced = 0;
            for (int a = 0; a < mb::links::kActionCount; ++a)
            {
                const auto& s = set[a];
                if (! s.bound()) continue;
                for (int c = 0; c < graph_.channelCount(); ++c)
                {
                    if (c == ch2) continue;
                    const auto l = mb::links::linkOf (c, a);
                    if (! l.bound() || l.letter != s.letter || l.index != s.index) continue;
                    if (l.key == s.key && l.mods == s.mods) continue;
                    auto g = l; g.key = s.key; g.mods = s.mods;
                    mb::links::setLink (c, a, g);
                    if (c < static_cast<int> (strips_.size()))
                        strips_[static_cast<size_t> (c)]->invalidate();
                    ++synced;
                }
            }

            if (ch2 < static_cast<int> (strips_.size()))
                strips_[static_cast<size_t> (ch2)]->invalidate();
            nodeCanvas_.repaint();
            dirty_ = true;
            if (set.empty())
            {
                setMessage (mb::tr ("已清空快捷键与链接", "Hotkey & link cleared"));
            }
            else
            {
                // 汇总各动作配置：动作名 + 快捷键 + 链接码
                juce::String cfg;
                for (int a = 0; a < mb::links::kActionCount; ++a)
                {
                    const auto& s = set[a];
                    if (! s.bound() && ! s.hasHotkey()) continue;
                    cfg += (cfg.isNotEmpty() ? mb::S ("  ·  ") : juce::String())
                         + mb::links::actionName (a) + mb::S (" ")
                         + (mb::links::hotkeyText (s).isNotEmpty()
                                ? mb::links::hotkeyText (s) + mb::S (" ") : juce::String())
                         + mb::links::codeString (s);
                }
                juce::String msg = mb::tr ("快捷键与链接已设置：", "Hotkey & link set: ") + cfg
                                 + (synced > 0
                                        ? mb::tr ("（已同步到 ", " (synced to ")
                                              + juce::String (synced) + mb::tr (" 个同码通道)",
                                                                                " same-code channels)")
                                        : juce::String());

                // 应用后快捷键冲突复查（跨动作汇总、通道名去重）：
                // 非同码通道共用按键时在状态栏警示
                juce::StringArray mutexOthers, others;
                for (int a = 0; a < mb::links::kActionCount; ++a)
                {
                    const auto& s = set[a];
                    if (! s.hasHotkey()) continue;
                    for (int c = 0; c < graph_.channelCount(); ++c)
                    {
                        if (c == ch2) continue;
                        const auto l = mb::links::linkOf (c, a);
                        if (! l.hasHotkey() || l.key != s.key || l.mods != s.mods) continue;
                        if (l.bound() && l.letter == s.letter && l.index == s.index) continue;
                        const auto n = mb::S ("CH") + juce::String (c + 1)
                                       + mb::S (" ") + mb::channelDisplayName (c);
                        auto& bucket = (l.bound() && l.letter == s.letter) ? mutexOthers : others;
                        if (! bucket.contains (n)) bucket.add (n);
                    }
                }
                if (! mutexOthers.isEmpty() || ! others.isEmpty())
                {
                    juce::String warn = mb::tr (" ⚠ 与 ", " ⚠ shares key with ");
                    if (! mutexOthers.isEmpty())
                        warn += mb::tr ("互斥组通道 ", "mutex channels ")
                                + mutexOthers.joinIntoString (mb::S ("、"));
                    if (! others.isEmpty())
                    {
                        if (! mutexOthers.isEmpty()) warn += mb::S ("；");
                        warn += others.joinIntoString (mb::S ("、"));
                    }
                    warn += mb::tr (" 快捷键相同，按下会同时触发",
                                    " — pressing triggers them together");
                    msg += warn;
                }
                setMessage (msg);
            }
            closeLinkOverlay();
        };
        linkOverlay_->onCancel = [this] { closeLinkOverlay(); };
        addAndMakeVisible (linkOverlay_.get());
        linkOverlay_->setBounds (getLocalBounds());
        linkOverlay_->toFront (true);
    }

    void closeLinkOverlay()
    {
        if (linkOverlay_ == nullptr) return;
        hotkeyEngine_.setSuppressed (false);   // 录入中途关闭也要恢复全局热键
        removeChildComponent (linkOverlay_.get());
        linkOverlay_.reset();
        raisePluginPanels();
    }

    /** 复制通道时随插件复制备注（旁通状态由调用方另行写入）。 */
    void copyPluginMeta (int dstCh, int dstIdx, int srcCh, int srcIdx)
    {
        auto& bank = engine_.pluginBank();
        bank.setPluginNote (dstCh, dstIdx, bank.pluginNote (srcCh, srcIdx));
    }

    //==========================================================================
    // 「输入源 / 输出目标」选择浮层：卡片式平铺列表，替代旧的多级菜单。
    //==========================================================================
    /** 同设备互补声道端口（channelIndex 0/1 配对且可见），没有返回 -1。 */
    int siblingPort (bool input, int port) const
    {
        const auto& cat = engine_.catalog();
        const auto& ports = input ? cat.inputPorts() : cat.outputPorts();
        if (port < 0 || port >= static_cast<int> (ports.size())) return -1;
        const auto& p = ports[static_cast<size_t> (port)];
        for (int i = 0; i < static_cast<int> (ports.size()); ++i)
        {
            if (i == port) continue;
            const auto& q = ports[static_cast<size_t> (i)];
            if (q.deviceIndex == p.deviceIndex
                && ((p.channelIndex == 0 && q.channelIndex == 1)
                 || (p.channelIndex == 1 && q.channelIndex == 0))
                && cat.portVisible (input, i))
                return i;
        }
        return -1;
    }

    /** 指定端口与通道之间是否已有连接。 */
    bool hasPortEdge (mb::EdgeKind kind, int ch, int port, bool input) const
    {
        for (const auto& e : graph_.edges())
            if (e.kind == kind
                && (input ? (e.src == port && e.dst == ch)
                          : (e.src == ch && e.dst == port)))
                return true;
        return false;
    }

    void showSourcePicker (int ch, bool input)
    {
        closeSourcePicker();
        sourcePicker_ = std::make_unique<SourcePickerOverlay> (graph_, engine_.catalog (),
                                                               ch, input);
        sourcePicker_->onTogglePort = [this] (int ch2, int port, bool /*stereo*/, bool in)
        {
            const auto kind = in ? mb::EdgeKind::InputToChannel : mb::EdgeKind::ChannelToOutput;
            // 已连接 → 断开该端口
            for (const auto& e : graph_.edges())
                if (e.kind == kind && e.src == (in ? port : ch2) && e.dst == (in ? ch2 : port))
                {
                    graph_.removeEdge (e.src, e.dst, e.kind);
                    afterSourceEdit (mb::tr ("已移除连接", "Connection removed"));
                    return;
                }
            // 接入：继承本通道已有同类设备连接的增益（等效旧的「切换设备」）
            float gain = 1.0f;
            for (const auto& e : graph_.edges())
                if (e.kind == kind && (in ? e.dst == ch2 : e.src == ch2)) { gain = e.gain; break; }
            std::string why;
            const bool ok = in ? graph_.tryAddEdge ({ port, ch2, kind, gain }, why)
                               : graph_.tryAddEdge ({ ch2, port, kind, gain }, why);
            // 立体声设备：点选任一声道自动补选同设备的另一声道（已有连接则不重复）；
            // 单声道设备无互补声道，引擎自动把该路复制到通道的 L/R（居中）
            if (ok)
            {
                const int sib = siblingPort (in, port);
                if (sib >= 0 && ! hasPortEdge (kind, ch2, sib, in))
                {
                    if (in) graph_.tryAddEdge ({ sib, ch2, kind, gain }, why);
                    else    graph_.tryAddEdge ({ ch2, sib, kind, gain }, why);
                }
            }
            afterSourceEdit (ok ? mb::tr ("已建立连接", "Connection added")
                                : mb::tr ("连接被拒绝：", "Rejected: ") + juce::String (why));
        };
        sourcePicker_->onToggleChannel = [this] (int ch2, int other, int side, bool in)
        {
            const int src = in ? other : ch2, dst = in ? ch2 : other;
            const auto kind = mb::EdgeKind::ChannelToChannel;
            // 该声道已连接 → 断开（双声道边降级保留另一声道，可单独取消任一条）
            const int st = graph_.channelEdgeSide (src, dst);
            if (st == -1 || st == side)
            {
                graph_.removeChannelEdgeSide (src, dst, side);
                afterSourceEdit (mb::tr ("已移除连接", "Connection removed"));
                return;
            }
            // 未连接 → 接入；点选任一声道后自动补选另一声道
            // （另一声道已有连接时 tryAddEdge 会直接合并为双声道，不会重复添加）
            std::string why;
            if (graph_.tryAddEdge ({ src, dst, kind, 1.0f, side }, why))
            {
                if (graph_.channelEdgeSide (src, dst) == side)   // 未发生合并 → 补互补声道
                    graph_.tryAddEdge ({ src, dst, kind, 1.0f, side == 0 ? 1 : 0 }, why);
                afterSourceEdit (mb::tr ("已建立连接", "Connection added"));
            }
            else
                setMessage (mb::tr ("连接被拒绝：", "Rejected: ") + juce::String (why));
        };
        sourcePicker_->onClose = [this] { closeSourcePicker(); };

        addAndMakeVisible (sourcePicker_.get());
        sourcePicker_->setBounds (getLocalBounds());
        sourcePicker_->toFront (true);
        sourcePicker_->grabKeyboardFocus();
    }

    void closeSourcePicker()
    {
        if (sourcePicker_ == nullptr) return;
        removeChildComponent (sourcePicker_.get());
        sourcePicker_.reset();
        raisePluginPanels();
    }

    /** 节点视图「移除插件」：与 togglePlugin 的移除路径同款收尾
        （自带插件联动参数/媒体播放器清理 + 关编辑器 + 刷新通道条）。 */
    void removePluginAt (int ch, int idx)
    {
        auto& bank = engine_.pluginBank();
        if (idx < 0 || idx >= bank.pluginCount (ch)) return;
        const int builtIn = bank.pluginBuiltInId (ch, idx);
        if (builtIn == 9)
        {
            destroyMediaPlayer();
            engine_.mediaPlayers().clearPlayer (ch);
        }
        else if (builtIn > 0)
        {
            auto params = engine_.fxParams (ch);
            mb::applyBuiltInToParams (params, builtIn, false);
            engine_.setFxParams (ch, params);
            closeFxPanel();
        }
        closePluginEditor (ch, idx);
        bank.removePlugin (ch, idx);
        refreshStripsFromGraph();
        updateSummary();
        dirty_ = true;
        nodeCanvas_.repaint();
    }

    //==========================================================================
    // 「添加插件」选择浮层：与输入源浮层同款卡片（左自带 / 右第三方）。
    // 前置小圆点显示勾选态；点击条目 = 切换添加/移除，浮层保持打开并原地刷新。
    //==========================================================================
    void showPluginPicker (int ch)
    {
        closePluginPicker();
        pluginPicker_ = std::make_unique<PluginPickerOverlay> (engine_, ch);
        pluginPicker_->onPick = [this, ch] (int id) { togglePlugin (ch, id); };
        pluginPicker_->onClose = [this] { closePluginPicker(); };
        addAndMakeVisible (pluginPicker_.get());
        pluginPicker_->setBounds (getLocalBounds());
        pluginPicker_->toFront (true);
        pluginPicker_->grabKeyboardFocus();
    }

    /** 浮层点击 = 切换：已添加（按内置 id / 插件标识匹配）→ 移除首个实例；
        未添加 → 追加。操作后刷新浮层勾选状态与通道条显示。 */
    void togglePlugin (int ch, int id)
    {
        auto& bank = engine_.pluginBank();

        // —— 自带插件：按内置模块 id 匹配 ——
        if (id >= 1 && id <= mb::kBuiltInPluginCount)
        {
            for (int i = bank.pluginCount (ch) - 1; i >= 0; --i)
                if (bank.pluginBuiltInId (ch, i) == id)
                {
                    if (id == 9)
                    {
                        destroyMediaPlayer();
                        engine_.mediaPlayers().clearPlayer (ch);
                    }
                    else
                    {
                        auto params = engine_.fxParams (ch);
                        mb::applyBuiltInToParams (params, id, false);
                        engine_.setFxParams (ch, params);
                        closeFxPanel();
                    }
                    closePluginEditor (ch, i);
                    bank.removePlugin (ch, i);
                    syncStripAfterPluginChange (ch);   // 移除后立即刷新通道条插件区
                    if (pluginPicker_) pluginPicker_->rebuild();
                    return;
                }
            for (auto& s : strips_)
                if (s->channelIndex() == ch) { s->addPluginById (id); break; }
            if (id == 9) engine_.mediaPlayers().ensurePlayer (ch);
            if (pluginPicker_) pluginPicker_->rebuild();
            return;
        }

        // —— 第三方插件：按插件标识匹配首个实例 ——
        const auto desc = engine_.pluginCatalog().get (id - 1000);
        for (int i = bank.pluginCount (ch) - 1; i >= 0; --i)
        {
            if (bank.pluginBuiltInId (ch, i) > 0) continue;
            if (bank.pluginIdentifier (ch, i) != desc.fileOrIdentifier) continue;
            closePluginEditor (ch, i);
            bank.removePlugin (ch, i);
            syncStripAfterPluginChange (ch);           // 移除后立即刷新通道条插件区
            if (pluginPicker_) pluginPicker_->rebuild();
            return;
        }
        for (auto& s : strips_)
            if (s->channelIndex() == ch) { s->addPluginById (id); break; }
        if (pluginPicker_) pluginPicker_->rebuild();
    }

    /** 插件增删后同步对应通道条的插件区显示：
        插件区绘制在通道条静态层缓存里，必须 invalidate() 重建，否则
        取消选中（移除）后画面残留旧插件行，重启才消失。 */
    void syncStripAfterPluginChange (int ch)
    {
        for (auto& s : strips_)
            if (s->channelIndex() == ch) { s->invalidate(); break; }
        updateSummary();
        dirty_ = true;
    }

    void closePluginPicker()
    {
        if (pluginPicker_ == nullptr) return;
        removeChildComponent (pluginPicker_.get());
        pluginPicker_.reset();
        raisePluginPanels();
    }

    /** 浮层里发生连接增删后：同步引擎、刷新全部通道条并重算浮层列表。 */
    void afterSourceEdit (const juce::String& msg)
    {
        engine_.syncDevices();
        refreshStripsFromGraph();
        updateSummary();
        dirty_ = true;
        setMessage (msg);
        if (sourcePicker_ != nullptr) sourcePicker_->refresh();
    }

    /** 应用内按键兜底：已注册到系统级钩子的组合键由钩子统一触发
        （带修饰键的组合会被钩子吞掉，无修饰键的字符键虽不吞但钩子已触发，
        这里必须跳过，否则一次按键会翻转两次 = 状态不变）。
        钩子未安装 / 按键不支持时仍保留原本的应用内触发。 */
    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        if (hotkeyEngine_.isBound (key)) return false;
        return triggerLinkHotkey (key);
    }

    void parentHierarchyChanged() override
    {
        // 顶层窗口就绪后挂一次键盘钩子
        if (linkKeyHooked_ == nullptr)
            if (auto* tl = getTopLevelComponent())
            {
                tl->addKeyListener (this);
                linkKeyHooked_ = tl;
            }
    }

    void showSetup (bool firstRun, const juce::String& error = {})
    {
        if (setup_ != nullptr) return;
        setup_ = std::make_unique<DeviceSetupOverlay> (engine_, engine_.savedChoice(), firstRun);
        setup_->setError (error);
        setup_->onApply = [this] (mb::DeviceChoice c)
        {
            if (applyDeviceSetup (c)) closeSetup();   // 应用失败则留在界面显示错误
        };
        setup_->onCancel = [this] { closeSetup(); };
        setup_->onNoteChanged = [this]
        {
            nodeCanvas_.updateSize();       // 端口分组标题已换成「备注  原名」
            nodeCanvas_.repaint();
            for (auto& s : strips_) s->repaint();   // 通道条当前输入/输出源文本同步
        };
        addAndMakeVisible (setup_.get());
        setup_->setBounds (getLocalBounds());
    }

    //==========================================================================
    // 应用设备选择（showSetup 弹窗与设置页内嵌「设备设置」Tab 共用）。
    // 返回 false = 应用失败（错误已显示到弹窗 / 内嵌页），调用方不应关闭界面。
    bool applyDeviceSetup (mb::DeviceChoice c)
    {
        // 只改了辅助设备勾选、默认设备没动：不清边、不重扫，保住现有连线
        const auto saved = engine_.savedChoice();
        const bool sameDevice = saved.valid
            && c.typeName == saved.typeName
            && c.inputName  == saved.inputName
            && c.outputName == saved.outputName
            && (c.sampleRate <= 0.0 || std::abs (c.sampleRate - saved.sampleRate) < 1.0)
            && (c.bufferSize <= 0 || c.bufferSize == saved.bufferSize);
        if (sameDevice)
        {
            // 可见性名单变化后必须重建辅助设备与路由：否则刚勾选的设备
            // 虽然出现在菜单里，引擎还没为它打开流 / 编译端口映射。
            engine_.prepareBuffers();
            engine_.syncDevices();
            updateSummary();
            juce::String msg = mb::tr ("辅助设备已更新，现有连线保持不变",
                                       "Aux devices updated; existing connections kept");
            // 设备打开失败时把诊断信息直接给出，避免「连上了却没声音」无从排查
            const auto& devs = engine_.catalog().devices();
            for (size_t di = 0; di < devs.size(); ++di)
            {
                const auto err = engine_.deviceError (static_cast<int> (di));
                if (err.isNotEmpty())
                {
                    msg += mb::S("  ·  ") + devs[di].displayName + mb::S(": ") + err;
                    break;
                }
            }
            setMessage (msg);
            return true;
        }
        // 更换主驱动：先让引擎打开新设备（内部已移除音频回调，不会爆音），
        // 再把旧连线全部作废 —— 端口编号会随驱动重扫变化，留着会指向错误的
        // 物理端口。重点：不再调用 applyDefaultRoutes()，通道一律保持
        // 「未指定输入 / 输出」，由用户按需在通道菜单里自行选择，避免每次
        // 换驱动都被自动塞满默认配线、还要一条条手工移除。
        // 通道数量、名称、顺序、推子等状态全部保持不变。
        if (! engine_.applySetup (c))
        {
            if (setup_ != nullptr) setup_->setError (engine_.error());
            if (settingsPanel_ != nullptr)
                if (auto* dp = settingsPanel_->devicePage())
                    dp->setError (engine_.error());
            return false;
        }
        // 切到 / 切出「（不使用设备）」时，catalog 的端口表顺序不变（scan 只枚举
        // 设备，与是否 markMaster 无关），已有连线的端口 id 依然指向同一物理端口：
        // 主设备端点只是从 master 回调改走辅助流，不需要清边，用户不用重连一遍。
        const bool portIdsStable = (c.inputName.isEmpty() && c.outputName.isEmpty())
                                || (saved.inputName.isEmpty() && saved.outputName.isEmpty());
        if (! portIdsStable)
            graph_.clearEdges();
        engine_.syncDevices();               // 按新路由开 / 关辅助设备流

        // 设备变化后端口 L/R 配对可能变化：重新推导并同步给路由内核，
        // 否则原有立体声通道的边会被误判成「居中」丢失声像。
        {
            const auto inP  = collectPortPairs (true);
            const auto outP = collectPortPairs (false);
            std::vector<std::pair<int, int>> rIn, rOut;
            for (const auto& p : inP)  rIn.push_back ({ p.l, p.r });
            for (const auto& p : outP) rOut.push_back ({ p.l, p.r });
            graph_.setPortPairs (rIn, rOut);
        }
        refreshStripsFromGraph();
        const auto note = engine_.fallbackNote();
        if (portIdsStable)
            setMessage (note.isNotEmpty() ? note
                        : mb::tr ("设备已更新，现有连线保持不变",
                                  "Device updated; existing connections kept"));
        else
            setMessage (note.isNotEmpty() ? note
                        : mb::tr ("已切换主驱动，通道输入/输出留空，请按需手动指定",
                                  "Master driver changed; channel I/O left empty - assign manually"));
        return true;
    }

    void closeSetup()
    {
        if (setup_ == nullptr) return;
        removeChildComponent (setup_.get());
        setup_.reset();
        updateSummary();
    }

    //==========================================================================
    // 新建预设：有未保存改动时先询问（保存 / 不保存 / 取消），否则直接继续。
    // 确认后清空为 0 通道空白预设：断开全部连接、退役全部插件、复位内置效果。
    void newPreset()
    {
        if (presetDirty_) { promptUnsavedThen ([this] (int) { doNewPreset(); }); return; }
        doNewPreset();
    }

    void doNewPreset()
    {
        // 插件实例即将退役：先关掉所有编辑器窗口（它们持有实例指针）
        for (int c = 0; c < mb::ChannelPluginBank::kMaxChannels; ++c)
            for (int i = engine_.pluginBank().pluginCount (c) - 1; i >= 0; --i)
                closePluginEditor (c, i);

        destroyMediaPlayer();        // 面板持有播放器裸指针，退役前先销毁

        {
            juce::ScopedValueSetter<bool> guard (linkSyncing_, true);
            graph_.resize (0, engine_.catalog().inputPortCount(),
                              engine_.catalog().outputPortCount());   // 0 通道、无连接
            for (int c = 0; c < mb::ChannelPluginBank::kMaxChannels; ++c)
                engine_.pluginBank().clearChannel (c);                // 退役全部插件
            for (int c = 0; c < mb::ChannelFxBank::kMaxChannels; ++c)
                engine_.setFxParams (c, {});                          // 复位内置效果
            engine_.mediaPlayers().clearAll();                     // 停播 + 销毁全部播放器（电平立即归零）
        }

        // 随通道的 UI 状态全部清零
        mb::customChannelNames().clear();
        mb::customChannelIcons().clear();
        mb::ui::customChannelColours().clear();
        mb::ui::channelArmState().clear();
        mb::ui::rightAlignedState().clear();
        mb::ui::rightAlignedOrigin().clear();
        mb::ui::channelStereoState().clear();
        mb::ui::channelSlots().clear();
        mb::links::table().clear();

        // 指向通道的浮层一律关闭
        if (fxPanel_ != nullptr) closeFxPanel();
        if (linkOverlay_ != nullptr) closeLinkOverlay();
        if (colourPicker_ != nullptr) closeColourPicker();

        // 新预设尚未落盘：清除当前预设指向，「保存预设」将落到默认位置
        settings_.currentPreset = juce::String();
        mb::AppSettings::save (settings_);

        rebuildStrips();
        engine_.prepareBuffers();
        engine_.syncDevices();
        presetDirty_ = false;
        dirty_ = true;                        // 会话与空白状态同步落盘
        savedSnapshot_ = mb::SessionStore::toJson (graph_, engine_, nodeCanvas_.layoutToJson());   // 新建预设也是显式保存点
        setMessage (mb::tr ("已新建空白预设", "New blank preset created"));
        refreshWindowWidth();
    }

    //==========================================================================
    // 当前预设 / 保存预设（覆盖式）
    juce::File currentPresetFile() const
    {
        if (settings_.currentPreset.isNotEmpty())
            return juce::File (settings_.currentPreset);
        return mb::SessionStore::presetDirectory()
                .getChildFile (mb::tr ("当前预设", "Current") + mb::S(".mbpreset"));
    }

    void saveCurrentPreset()
    {
        const auto f = currentPresetFile();
        const auto snapshot = mb::SessionStore::toJson (graph_, engine_, nodeCanvas_.layoutToJson());
        if (! mb::SessionStore::save (f, snapshot))
        {
            setMessage (mb::tr ("预设保存失败", "Preset save failed"));
            return;
        }
        settings_.currentPreset = f.getFullPathName();
        mb::AppSettings::save (settings_);
        // 会话同步，避免下次启动与刚保存的预设不一致
        mb::SessionStore::save (mb::SessionStore::sessionFile(), snapshot);
        dirty_ = false;
        presetDirty_ = false;
        savedSnapshot_ = snapshot;            // 显式保存点 = 「不保存」回滚基准
        graph_.clearDroppedEdges();           // 已保存 = 文件与设备集一致，撤悬空角标
        nodeCanvas_.repaint();
        setMessage (mb::tr ("预设已保存：", "Preset saved: ") + f.getFileName());
    }

    //==========================================================================
    /** 把设置页的录音配置应用到录音器（模式 / 格式 / 位深 / 目录）。 */
    void applyRecorderSettings (const mb::AppSettingsData& s)
    {
        engine_.recorder().setMode (s.recordMode == "multi"
            ? mb::ChannelRecorder::Mode::MultiTrack : mb::ChannelRecorder::Mode::Sum);
        engine_.recorder().setFormat (s.recordFormat);
        engine_.recorder().setBitDepth (s.recordBitDepth);
        if (s.recordDir.isNotEmpty())
            engine_.recorder().setDirectory (juce::File (s.recordDir));
    }

    /** 危险操作二次确认（异步弹窗，兼容 JUCE_MODAL_LOOPS_PERMITTED=0）。 */
    void confirmThen (const juce::String& title, const juce::String& body,
                      std::function<void()> action)
    {
        auto* aw = new juce::AlertWindow (title, body, juce::MessageBoxIconType::NoIcon);
        aw->addButton (mb::tr ("确认", "OK"),     1);
        aw->addButton (mb::tr ("取消", "Cancel"), 0);
        juce::Component::SafePointer<MainComponent> safe (this);
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create ([safe, action] (int r)
            {
                if (safe != nullptr && r == 1) action();
            }),
            true);
    }

    void confirmClearRoutes()
    {
        confirmThen (mb::tr ("清除连接", "Clear connections"),
                     mb::tr ("将移除全部路由连接（通道与预设不受影响），确定继续？",
                             "This removes ALL routing connections (channels and presets are kept). Continue?"),
                     [this] { clearRoutes(); });
    }

    // 设置页
    void showSettings()
    {
        if (settingsPanel_ != nullptr) return;
        auto p = std::make_unique<SettingsPanel> (settings_, engine_.sampleRate(), engine_);
        p->onClose = [this] { closeSettings(); };
        p->onDiagnostics = [this] { closeSettings(); showDiagnostics(); };
        p->onLaunchNewInstance = [this] { closeSettings(); launchNewInstance(); };
        p->onRestartApp        = [this] { restartApplication(); };
        // 内嵌页接线：设备设置 / 插件库直接在 Tab 内生效，不再弹窗
        if (auto* dp = p->devicePage())
        {
            dp->onApply  = [this] (mb::DeviceChoice c) { applyDeviceSetup (c); };
            dp->onCancel = [this]
            {
                if (auto* d = settingsPanel_ != nullptr ? settingsPanel_->devicePage() : nullptr)
                    d->setError ({});
            };
            dp->onNoteChanged = [this]
            {
                nodeCanvas_.updateSize();
                nodeCanvas_.repaint();
                for (auto& s : strips_) s->repaint();
            };
        }
        if (auto* pp = p->pluginPage())
        {
            pp->onMessage = [this] (const juce::String& m) { setMessage (m); };
            pp->onListChanged = [this] { for (auto& s : strips_) s->repaint(); };
        }
        p->onLightTheme = [this] (bool light)
        {
            settings_.lightTheme = light;
            mb::AppSettings::save (settings_);
            setLightTheme (light);
            if (settingsPanel_ != nullptr) settingsPanel_->syncLightTheme (light);
        };
        p->onLanguageToggle = [this]
        {
            toggleLanguage();
            settings_.chinese = mb::isZh();
            mb::AppSettings::save (settings_);
            presetDirty_ = false;           // 语言切换不算调音台改动
        };
        p->onRecordSampleRate = [this] (double sr)
        {
            if (sr > 8000.0) engine_.setDeviceSampleRate (sr);   // 0 = 跟随设备，不动
        };
        // 全局缩放只放大渲染、不改变窗口物理尺寸 → 右侧通道会被裁掉。
        // 按新旧比例同步放大/缩小窗口（clamp 到上下限），内容始终完整可见。
        p->onUiScaleChanged = [this] (double scale, double prev)
        {
            if (prev <= 0.0 || std::abs (scale - prev) < 0.005) return;
            const double ratio = scale / prev;
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            {
                const int nw = juce::jlimit (mb::ui::kMinWindowWidth, mb::ui::kMaxWindowWidth,
                                             juce::roundToInt (w->getWidth() * ratio));
                const int nh = juce::jlimit (mb::ui::kMinWindowHeight, mb::ui::kMaxWindowHeight,
                                             juce::roundToInt (w->getHeight() * ratio));
                w->setSize (nw, nh);
            }
        };
        // 录入快捷键时临时抑制全局钩子，否则正在录入的组合键会被钩子吞掉
        p->onHotkeyCapture = [this] (bool capturing)
        {
            hotkeyEngine_.setSuppressed (capturing);
        };
        p->setHotkeyStatus (settings_.hideHotkeyKey == 0
                            || hotkeyEngine_.isBound (settings_.hideHotkeyKey,
                                                      settings_.hideHotkeyMods));
        p->setUnlockHotkeyStatus (unlockHotkeyActive());
        p->onSettingsChanged = [this] (mb::AppSettingsData s)
        {
            // 设置页持有的是打开时的副本，浮层窗口状态可能在其后被改动，先保留
            const auto fxCard = settings_.fxCard, mediaCard = settings_.mediaCard;
            // 桌面歌词窗口可能被用户拖过并即时写盘，别被设置页的旧副本覆盖
            const bool   lyrHas = settings_.desktopLyricsHasBounds;
            const bool   lyrLocked = settings_.desktopLyricsLocked;
            const double lyrX = settings_.desktopLyricsX, lyrY = settings_.desktopLyricsY,
                         lyrW = settings_.desktopLyricsW, lyrH = settings_.desktopLyricsH;
            settings_ = s;
            settings_.fxCard = fxCard;
            settings_.mediaCard = mediaCard;
            settings_.desktopLyricsHasBounds = lyrHas;
            settings_.desktopLyricsLocked = lyrLocked;   // 锁定由窗口/热键即时改动，设置页副本滞后
            settings_.desktopLyricsX = lyrX; settings_.desktopLyricsY = lyrY;
            settings_.desktopLyricsW = lyrW; settings_.desktopLyricsH = lyrH;
            settings_.chinese = mb::isZh();   // 面板里的语言字段不覆盖运行时语言
            settings_.languageMode = mb::languageMode();   // 面板副本可能滞后于手动切换
            mb::ui::channelBorderMode() = s.channelBorder;
            applyRecorderSettings (s);
            mb::AppSettings::save (settings_);
            mb::AppSettings::applyAutoStart (s.autoStart);
            syncTrayIcon();
            applyHideHotkey();               // 快捷键 / 隐藏方式变化即时重新注册
            applyAlwaysOnTop();              // 置顶选项即时生效（含桌面歌词窗口）
            // invalidate 强制静态层重建：顶部圆点颜色 / 名称条左缘随边框样式即时刷新
            for (auto& st : strips_) st->invalidate();
        };
        settingsPanel_ = std::move (p);
        addAndMakeVisible (settingsPanel_.get());
        settingsPanel_->setBounds (getLocalBounds());
        settingsPanel_->toFront (false);
    }

    void closeSettings()
    {
        if (settingsPanel_ == nullptr) return;
        hotkeyEngine_.setSuppressed (false);   // 录入中途关闭也要恢复全局热键
        removeChildComponent (settingsPanel_.get());
        settingsPanel_.reset();
        raisePluginPanels();
    }

    //==========================================================================
    // 系统托盘
    static juce::Image makeTrayImage()
    {
        juce::Image im (juce::Image::ARGB, 32, 32, true);
        juce::Graphics g (im);
        g.setColour (mb::ui::accent[0]);
        g.fillRoundedRectangle (1.0f, 1.0f, 30.0f, 30.0f, 7.0f);
        g.setColour (juce::Colours::white);
        g.setFont (mb::ui::font (18.0f, true));
        g.drawText (mb::S("M"), im.getBounds(), juce::Justification::centred, false);
        return im;
    }

    /** 托盘右键：所有项等高、不插分隔线（分隔线会额外占 9px，导致组间/组内
        上下间距不一致）。分组靠文案语义区分，列表整体等距。 */
    void showTrayMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, mb::tr ("显示主窗口", "Show main window"));
        m.addItem (30, mb::tr ("设置", "Settings"));
        m.addItem (10, mb::tr ("开机自动启动", "Launch at startup"),
                   true, settings_.autoStart);
        m.addItem (11, mb::tr ("关闭时隐藏到托盘", "Hide to tray on close"),
                   true, settings_.closeToTray);
        m.addItem (12, mb::tr ("启动时隐藏到托盘", "Start minimized to tray"),
                   true, settings_.startMinimizedToTray);
        // 动作型菜单项：标题直接写「点它会发生什么」，永远不打勾。
        // 勾选表示「当前状态」，与动作标题并用会造成语义矛盾（锁定时显示
        // 「解锁桌面歌词」却打着勾，用户分不清下一步是解锁还是锁定）。
        // 锁定后鼠标穿透、窗口点不到，这里是快捷键之外的兜底切换入口。
        if (desktopLyricsWindow() != nullptr)
            m.addItem (13, settings_.desktopLyricsLocked
                            ? mb::tr ("解锁桌面歌词", "Unlock desktop lyrics")
                            : mb::tr ("锁定桌面歌词（鼠标穿透）", "Lock desktop lyrics (click-through)"));
        m.addItem (20, mb::tr ("另起新实例", "New instance"));
        m.addItem (21, mb::tr ("重启", "Restart"));
        m.addItem (2, mb::tr ("退出", "Quit"));
        m.showMenuAsync (juce::PopupMenu::Options(), [this] (int r)
        {
            auto flip = [&] (bool& v) { v = ! v; mb::AppSettings::save (settings_); };
            switch (r)
            {
                case 1: if (onShowWindow) onShowWindow(); break;
                case 2: quitFromTray(); break;
                case 10: flip (settings_.autoStart);
                         mb::AppSettings::applyAutoStart (settings_.autoStart); break;
                case 11: flip (settings_.closeToTray); syncTrayIcon(); break;
                case 12: flip (settings_.startMinimizedToTray); syncTrayIcon(); break;
                case 13: toggleDesktopLyricsLock(); break;
                case 20: launchNewInstance(); break;
                case 21: restartApplication(); break;
                case 30: if (onShowWindow) onShowWindow();   // 最小化到托盘时先唤回窗口
                         showSettings(); break;
                default: break;
            }
        });
    }

    /** 托盘退出：真正退出应用——有未保存改动时才弹窗询问。 */
    void quitFromTray()
    {
        if (quitPromptOpen_) return;
        if (! presetDirty_) { finishQuit (false); return; }
        promptUnsavedThen ([this] (int choice) { finishQuit (false, choice == 2); });
    }

    /** 启用任一托盘选项时常驻图标；全部关闭时移除。图标用应用图标（app_icon）。
        注：「最小化时折叠窗口」已改为卷起折叠（不进托盘），故不再需要托盘图标。 */
    void syncTrayIcon()
    {
        // 设了「隐藏/恢复」快捷键且选「隐藏到托盘」时，图标也必须常驻，
        // 否则隐藏后没有可点击恢复的入口，会与「完全隐藏」表现完全一样
        const bool hotkeyTray = settings_.hideHotkeyKey != 0 && settings_.hideMode == 0;
        // 桌面歌词锁定时鼠标穿透、窗口点不到，托盘图标必须常驻以便右键解锁
        const bool need = settings_.closeToTray || settings_.startMinimizedToTray
                       || hotkeyTray || settings_.desktopLyricsLocked;
        if (! need) { tray_.reset(); return; }
        if (tray_ != nullptr) return;

        auto icon = std::make_unique<TrayIcon>();
        // 托盘图标用应用图标；二进制资源缺失时回退到内绘字母图
        auto im = appIcon_.isValid() ? appIcon_.rescaled (32, 32) : makeTrayImage();
        icon->setIconImage (im, im);
        icon->setIconTooltip (mb::S("MicroBridge"));
        icon->onShow = [this] { if (onShowWindow) onShowWindow(); };
        icon->onMenu = [this] { showTrayMenu(); };
        icon->onQuit = [this] { quitFromTray(); };
        tray_ = std::move (icon);
    }

    /** 依据当前设置 + 通道链接表重建系统级全局快捷键绑定。
        隐藏热键与所有通道快捷键共用一个低级键盘钩子；通道快捷键按 (键,修饰键)
        去重 —— 同码共享组的多条通道只需注册一次，命中时 triggerLinkHotkey
        会按动作把整组一起翻转。 */
    void refreshGlobalHotkeys()
    {
        hotkeyDigest_ = currentHotkeyDigest();

        std::vector<mb::GlobalHotkey::Binding> bs;
        if (settings_.hideHotkeyKey != 0)
            bs.push_back ({ settings_.hideHotkeyKey, settings_.hideHotkeyMods,
                            [this] { if (hotkeyEngine_.onTrigger) hotkeyEngine_.onTrigger(); } });

        // 锁定 / 解锁桌面歌词切换键（默认 Ctrl+Alt+L）：与隐藏热键 / 通道快捷键撞键时不注册
        if (settings_.unlockLyricsHotkeyKey != 0 && ! unlockHotkeyConflict())
            bs.push_back ({ settings_.unlockLyricsHotkeyKey, settings_.unlockLyricsHotkeyMods,
                            [this] { toggleDesktopLyricsLock(); } });

        juce::StringArray seen;
        const int n = graph_.channelCount();
        for (int c = 0; c < n; ++c)
            for (int a = 0; a < mb::links::kActionCount; ++a)
            {
                const auto l = mb::links::linkOf (c, a);
                if (! l.hasHotkey() || a == mb::links::kVolume) continue;  // 音量无翻转语义
                const auto tag = juce::String (l.key) + mb::S(":") + juce::String (l.mods);
                if (seen.contains (tag)) continue;
                seen.add (tag);
                bs.push_back ({ l.key, l.mods, [this, key = l.key, mods = l.mods]
                {
                    triggerLinkHotkey (juce::KeyPress (key, juce::ModifierKeys (mods), 0));
                } });
            }

        if (! hotkeyEngine_.setBindings (std::move (bs)))
            setMessage (mb::tr ("部分全局快捷键未生效：该按键不支持或钩子安装失败",
                                "Some global hotkeys inactive: unsupported key or hook failed"));

        // 两条录入按钮都回写「已生效 / 未生效」（含撞键被跳过的情况）
        if (settingsPanel_ != nullptr)
        {
            settingsPanel_->setHotkeyStatus (settings_.hideHotkeyKey == 0
                || hotkeyEngine_.isBound (settings_.hideHotkeyKey, settings_.hideHotkeyMods));
            settingsPanel_->setUnlockHotkeyStatus (unlockHotkeyActive());
        }
    }

    /** 绑定指纹：设置页隐藏热键 + 每条通道每个动作的快捷键（含动作，变化即重建）。 */
    juce::String currentHotkeyDigest() const
    {
        juce::String s = juce::String (settings_.hideHotkeyKey) + mb::S(":")
                       + juce::String (settings_.hideHotkeyMods)
                       + mb::S("|u:") + juce::String (settings_.unlockLyricsHotkeyKey)
                       + mb::S(":") + juce::String (settings_.unlockLyricsHotkeyMods);
        const int n = graph_.channelCount();
        for (int c = 0; c < n; ++c)
            for (int a = 0; a < mb::links::kActionCount; ++a)
            {
                const auto l = mb::links::linkOf (c, a);
                if (! l.hasHotkey() || a == mb::links::kVolume) continue;
                s += mb::S("|") + juce::String (l.key) + mb::S(":")
                   + juce::String (l.mods) + mb::S(":") + juce::String (a);
            }
        return s;
    }

    /** 设置页改动隐藏热键后：立即重建绑定，并把生效结果显示在录入按钮上。 */
    void applyHideHotkey()
    {
        refreshGlobalHotkeys();      // 内部同时回写两条录入按钮的生效状态
    }

    //==========================================================================
    // 桌面歌词锁定：窗口按钮 / 全局热键 / 托盘三条入口共用同一份状态
    //==========================================================================
    DesktopLyricsWindow* desktopLyricsWindow() const noexcept
    {
        return mediaPanel_ != nullptr ? mediaPanel_->desktopLyricsWindow() : nullptr;
    }

    static int hotkeyModMask (int mods) noexcept
    {
        return mods & (juce::ModifierKeys::ctrlModifier
                     | juce::ModifierKeys::altModifier
                     | juce::ModifierKeys::shiftModifier);
    }

    /** 锁定/解锁热键是否与隐藏热键 / 通道快捷键撞键（撞键则跳过注册，避免一次按键触发两个动作）。 */
    bool unlockHotkeyConflict() const
    {
        const int key = settings_.unlockLyricsHotkeyKey;
        if (key == 0) return false;
        const int mods = hotkeyModMask (settings_.unlockLyricsHotkeyMods);
        if (settings_.hideHotkeyKey == key
            && hotkeyModMask (settings_.hideHotkeyMods) == mods) return true;
        for (int c = 0; c < graph_.channelCount(); ++c)
            for (int a = 0; a < mb::links::kActionCount; ++a)
            {
                const auto l = mb::links::linkOf (c, a);
                if (a != mb::links::kVolume && l.hasHotkey()
                    && l.key == key && hotkeyModMask (l.mods) == mods) return true;
            }
        return false;
    }

    bool unlockHotkeyActive() const
    {
        return settings_.unlockLyricsHotkeyKey == 0
            || (! unlockHotkeyConflict()
                && hotkeyEngine_.isBound (settings_.unlockLyricsHotkeyKey,
                                          settings_.unlockLyricsHotkeyMods));
    }

    /** 锁定态写盘 + 托盘入口刷新（窗口自身状态由调用方同步，避免回环）。 */
    void applyDesktopLyricsLock (bool locked)
    {
        settings_.desktopLyricsLocked = locked;
        mb::AppSettings::save (settings_);
        syncTrayIcon();
    }

    /** 全局热键 / 托盘菜单触发：翻转锁定态（未锁定→锁定，锁定→解锁）。
        锁定态下窗口收不到鼠标，这个系统级热键是主要解锁通道。 */
    void toggleDesktopLyricsLock()
    {
        auto* w = desktopLyricsWindow();
        if (w == nullptr)
        {
            setMessage (mb::tr ("桌面歌词未打开", "Desktop lyrics window is not open"));
            return;
        }
        const bool next = ! w->isLocked();
        w->setLocked (next, false);
        applyDesktopLyricsLock (next);
    }

    //==========================================================================
    // 关闭 / 退出：未保存改动时异步询问（JUCE_MODAL_LOOPS_PERMITTED=0）。
    // action 收到按钮值：1 = 保存后继续，2 = 不保存继续（改动全部作废）。
    void promptUnsavedThen (const std::function<void (int choice)>& action)
    {
        if (! presetDirty_) { action (1); return; }
        if (quitPromptOpen_) return;
        quitPromptOpen_ = true;

        // 不用 showAsync：原生 / 自绘两种 AlertWindow 的按钮编号语义不同，
        // 这里用 addButton 显式指定返回值，兼容任意 LookAndFeel。
        auto* aw = new juce::AlertWindow (
            mb::tr ("调音台有未保存的改动", "Unsaved mixer changes"),
            mb::tr ("是否保存当前预设？", "Save the current preset?"),
            juce::MessageBoxIconType::QuestionIcon);
        aw->addButton (mb::tr ("保存", "Save"),       1);
        aw->addButton (mb::tr ("不保存", "Don't save"), 2);
        aw->addButton (mb::tr ("取消", "Cancel"),     0);

        juce::Component::SafePointer<MainComponent> safe (this);
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create ([safe, action] (int r)
            {
                if (safe == nullptr) return;
                safe->quitPromptOpen_ = false;
                if (r == 0) return;                       // 取消 / 关闭：留在应用里
                if (r == 1) safe->saveCurrentPreset();
                safe->presetDirty_ = false;
                action (r);
            }),
            true);
    }

    /** 隐藏到托盘，或真正退出（自行 quit，调用方不要再触发退出）。
        discardUnsaved = 用户在退出询问里选了「不保存」：本次运行的全部改动
        （含约 2 s 自动保存已写盘的）都不得生效 —— 把会话文件回滚到上次
        显式保存点（启动恢复内容 / 最近一次保存·导出·导入预设 / 新建预设），
        重启后恢复的就是不保存前的样子。 */
    void finishQuit (bool allowTrayHide, bool discardUnsaved = false)
    {
        if (discardUnsaved)
        {
            dirty_ = false;                       // 退出间隙不让定时器再把新状态写盘
            saveTick_ = 0;
            if (! savedSnapshot_.isVoid())
                mb::SessionStore::save (mb::SessionStore::sessionFile(), savedSnapshot_);
        }
        else
        {
            // 退出 / 隐藏托盘前强制落盘一次：插件私有状态可能在上次 5 s 指纹
            // 检测之后又变了（例如刚导入歌曲就关窗），不能丢。
            saveSessionQuietly();
        }

        if (allowTrayHide && settings_.closeToTray)
        {
            if (onHideWindow) onHideWindow();
            if (tray_ != nullptr)
                tray_->showInfoBubble (mb::S("MicroBridge"),
                    mb::tr ("已隐藏到托盘，点击图标恢复", "Minimized to tray; click the icon to restore"));
            return;
        }
        juce::JUCEApplication::getInstance()->quit();
    }

    //==========================================================================
    // 会话 / 预设

    /** 预设体验 5b：装载后有因设备不匹配而未恢复的路由时，在消息条给出
        完整提示（覆盖 prefix），NodeView 通道卡由 droppedChannels 驱动红色角标。
        角标持续到用户重新保存预设（届时文件与当前设备集一致）为止。 */
    void reportDroppedRoutes (const juce::String& prefix)
    {
        const int n = static_cast<int> (graph_.droppedEdges().size());
        if (n <= 0) return;
        setMessage (prefix + mb::tr (" —— ", " — ")
                    + juce::String (n) + mb::tr (" 条路由因当前设备不匹配未恢复，"
                    "节点视图已用红色 ! 角标标记相关通道；重新接线并保存预设后清除。",
                    " route(s) not restored (device mismatch), marked with red "
                    "! badges in Node View; re-wire and re-save the preset to clear."));
        nodeCanvas_.repaint();
    }

    bool tryRestoreSession()
    {
        const auto v = mb::SessionStore::load (mb::SessionStore::sessionFile());
        if (! v.isObject()) return false;
        if (! mb::SessionStore::fromJson (v, graph_, engine_,
                                      engine_.catalog().inputPortCount(),
                                      engine_.catalog().outputPortCount()))
            return false;
        nodeCanvas_.layoutFromJson (v.getProperty ("nodeLayout", juce::var()));
        engine_.prepareBuffers();
        return true;
    }

    void exportPreset()
    {
        mb::SessionStore::presetDirectory().createDirectory();
        presetChooser_ = std::make_unique<juce::FileChooser> (
            mb::tr ("导出预设", "Export preset"), mb::SessionStore::presetDirectory(),
            mb::S("*.mbpreset"));
        presetChooser_->launchAsync (juce::FileBrowserComponent::saveMode
                                   | juce::FileBrowserComponent::canSelectFiles
                                   | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f == juce::File()) return;
                if (! f.hasFileExtension (mb::S(".mbpreset")))
                    f = f.withFileExtension (mb::S("mbpreset"));
                if (mb::SessionStore::save (f, mb::SessionStore::toJson (graph_, engine_, nodeCanvas_.layoutToJson())))
                {
                    settings_.currentPreset = f.getFullPathName();
                    mb::AppSettings::save (settings_);
                    presetDirty_ = false;
                    savedSnapshot_ = mb::SessionStore::toJson (graph_, engine_, nodeCanvas_.layoutToJson());   // 导出 = 显式保存点
                    graph_.clearDroppedEdges();        // 已保存 = 文件与设备集一致，撤悬空角标
                    nodeCanvas_.repaint();
                    setMessage (mb::tr ("预设已导出：", "Preset saved: ") + f.getFileName());
                }
                else
                    setMessage (mb::tr ("预设导出失败", "Preset export failed"));
            });
    }

    void importPreset()
    {
        presetChooser_ = std::make_unique<juce::FileChooser> (
            mb::tr ("导入预设", "Import preset"), mb::SessionStore::presetDirectory(),
            mb::S("*.mbpreset"));
        presetChooser_->launchAsync (juce::FileBrowserComponent::openMode
                                   | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (! f.existsAsFile()) return;
                bool linkGuard = linkSyncing_;               // 装载期抑制链接联动
                linkSyncing_ = true;
                const auto imported = mb::SessionStore::load (f);
                const bool ok = mb::SessionStore::fromJson (imported, graph_, engine_,
                                            engine_.catalog().inputPortCount(),
                                            engine_.catalog().outputPortCount());
                linkSyncing_ = linkGuard;
                if (ok)
                {
                    nodeCanvas_.layoutFromJson (imported.getProperty ("nodeLayout", juce::var()));
                    syncChannelNames();      // 预设里名字不足时补齐
                    syncAlignmentVector();
                    engine_.prepareBuffers();
                    engine_.syncDevices();
                    rebuildStrips();
                    for (auto& s : strips_) s->syncFromGraph();
                    settings_.currentPreset = f.getFullPathName();
                    mb::AppSettings::save (settings_);
                    presetDirty_ = false;
                    savedSnapshot_ = mb::SessionStore::toJson (graph_, engine_, nodeCanvas_.layoutToJson());   // 导入 = 显式保存点
                    setMessage (mb::tr ("已导入预设：", "Preset loaded: ") + f.getFileName());
                    reportDroppedRoutes (mb::tr ("已导入预设：", "Preset loaded: ") + f.getFileName());
                }
                else
                    setMessage (mb::tr ("预设导入失败（格式不正确）", "Preset import failed"));
            });
    }

    //==========================================================================
    // 插件编辑器窗口（双击通道条插件行 / 菜单「打开界面」）
    void openPluginEditor (int ch, int idx)
    {
        auto* p = engine_.pluginBank().pluginInstance (ch, idx);
        if (p == nullptr) return;

        for (auto* w : pluginWindows_)
            if (w->isFor (p))
            {
                w->toFront (true);
                return;
            }

        auto* w = new PluginWindow (p);
        w->onClose = [this, w] { pluginWindows_.removeObject (w, true); };
        pluginWindows_.add (w);
    }

    void closePluginEditor (int ch, int idx)
    {
        auto* p = engine_.pluginBank().pluginInstance (ch, idx);
        if (p == nullptr) return;
        for (auto* w : pluginWindows_)
            if (w->isFor (p))
            {
                pluginWindows_.removeObject (w, true);
                return;
            }
    }

    //==========================================================================
    // 录音机：录制所有已布防（R）的通道
    void toggleRecording()
    {
        if (engine_.recorder().activeCount() > 0) { stopRecording(); return; }

        int started = 0;
        for (int c = 0; c < graph_.channelCount(); ++c)
        {
            if (! mb::ui::isChannelArmed (c)) continue;
            if (engine_.recorder().start (c, mb::channelDisplayName (c), engine_.sampleRate()))
                ++started;
        }
        if (started == 0)
        {
            setMessage (mb::tr ("没有已布防的通道：先在通道条上按 R 布防",
                                "No armed channels: press R on a strip first"));
            return;
        }
        recStartMs_ = juce::Time::getMillisecondCounter();
        recPausedTotalMs_ = 0;
        recPausedAtMs_ = 0;
        setMessage (mb::tr ("开始录音：", "Recording started: ") + juce::String (started)
                  + mb::tr (" 个通道", " channel(s)"));
    }

    /** 暂停 / 继续：录音会话保持，暂停期间样本丢弃、计时停走。 */
    void toggleRecordingPause()
    {
        if (engine_.recorder().activeCount() == 0)
        {
            setMessage (mb::tr ("没有进行中的录音", "Not recording"));
            return;
        }
        const bool toPaused = ! engine_.recorder().isPaused();
        engine_.recorder().setPaused (toPaused);
        const auto now = juce::Time::getMillisecondCounter();
        if (toPaused)
        {
            recPausedAtMs_ = now;
        }
        else if (recPausedAtMs_ != 0)
        {
            recPausedTotalMs_ += now - recPausedAtMs_;   // 累计暂停时长，计时扣除
            recPausedAtMs_ = 0;
        }
        setMessage (toPaused ? mb::tr ("录音已暂停", "Recording paused")
                             : mb::tr ("录音继续", "Recording resumed"));
        refreshRecorderBar();
    }

    void stopRecording()
    {
        int n = 0;
        juce::String last;
        for (int c = 0; c < graph_.channelCount(); ++c)
        {
            if (! engine_.recorder().isRecording (c)) continue;
            const auto f = engine_.recorder().stop (c);
            ++n;
            if (f.existsAsFile()) last = f.getFileName();
        }
        recorderBar_.lastFile = last;
        engine_.recorder().setPaused (false);        // 停止后复位暂停状态与计时
        recPausedTotalMs_ = 0;
        recPausedAtMs_ = 0;
        refreshRecorderBar();
        setMessage (n > 0 ? mb::tr ("录音完成，已保存 ", "Saved ") + juce::String (n)
                              + mb::tr (" 个文件", " file(s)")
                          : mb::tr ("没有进行中的录音", "Not recording"));
    }

    void refreshRecorderBar()
    {
        recorderBar_.activeCount = engine_.recorder().activeCount();

        // 录音电平：未开录时跟随「已布防 R」的通道，开录后跟随录制中的通道；
        // 快攻慢释——有声瞬间顶上去，静音后缓缓回落
        const bool rec = engine_.recorder().activeCount() > 0;
        float target = 0.0f;
        for (int c = 0; c < graph_.channelCount(); ++c)
        {
            const bool relevant = rec ? engine_.recorder().isRecording (c)
                                      : mb::ui::isChannelArmed (c);
            if (relevant) target = juce::jmax (target, engine_.channelPeak (c));
        }
        target = mb::ui::levelToMeter (target);
        auto& disp = recorderBar_.level;
        disp = target > disp ? target : disp * 0.82f + target * 0.18f;
        if (disp < 0.003f) disp = 0.0f;
        int armed = 0;
        for (int c = 0; c < graph_.channelCount(); ++c)
            if (mb::ui::isChannelArmed (c)) ++armed;
        recorderBar_.armedCount = armed;

        if (recorderBar_.activeCount > 0)
        {
            const bool paused = engine_.recorder().isPaused();
            recorderBar_.paused = paused;
            // 暂停时计时冻结在暂停瞬间；恢复后扣除累计暂停时长
            const auto endMs = paused ? recPausedAtMs_
                                      : juce::Time::getMillisecondCounter();
            const int total = static_cast<int> ((endMs - recStartMs_ - recPausedTotalMs_) / 1000);
            recorderBar_.elapsed = juce::String (total / 60).paddedLeft ('0', 2)
                                 + mb::S(":")
                                 + juce::String (total % 60).paddedLeft ('0', 2);
        }
        else
        {
            recorderBar_.elapsed = {};
            recorderBar_.paused = false;
        }
        // 空闲且各显示量无变化时跳过重绘（电平静止后 disp 恒为 0）
        if (recorderBar_.level != lastRecLevel_
            || recorderBar_.activeCount != lastRecActive_
            || recorderBar_.armedCount != lastRecArmed_
            || recorderBar_.elapsed != lastRecElapsed_
            || recorderBar_.paused != lastRecPaused_)
        {
            recorderBar_.repaint();
            lastRecLevel_ = recorderBar_.level;
            lastRecActive_ = recorderBar_.activeCount;
            lastRecArmed_ = recorderBar_.armedCount;
            lastRecElapsed_ = recorderBar_.elapsed;
            lastRecPaused_ = recorderBar_.paused;
        }
    }

    //==========================================================================
    // 通道效果器面板 / 设备诊断
    void showFx (int channelIndex, int fxSlot = -1)
    {
        if (fxSlot == 9) { showMediaPlayer (channelIndex); return; }   // 音频播放走自己的面板
        if (fxPanel_ != nullptr)
        {
            saveFxCardState();
            removeChildComponent (fxPanel_.get());
        }
        fxPanel_ = std::make_unique<FxPanel> (engine_, channelIndex, fxSlot);
        fxPanel_->onChanged = [this]
        {
            for (auto& s : strips_) s->syncFromGraph();
        };
        fxPanel_->onClose = [this] { closeFxPanel(); };
        fxPanel_->onStateChanged = [this] { saveFxCardState(); };
        addAndMakeVisible (fxPanel_.get());
        fxPanel_->setBounds (getLocalBounds());
        fxPanel_->applyCardState (fromSettingsCard (settings_.fxCard));
        fxPanel_->toFront (false);
    }

    void closeFxPanel()
    {
        if (fxPanel_ == nullptr) return;
        saveFxCardState();
        removeChildComponent (fxPanel_.get());
        fxPanel_.reset();
        for (auto& s : strips_) s->syncFromGraph();
    }

    /** 内置插件浮层（效果器 / 音频播放）常驻主窗口上层：其它临时浮层关闭、
        视图切换或布局重算后，把它们重新抬回基础控件之上（展开 / 折叠都生效）。 */
    void raisePluginPanels()
    {
        if (fxPanel_    != nullptr) fxPanel_->bringToFront();
        if (mediaPanel_ != nullptr) mediaPanel_->bringToFront();
    }

    //==========================================================================
    // 「音频播放」自带插件面板
    //==========================================================================
    void showMediaPlayer (int channelIndex)
    {
        // 关闭播放器只是隐藏到后台，面板仍在：再次点「+插件」直接把它唤回，
        // 播放列表 / 歌词 / 桌面歌词窗口状态原样保留。
        if (mediaPanel_ != nullptr && mediaChannel_ == channelIndex)
        {
            if (! mediaPanel_->isVisible()) mediaPanel_->setVisible (true);
            mediaPanel_->toFront (true);
            return;
        }
        destroyMediaPlayer();
        mediaChannel_ = channelIndex;
        mediaPanel_ = std::make_unique<MediaPlayerPanel> (engine_, graph_, channelIndex);
        mediaPanel_->onChanged = [this] { updateSummary(); };
        mediaPanel_->onClose = [this] { closeMediaPlayer(); };
        mediaPanel_->onStateChanged = [this] { saveMediaCardState(); };
        mediaPanel_->onDesktopLyricsPinChanged = [this] (bool on)
        {
            settings_.desktopLyricsOnTop = on;
            mb::AppSettings::save (settings_);
            if (settingsPanel_ != nullptr) settingsPanel_->syncDesktopLyricsOnTop (on);
        };
        mediaPanel_->onDesktopLyricsLockChanged = [this] (bool on)
        {
            applyDesktopLyricsLock (on);
        };
        mediaPanel_->onDesktopLyricsBoundsChanged = [this] (juce::Rectangle<int> b)
        {
            settings_.desktopLyricsHasBounds = true;
            settings_.desktopLyricsX = b.getX();  settings_.desktopLyricsY = b.getY();
            settings_.desktopLyricsW = b.getWidth(); settings_.desktopLyricsH = b.getHeight();
        };
        addAndMakeVisible (mediaPanel_.get());
        mediaPanel_->setBounds (getLocalBounds());
        mediaPanel_->applyCardState (fromSettingsCard (settings_.mediaCard));
        mediaPanel_->toFront (false);
    }

    /** 点播放器关闭按钮：只隐藏面板，后台播放与桌面歌词继续运行。
        面板的 30fps timer 不依赖可见性，歌词/主题/换曲仍会推给桌面歌词窗口。 */
    void closeMediaPlayer()
    {
        if (mediaPanel_ == nullptr) return;
        saveMediaCardState();
        mediaPanel_->setVisible (false);
    }

    /** 真正销毁：移除内置插件、切换通道、新建预设（播放器实例即将退役）。 */
    void destroyMediaPlayer()
    {
        if (mediaPanel_ == nullptr) return;
        saveMediaCardState();
        removeChildComponent (mediaPanel_.get());
        mediaPanel_.reset();
        mediaChannel_ = -1;
    }

    void showDiagnostics()
    {
        if (diagPanel_ != nullptr) return;
        diagPanel_ = std::make_unique<DiagnosticsPanel> (engine_, engine_.recorder());
        diagPanel_->onClose = [this]
        {
            removeChildComponent (diagPanel_.get());
            diagPanel_.reset();
            raisePluginPanels();
        };
        diagPanel_->onMessage = [this] (const juce::String& m) { setMessage (m); };
        addAndMakeVisible (diagPanel_.get());
        diagPanel_->setBounds (getLocalBounds());
    }

    //==========================================================================
    mb::RouteGraph graph_;
    mb::AudioEngine engine_;
    juce::OwnedArray<PluginWindow> pluginWindows_;   // 须在 engine_ 之后析构
    juce::TooltipWindow tooltipWindow_;              // 悬停提示（收纳模式下按钮仅有图标）
    int drawerW_ = mb::ui::kDrawerWidth;             // 抽屉当前宽度（收纳时收窄）
    bool drawerCollapsed_ = false;
    bool nodeViewMode_ = false;                      // 主区当前投影：调音台 / 节点连接视图
    bool recorderVisible_ = true;                    // 顶部录音区显隐（重启恢复）
    bool folded_ = false;                            // 窗口卷起：只保留通道底部控制带

    juce::Viewport viewport_;
    StripContainer stripContainer_;
    DragOverlay dragOverlay_;                 // stripContainer_ 的子组件，不拦截鼠标
    int dragChannel_ = -1;                   // 当前高亮的通道（跟随数据换位）
    std::vector<std::unique_ptr<ChannelStrip>> strips_;

    juce::Viewport nodeViewport_;
    mb::NodeCanvas nodeCanvas_;              // 节点连接视图（与调音台同一张 RouteGraph）

    DrawerButton addBtn_, newBtn_, savePresetBtn_, clearBtn_,
                 exportBtn_, importBtn_, settingsBtn_, toggleBtn_, viewBtn_,
                 themeBtn_, recBtn_, foldBtn_;
    std::unique_ptr<DeviceSetupOverlay> setup_;
    std::unique_ptr<WelcomeOverlay> welcome_;      // 首次开箱：语言选择
    std::unique_ptr<ChannelLinkOverlay> linkOverlay_;   // 通道链接设置弹窗
    std::unique_ptr<SourcePickerOverlay> sourcePicker_; // 输入源 / 输出目标选择浮层
    std::unique_ptr<PluginPickerOverlay> pluginPicker_; // 添加插件选择浮层
    bool linkSyncing_ = false;                     // 联动写回重入保护
    juce::Component* linkKeyHooked_ = nullptr;     // 已挂键盘钩子的顶层窗口
    bool firstLaunch_ = false;
    std::unique_ptr<FxPanel> fxPanel_;
    std::unique_ptr<MediaPlayerPanel> mediaPanel_;   // 「音频播放」面板
    int mediaChannel_ = -1;
    std::unique_ptr<DiagnosticsPanel> diagPanel_;
    std::unique_ptr<SettingsPanel> settingsPanel_;
    std::unique_ptr<ColourPickerPanel> colourPicker_;
    int pendingSlot_ = -1;                  // 「新建通道」待复用的空槽（-1 = 追加）
    int pendingCount_ = 1;                  // 「新建通道」本次要加的条数（1–10，菜单里选）
    std::unique_ptr<TrayIcon> tray_;
    mb::GlobalHotkey hotkeyEngine_;          // 系统级全局快捷键引擎（隐藏窗口 + 通道联动）
    juce::String hotkeyDigest_;              // 已注册绑定的指纹，变化时自动重建
    juce::Image appIcon_;                    // 应用图标（托盘 / 品牌区共用）
    std::unique_ptr<juce::FileChooser> presetChooser_;
    std::unique_ptr<juce::FileChooser> dirChooser_;     // 诊断面板共用的目录选择（备用）

    RecorderBar recorderBar_;
    float lastRecLevel_ = -1.0f;                 // 走带条重绘去重快照
    int lastRecActive_ = -1, lastRecArmed_ = -1;
    juce::String lastRecElapsed_;
    bool lastRecPaused_ = false;
    std::uint32_t recStartMs_ = 0;               // 录音计时起点（暂停时长从中扣除）
    std::uint32_t recPausedTotalMs_ = 0;         // 累计暂停时长
    std::uint32_t recPausedAtMs_ = 0;            // 本次暂停开始时刻（0 = 未在暂停）

    juce::String message_;
    std::uint32_t messageExpiry_ = 0;
    juce::String lastWindowTitle_;          // 原生标题去重缓存

    mb::AppSettingsData settings_;
    int schedTick_ = 0, saveTick_ = 0, stateTick_ = 0;
    juce::String lastPluginDigest_;
    bool dirty_ = false, presetDirty_ = false, quitPromptOpen_ = false;
    juce::var savedSnapshot_;   // 「不保存」退出时的会话回滚基准（上次显式保存点）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
