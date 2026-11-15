#pragma once

// 「音频播放」自带插件面板（浮层）—— 按 S-Rack 原版播放器 1:1 复刻
//
// · 深色调色板跟随主页面 mb::ui（中性深灰 + 主题蓝 accent），浅色仍用原淡蓝白；
//   频谱铺底（LED 段 + 倒影 + 上升气泡），
//   KTV 逐字歌词浮层压在频谱倒影区，左栏播放列表，底部走带。
// · S-Rack 延伸式弹窗体系：播放模式 / 歌词偏移 / 音量 三个向上弹出的面板，
//   与触发按钮「基座」无缝相接（attached 胶囊 + accent 描边 + 接缝亮线），
//   点击弹窗外区域统一收起（DismissListener）。
// · 音量/歌词偏移均为参考线式垂直调节器（拖把手，双击音量归位 100%）；
//   进度条为 S-Rack 细轨 + 白点外观，拖动结束定位并留 0.5s EOF 余量。
// · 播放列表：右键菜单（删除/清空）、空列表提示、当前曲高亮。
// · 功能：播放列表（双击播放 / 添加）、四种播放模式、L/R 输出端口选择
//   （MicroBridge 路由专属）；全部随会话保存恢复。
// · AI 音频分离（S-Rack 分离面板/输出源/状态条）按需求不移植。

#include <JuceHeader.h>

#include <cmath>
#include <vector>

#include "DesktopLyrics.h"
#include "I18n.h"
#include "Lyrics.h"
#include "MediaPlayer.h"
#include "PanelChrome.h"
#include "RouteGraph.h"
#include "Theme.h"

//==============================================================================
// S-Rack 配色（主题化）：随 mb::ui::isDarkTheme() 同步的运行时变量。
// 各 paint 入口调用 sr::syncTheme() 后再取色；切换主题后 applyTheme() 重绘即换肤。
//==============================================================================
namespace sr
{
    inline juce::Colour bg1, bg2;                    // 面板渐变亮 / 暗端
    inline juce::Colour titleBg;                     // 标题条
    inline juce::Colour line;                        // 分隔线 / 边框
    inline juce::Colour accent;                      // 主色
    inline juce::Colour pill, pillHover, pillOn;     // 胶囊：常态 / 悬停 / 开关态
    inline juce::Colour pillBase, pillOff;           // 弹窗基座 / 禁用态
    inline juce::Colour keyBorder, offText;          // 键帽描边 / 禁用文字
    inline juce::Colour listBg, empty;               // 播放列表底板 / 空列表提示
    inline juce::Colour text, dim, faint;            // 文字三级
    inline juce::Colour vizBg1, vizBg2, vizBgMid;    // 频谱渐变底
    inline juce::Colour vizReflect;                  // 频谱倒影罩色
    inline juce::Colour lyricBacking, lyricText, lyricTextDim;  // 歌词衬底 / 未唱文字
    inline juce::Colour green, yellow, red;          // LED 红→黄→绿（双主题共用）
    inline juce::Colour early, late;                 // 提前（绿）/ 延后（黄）

    inline void applyPalette (bool dark)
    {
        if (dark)
        {
            // 深色 = 主页面同一套 Studio One 风格调色板（Theme.h 的 mb::ui 运行时变量），
            // 主题切换 / 重建后仍自动跟随；不再使用 S-Rack 的深海军蓝。
            bg1 = mb::ui::card;                 bg2 = mb::ui::background;
            titleBg = mb::ui::topBar;           line = mb::ui::border;
            accent = mb::ui::accent[0];
            pill = mb::ui::slotBg;
            pillHover = mb::ui::slotBg.brighter (0.12f);
            pillOn = mb::ui::slotBg.interpolatedWith (mb::ui::accent[0], 0.30f);
            pillBase = mb::ui::slotBg.interpolatedWith (mb::ui::accent[0], 0.45f);
            pillOff = mb::ui::sectionBg;
            keyBorder = mb::ui::border;         offText = mb::ui::faintText;
            listBg = mb::ui::sectionBg;         empty = mb::ui::faintText;
            text = mb::ui::text;                dim = mb::ui::subText;
            faint = mb::ui::faintText;
            vizBg1 = mb::ui::sectionBg;         vizBg2 = mb::ui::background;
            vizBgMid = mb::ui::card;            vizReflect = mb::ui::background;
            lyricBacking = mb::ui::background.withAlpha (0.85f);
            lyricText = mb::ui::text;
            lyricTextDim = mb::ui::subText.withAlpha (0.85f);
        }
        else
        {
            bg1 = juce::Colour (0xfff2f7ff); bg2 = juce::Colour (0xffdce7fc);
            titleBg = juce::Colour (0xffe2ecfd); line = juce::Colour (0xff9fb3dd);
            accent = juce::Colour (0xff1e88d0);
            pill = juce::Colour (0xffe4eefe); pillHover = juce::Colour (0xffd6e3fc);
            pillOn = juce::Colour (0xffd2e1fb); pillBase = juce::Colour (0xffcadcf9);
            pillOff = juce::Colour (0xffc7d4ef);
            keyBorder = juce::Colour (0xff9fb3dd); offText = juce::Colour (0xff9aa8c8);
            listBg = juce::Colour (0xffe8f0fe); empty = juce::Colour (0xff8292b8);
            text = juce::Colour (0xff20304f); dim = juce::Colour (0xff4c5e86);
            faint = juce::Colour (0xff8292b8);
            vizBg1 = juce::Colour (0xffdcebf9); vizBg2 = juce::Colour (0xffc9dbf8);
            vizBgMid = juce::Colour (0xffd4e4fb); vizReflect = juce::Colour (0xffc6d8f6);
            lyricBacking = juce::Colour (0x90ffffff);
            lyricText = juce::Colour (0xff20304f); lyricTextDim = juce::Colour (0xcc20304f);
        }
        green  = juce::Colour (0xff33d433); yellow = juce::Colour (0xffeed500);
        red    = juce::Colour (0xffe4390e);
        early  = juce::Colour (0xff8bc34a); late   = juce::Colour (0xffffd54f);
        if (dark)   // 深色：与主页面电平表 / 状态色一致；浅色保持原样
        {
            green = mb::ui::meterGreen; yellow = mb::ui::meterYellow; red = mb::ui::meterRed;
            early = mb::ui::meterGreen; late  = mb::ui::meterYellow;
        }
    }

    /** paint 入口调用：把调色板对齐到当前全局主题（幂等，开销可忽略）。 */
    inline void syncTheme() { applyPalette (mb::ui::isDarkTheme()); }
}

//==============================================================================
// 胶囊按钮（S-Rack 侧边栏样式：悬停/按下/开关态 + 弹窗基座 attached 态）
//==============================================================================
class SrPillButton : public juce::Button
{
public:
    SrPillButton (const juce::String& t, bool toggling = false) : juce::Button (t)
    {
        setClickingTogglesState (toggling);
    }

    // 上方弹窗打开时，把按钮渲染成「基座」：上方直角 + accent 高亮，与弹窗无缝相接
    void setAttached (bool a)
    {
        if (attached != a) { attached = a; repaint(); }
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        sr::syncTheme();
        const auto b = getLocalBounds().toFloat();
        const bool enabled = isEnabled();
        const bool on = enabled && (getToggleState() || attached);

        juce::Colour bg = ! enabled ? sr::pillOff
                          : attached ? sr::pillBase
                          : (on ? sr::pillOn
                                : (over ? sr::pillHover : sr::pill));
        if (down && enabled) bg = bg.brighter (0.12f);

        if (attached)
        {
            const float r = 6.0f;
            juce::Path shape;
            shape.startNewSubPath (b.getX(), b.getY());
            shape.lineTo (b.getRight(), b.getY());
            shape.lineTo (b.getRight(), b.getBottom() - r);
            shape.quadraticTo (b.getRight(), b.getBottom(), b.getRight() - r, b.getBottom());
            shape.lineTo (b.getX() + r, b.getBottom());
            shape.quadraticTo (b.getX(), b.getBottom(), b.getX(), b.getBottom() - r);
            shape.closeSubPath();
            g.setColour (bg);
            g.fillPath (shape);

            // 只描左 / 右 / 下三边，上边留给弹窗继续
            juce::Path edge;
            edge.startNewSubPath (b.getX() + 0.5f, b.getY());
            edge.lineTo (b.getX() + 0.5f, b.getBottom() - r);
            edge.quadraticTo (b.getX() + 0.5f, b.getBottom() - 0.5f,
                              b.getX() + r, b.getBottom() - 0.5f);
            edge.lineTo (b.getRight() - r, b.getBottom() - 0.5f);
            edge.quadraticTo (b.getRight() - 0.5f, b.getBottom() - 0.5f,
                              b.getRight() - 0.5f, b.getBottom() - r);
            edge.lineTo (b.getRight() - 0.5f, b.getY());
            g.setColour (juce::Colour (sr::accent).withAlpha (0.90f));
            g.strokePath (edge, juce::PathStrokeType (1.2f));
        }
        else
        {
            const auto rb = b.reduced (0.5f);
            // 物理键帽：投影（按下下沉）+ 立体渐变帽体 + 顶缘高光 + 边框
            g.setColour (juce::Colours::black.withAlpha (down ? 0.22f : 0.38f));
            g.fillRoundedRectangle (rb.translated (0.0f, down ? 1.0f : 2.0f), 6.0f);
            g.setGradientFill (juce::ColourGradient (bg.brighter (0.14f), 0.0f, rb.getY(),
                                                     bg.darker (0.30f), 0.0f, rb.getBottom(), false));
            g.fillRoundedRectangle (rb, 6.0f);
            g.setColour (on ? sr::accent : sr::keyBorder);
            g.drawRoundedRectangle (rb.reduced (0.5f), 6.0f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (down ? 0.05f : 0.14f));
            g.drawLine (rb.getX() + 6.0f, rb.getY() + 1.0f, rb.getRight() - 6.0f, rb.getY() + 1.0f);
        }

        // 字形凹刻：暗影 + 本体
        g.setFont (mb::ui::font (13.0f));
        if (enabled && ! down)
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawText (getButtonText(), b.translated (0.0f, 0.8f), juce::Justification::centred);
        }
        g.setColour (! enabled ? sr::offText
                               : (on ? juce::Colours::white : sr::text));
        g.drawText (getButtonText(), b, juce::Justification::centred);
    }

private:
    bool attached = false;
};

//==============================================================================
// 走带图标按钮：与底部右侧 SrPillButton 同源的键帽
// 同一组底色 / 描边 / 顶缘高光 / 凹刻字形，只把「文字」换成「图标」。
//==============================================================================
class SrIconButton : public juce::Button
{
public:
    enum Icon { Play, Pause, Prev, Next };
    SrIconButton (Icon ic, bool mainButton = false) : juce::Button ("icon"), icon (ic), main (mainButton) {}

    void setIcon (Icon ic) { if (icon != ic) { icon = ic; repaint(); } }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        sr::syncTheme();
        const auto b  = getLocalBounds().toFloat();
        const auto rb = b.reduced (0.5f);
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float d  = juce::jmin (b.getWidth(), b.getHeight());
        const float s  = d * 0.30f;
        const bool enabled = isEnabled();

        // 键帽配色 / 立体细节与 SrPillButton::paintButton 完全一致：
        // pill / pillHover / pillOn / pillOff、纵向渐变、投影、暗边框、顶缘高光。
        // 主按钮沿用右侧按钮的 on 态（pillOn + accent 描边）以保留主次区分。
        juce::Colour bg = ! enabled ? sr::pillOff
                          : (main ? sr::pillOn
                                  : (over ? sr::pillHover : sr::pill));
        if (down && enabled) bg = bg.brighter (0.12f);

        g.setColour (juce::Colours::black.withAlpha (down ? 0.22f : 0.38f));
        g.fillRoundedRectangle (rb.translated (0.0f, down ? 1.0f : 2.0f), 6.0f);
        g.setGradientFill (juce::ColourGradient (bg.brighter (0.14f), 0.0f, rb.getY(),
                                                 bg.darker (0.30f), 0.0f, rb.getBottom(), false));
        g.fillRoundedRectangle (rb, 6.0f);
        g.setColour (main ? sr::accent.withAlpha (0.90f) : sr::keyBorder);
        g.drawRoundedRectangle (rb.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (down ? 0.05f : 0.14f));
        g.drawLine (rb.getX() + 6.0f, rb.getY() + 1.0f, rb.getRight() - 6.0f, rb.getY() + 1.0f);

        juce::Path p;
        switch (icon)
        {
            case Play:
                p.addTriangle (cx - s * 0.7f, cy - s, cx - s * 0.7f, cy + s, cx + s, cy);
                break;
            case Pause:
                p.addRoundedRectangle (cx - s * 0.75f, cy - s, s * 0.5f, s * 2.0f, 1.5f);
                p.addRoundedRectangle (cx + s * 0.25f, cy - s, s * 0.5f, s * 2.0f, 1.5f);
                break;
            case Prev:
                p.addRoundedRectangle (cx - s * 0.8f, cy - s, s * 0.32f, s * 2.0f, 1.0f);
                p.addTriangle (cx + s * 0.8f, cy - s, cx + s * 0.8f, cy + s, cx - s * 0.35f, cy);
                break;
            case Next:
                p.addTriangle (cx - s * 0.8f, cy - s, cx - s * 0.8f, cy + s, cx + s * 0.35f, cy);
                p.addRoundedRectangle (cx + s * 0.48f, cy - s, s * 0.32f, s * 2.0f, 1.0f);
                break;
        }

        // 图标取右侧按钮同一套字形色：常态 sr::text，主按钮 / 悬停 / 按下白色，
        // 禁用暗蓝；并复刻文字的凹刻暗影，避免图标「浮」在键帽上。
        if (enabled && ! down)
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillPath (p, juce::AffineTransform::translation (0.0f, 0.8f));
        }
        g.setColour (! enabled ? sr::offText
                               : ((main || over || down) ? juce::Colours::white
                                                         : sr::text));
        g.fillPath (p);
    }

private:
    Icon icon;
    bool main;
};

//==============================================================================
// 向上弹窗通用外观：与触发按钮同宽、底边紧贴按钮上沿的「延伸式」面板。
// 上方圆角、下方直角，底边不描边；底部有向按钮汇聚的 accent 高光，
// 与下方 attached 状态的按钮拼在一起，视觉上像从按钮本身向上长出来。
//==============================================================================
inline void drawVerticalPopupPanel (juce::Graphics& g, juce::Component& c, const juce::String& title)
{
    const auto b = c.getLocalBounds().toFloat();
    const float w = b.getWidth();
    const float r = juce::jmin (10.0f, w * 0.20f);

    juce::Path panel;
    panel.startNewSubPath (b.getX(), b.getBottom());
    panel.lineTo (b.getX(), b.getY() + r);
    panel.quadraticTo (b.getX(), b.getY(), b.getX() + r, b.getY());
    panel.lineTo (b.getRight() - r, b.getY());
    panel.quadraticTo (b.getRight(), b.getY(), b.getRight(), b.getY() + r);
    panel.lineTo (b.getRight(), b.getBottom());
    panel.closeSubPath();
    g.setColour (mb::ui::sectionBg);
    g.fillPath (panel);

    // 底部只留极淡的过渡，不做横向亮带，避免把面板与按钮「切」开
    g.setColour (mb::ui::accent[0].withAlpha (0.07f));
    g.fillRect (b.getX() + 1.5f, b.getBottom() - 8.0f, w - 3.0f, 7.0f);

    // 描边只走上 / 左 / 右三边，底边留空，与下方按钮连成一体
    juce::Path edge;
    edge.startNewSubPath (b.getX() + 0.5f, b.getBottom());
    edge.lineTo (b.getX() + 0.5f, b.getY() + r);
    edge.quadraticTo (b.getX() + 0.5f, b.getY() + 0.5f, b.getX() + r, b.getY() + 0.5f);
    edge.lineTo (b.getRight() - r, b.getY() + 0.5f);
    edge.quadraticTo (b.getRight() - 0.5f, b.getY() + 0.5f, b.getRight() - 0.5f, b.getY() + r);
    edge.lineTo (b.getRight() - 0.5f, b.getBottom());
    g.setColour (mb::ui::accent[0].withAlpha (0.65f));
    g.strokePath (edge, juce::PathStrokeType (1.0f));

    // 接缝亮线：与按钮上沿对齐，暗示两者是同一块
    g.setColour (mb::ui::accent[0].withAlpha (0.30f));
    g.fillRect (b.getX() + 2.0f, b.getBottom() - 1.0f, w - 4.0f, 1.0f);

    // 标题：窄面板自动缩小字号；传空标题时整块跳过（音量弹窗无标题）
    if (title.isNotEmpty())
    {
        g.setColour (mb::ui::text);
        g.setFont (mb::ui::font (w < 60.0f ? 10.0f : 11.5f, true));
        g.drawText (title, juce::Rectangle<float> (0, 5.0f, w, 17.0f), juce::Justification::centred);
    }
}

//==============================================================================
// 播放器滑条外观（S-Rack：细轨 + 白色圆点）
//==============================================================================
class SrSliderLook : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style, juce::Slider&) override
    {
        const auto trackColour = mb::ui::track;
        const auto fillColour  = mb::ui::accent[0];

        if (style == juce::Slider::LinearVertical)
        {
            const float cx = (float) x + (float) width * 0.5f;
            const float trackW = 6.0f;
            const auto rail = juce::Rectangle<float> (cx - trackW * 0.5f, (float) y, trackW, (float) height);
            // 凹槽导轨：投影 + 槽体 + 内缘高光
            g.setColour (juce::Colours::black.withAlpha (0.40f));
            g.fillRoundedRectangle (rail.translated (0.0f, 1.0f), 3.0f);
            g.setColour (trackColour);
            g.fillRoundedRectangle (rail, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawLine (rail.getX() + 1.0f, rail.getY() + 2.0f,
                        rail.getX() + 1.0f, rail.getBottom() - 2.0f);
            const float fillH = (float) height - sliderPos;
            g.setGradientFill (juce::ColourGradient (fillColour.darker (0.25f), cx, (float) y + sliderPos,
                                                     fillColour.brighter (0.10f), cx, (float) y + height, false));
            g.fillRoundedRectangle (cx - trackW * 0.5f, (float) y + sliderPos, trackW, fillH, 3.0f);
            // 物理旋钮：投影 + 渐变球面 + 边框 + 高光点
            const auto knob = juce::Rectangle<float> (cx - 7.0f, (float) y + sliderPos - 7.0f, 14.0f, 14.0f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillEllipse (knob.translated (0.0f, 1.5f));
            g.setGradientFill (juce::ColourGradient (juce::Colours::white, knob.getCentreX(), knob.getY() + 2.0f,
                                                     juce::Colour (0xffc2ced6), knob.getCentreX(), knob.getBottom(), false));
            g.fillEllipse (knob);
            g.setColour (mb::ui::border);
            g.drawEllipse (knob.reduced (0.5f), 1.0f);
            g.setColour (juce::Colours::white);
            g.fillEllipse (knob.getCentreX() - 2.0f, knob.getY() + 2.5f, 4.0f, 3.0f);
        }
        else // LinearHorizontal（进度条）
        {
            const float cy = (float) y + (float) height * 0.5f;
            const float trackH = 4.0f;
            const auto rail = juce::Rectangle<float> ((float) x, cy - trackH * 0.5f, (float) width, trackH);
            g.setColour (juce::Colours::black.withAlpha (0.40f));
            g.fillRoundedRectangle (rail.translated (0.0f, 1.0f), 2.0f);
            g.setColour (trackColour);
            g.fillRoundedRectangle (rail, 2.0f);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawLine (rail.getX() + 2.0f, rail.getY() + 1.0f,
                        rail.getRight() - 2.0f, rail.getY() + 1.0f);
            const float fillW = sliderPos - (float) x;
            g.setGradientFill (juce::ColourGradient (fillColour.brighter (0.10f), (float) x, cy,
                                                     fillColour.darker (0.20f), (float) x + fillW, cy, false));
            g.fillRoundedRectangle ((float) x, cy - trackH * 0.5f, fillW, trackH, 2.0f);
            const auto knob = juce::Rectangle<float> (sliderPos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillEllipse (knob.translated (0.0f, 1.5f));
            g.setGradientFill (juce::ColourGradient (juce::Colours::white, knob.getCentreX(), knob.getY() + 2.0f,
                                                     juce::Colour (0xffc2ced6), knob.getCentreX(), knob.getBottom(), false));
            g.fillEllipse (knob);
            g.setColour (mb::ui::border);
            g.drawEllipse (knob.reduced (0.5f), 1.0f);
            g.setColour (juce::Colours::white);
            g.fillEllipse (knob.getCentreX() - 2.0f, knob.getY() + 2.0f, 4.0f, 3.0f);
        }
    }
};

//==============================================================================
class MediaPlayerPanel : public FloatingCardPanel,
                         private juce::Timer
{
public:
    std::function<void()> onChanged;      // 任何改动（会话自动保存用）
    /** 桌面歌词窗口右上角置顶开关 → MainComponent 写盘并同步设置页复选框。 */
    std::function<void (bool)> onDesktopLyricsPinChanged;
    /** 桌面歌词窗口锁定 / 解锁 / 关闭 → MainComponent 写盘并刷新托盘切换入口。 */
    std::function<void (bool)> onDesktopLyricsLockChanged;
    /** 桌面歌词窗口拖动 / 缩放 → MainComponent 同步内存里的位置副本。 */
    std::function<void (juce::Rectangle<int>)> onDesktopLyricsBoundsChanged;

    /** 设置页改动置顶/锁定选项后由 MainComponent 调用：窗口状态从磁盘重新同步。 */
    void applyTopMost()
    {
        if (desktopLyrics_ != nullptr) desktopLyrics_->syncSettings();
    }

    /** 全局解锁热键 / 托盘菜单：直接翻转窗口锁定态。 */
    void setDesktopLyricsLocked (bool locked)
    {
        if (desktopLyrics_ != nullptr) desktopLyrics_->setLocked (locked, false);
    }

    DesktopLyricsWindow* desktopLyricsWindow() const noexcept
    {
        return desktopLyrics_.get();
    }

    MediaPlayerPanel (mb::AudioEngine& engine, mb::RouteGraph& graph, int ch)
        : engine_ (engine), bank_ (engine.mediaPlayers()), ch_ (ch)
    {
        sr::syncTheme();          // 构造期取色前先把调色板对齐当前主题
        player_ = bank_.playerPtr (ch_);
        if (player_ == nullptr) player_ = &bank_.ensurePlayer (ch_);

        // 图标长度运行时查询：换图标后即使本翻译单元未重编也不会越界读资源。
        int iconSize = 0;
        const char* iconData = BinaryData::getNamedResource ("app_icon_png", iconSize);
        logo_ = iconData != nullptr && iconSize > 0
              ? juce::ImageCache::getFromMemory (iconData, iconSize)
              : juce::Image();

        // ---- 播放列表（S-Rack：透明列表 + 圆角底板 + 空列表提示）-----------
        listModel_ = std::make_unique<PlaylistModel> (*this);
        playlist_.setModel (listModel_.get());
        playlist_.setRowHeight (26);
        playlist_.setClickingTogglesRowSelection (false);
        playlist_.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        playlist_.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        playlist_.setOpaque (false);
        addAndMakeVisible (playlist_);

        auto wire = [this] (SrPillButton& b, const char* zh, const char* en, std::function<void()> fn)
        {
            b.setButtonText (mb::tr (zh, en));
            if (zh != nullptr && zh[0] != '\0')       // modeBtn_ 文案由 updateModeText() 负责
            {
                auto* bp = &b;
                textRefreshers_.push_back ([bp, zh, en] { bp->setButtonText (mb::tr (zh, en)); });
            }
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };
        wire (addBtn_, "添加音频", "Add audio", [this] { browseFiles(); });

        // 旁通开关（与通道条插件圆点同步：音频播放内置插件 id = 9）
        addAndMakeVisible (bypassBtn_);
        bypassBtn_.setButtonText (mb::tr ("旁通", "Bypass"));
        bypassBtn_.setToggleState (currentBypassState(), juce::dontSendNotification);
        bypassBtn_.onStateChange = [this]
        {
            setBypassState (bypassBtn_.getToggleState());
            if (onChanged) onChanged();
        };

        wire (modeBtn_, "", "", {});          // 文案由 updateModeText() 按当前模式写入
        modeBtn_.onClick = [this] { togglePopup (modePopup_); };

        // ---- 走带 ---------------------------------------------------------
        addAndMakeVisible (prevBtn_);
        prevBtn_.onClick = [this] { if (player_ != nullptr) player_->advanceTrack (false); };
        addAndMakeVisible (playBtn_);
        playBtn_.onClick = [this]
        {
            if (player_ == nullptr) return;
            player_->toggle();
            if (onChanged) onChanged();
        };
        addAndMakeVisible (nextBtn_);
        nextBtn_.onClick = [this] { if (player_ != nullptr) player_->advanceTrack (true); };

        // ---- 底部右侧按钮组（S-Rack：频谱 / 歌词 / 偏移 / 音量 + 端口）----
        wire (offsetBtn_, "偏移", "Offset", [this] { togglePopup (offsetPopup_); });
        wire (volumeBtn_, "音量", "Volume", [this] { togglePopup (volumePopup_); });
        addAndMakeVisible (spectrumBtn_);
        spectrumBtn_.setButtonText (mb::tr ("频谱", "Spectrum"));
        spectrumBtn_.setToggleState (true, juce::dontSendNotification);
        spectrumBtn_.onStateChange = [this]
        {
            visualizer_.setVisible (spectrumBtn_.getToggleState());
            spectrumBtn_.repaint();
        };
        // 桌面歌词：在「歌词」左侧，开关独立透明窗口（拖动定位 / 拖右下角缩放）
        wire (desktopLyricsBtn_, "桌面歌词", "Desktop lyrics", [this]
        {
            if (desktopLyricsBtn_.getToggleState())
            {
                if (desktopLyrics_ == nullptr)
                {
                    desktopLyrics_ = std::make_unique<DesktopLyricsWindow>();
                    auto* w = desktopLyrics_.get();
                    w->onPinChanged = [this] (bool on)
                    {
                        if (onDesktopLyricsPinChanged) onDesktopLyricsPinChanged (on);
                    };
                    w->onLockChanged = [this] (bool on)
                    {
                        if (onDesktopLyricsLockChanged) onDesktopLyricsLockChanged (on);
                    };
                    w->onBoundsChanged = [this] (juce::Rectangle<int> b)
                    {
                        if (onDesktopLyricsBoundsChanged) onDesktopLyricsBoundsChanged (b);
                    };
                    w->onCloseClicked = [this]
                    {
                        // 窗口的 mouseDown 里不能直接自毁：延到下一轮消息循环，
                        // 再把播放器里的「桌面歌词」开关置回关闭态
                        juce::Component::SafePointer<MediaPlayerPanel> safe (this);
                        juce::MessageManager::callAsync ([safe]
                        {
                            auto* p = safe.getComponent();
                            if (p == nullptr) return;
                            p->desktopLyricsBtn_.setToggleState (false,
                                                                 juce::dontSendNotification);
                            p->desktopLyrics_.reset();          // 窗口已关，锁定态一并解除
                            if (p->onDesktopLyricsLockChanged)
                                p->onDesktopLyricsLockChanged (false);
                            p->desktopLyricsBtn_.repaint();
                        });
                    };
                    w->syncSettings();               // 按当前设置对齐置顶/锁定态
                }
            }
            else
            {
                desktopLyrics_.reset();              // 窗口已关，锁定态一并解除
                if (onDesktopLyricsLockChanged) onDesktopLyricsLockChanged (false);
            }
            pushDesktopLyrics();                // 打开即刻填上当前行，不等下一个 timer 帧
        });
        addAndMakeVisible (lyricBtn_);
        lyricBtn_.setButtonText (mb::tr ("歌词", "Lyrics"));
        lyricBtn_.setToggleState (true, juce::dontSendNotification);
        lyricBtn_.onStateChange = [this]
        {
            lyrics_.setVisible (lyricBtn_.getToggleState());
            lyricBtn_.repaint();
        };

        // ---- 向上弹窗（延伸式，同一时刻只显示一个）------------------------
        offsetPopup_.owner = this;
        volumePopup_.owner = this;
        modePopup_.owner = this;
        addAndMakeVisible (offsetPopup_);
        addAndMakeVisible (volumePopup_);
        addAndMakeVisible (modePopup_);
        offsetPopup_.setVisible (false);
        volumePopup_.setVisible (false);
        modePopup_.setVisible (false);

        // ---- 进度条（S-Rack 细轨外观；拖动结束定位，留 0.5s EOF 余量）----
        seek_.setRange (0.0, 1.0, 0.001);
        seek_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        seek_.setLookAndFeel (&sliderLook_);
        seek_.onDragStart = [this] { seeking_ = true; };
        seek_.onDragEnd = [this]
        {
            seeking_ = false;
            const double len = player_ != nullptr ? player_->trackLengthSec.load() : 0.0;
            if (len > 0.0)
            {
                // 留 0.5s 余量，避免拖到最右立刻触发切歌
                const double target = juce::jmin (seek_.getValue() * len, len - 0.5);
                player_->seekTo (juce::jmax (0.0, target));
            }
        };
        addAndMakeVisible (seek_);

        timeLabel_.setFont (mb::ui::font (12.5f));
        timeLabel_.setColour (juce::Label::textColourId, juce::Colour (sr::text));
        timeLabel_.setText ("00:00 / 00:00", juce::dontSendNotification);
        addAndMakeVisible (timeLabel_);

        // ---- 频谱 / 歌词浮层 ----------------------------------------------
        addAndMakeVisible (visualizer_);
        visualizer_.addAndMakeVisible (lyrics_);
        lyrics_.owner = this;                     // 浮层 paint 依赖 owner（否则空指针闪退）
        lyrics_.setInterceptsMouseClicks (false, false);

        // ---- 窗口按钮（最小化/最大化/关闭）由基类 FloatingCardPanel 托管 ----

        addAndMakeVisible (pathLabel_);
        pathLabel_.setFont (mb::ui::font (9.0f));
        pathLabel_.setColour (juce::Label::textColourId, juce::Colour (sr::faint));
        pathLabel_.setJustificationType (juce::Justification::centredLeft);

        // 点击弹窗/触发按钮以外的区域时，统一收起向上弹窗
        addMouseListener (&dismissListener_, true);

        updateModeText();
        reloadLyrics();
        refreshPlaylist();

        startTimerHz (30);
    }

    ~MediaPlayerPanel() override
    {
        stopTimer();
        seek_.setLookAndFeel (nullptr);
    }

    /** 主题切换：同步 sr 调色板 + 重绘即换肤（弹窗 / 推子 / 进度条在 paint 时读色）。 */
    void applyTheme()
    {
        sr::syncTheme();
        timeLabel_.setColour (juce::Label::textColourId, sr::text);
        pathLabel_.setColour (juce::Label::textColourId, sr::faint);
        repaint();
        modePopup_.repaint();
        offsetPopup_.repaint();
        volumePopup_.repaint();
        pushDesktopLyrics();            // 主题色变了，桌面歌词文字/描边立即跟上
    }

    /** 语言切换：刷新构造期缓存的按钮文案；弹窗标题在 paint 时取词，重绘即可。 */
    void applyLanguage()
    {
        for (auto& f : textRefreshers_) f();
        bypassBtn_.setButtonText   (mb::tr ("旁通", "Bypass"));
        spectrumBtn_.setButtonText (mb::tr ("频谱", "Spectrum"));
        lyricBtn_.setButtonText    (mb::tr ("歌词", "Lyrics"));
        updateModeText();
        offsetPopup_.repaint();
        volumePopup_.repaint();
        modePopup_.repaint();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        sr::syncTheme();
        const auto card = cardBounds();
        const bool collapsed = minimised();

        // 面板底：S-Rack 渐变（深色海军蓝 / 浅色淡蓝白）
        juce::ColourGradient bg (sr::bg1, card.getX(), card.getY(),
                                 sr::bg2, card.getX() + card.getWidth() * 0.35f,
                                 card.getBottom(), false);
        g.setGradientFill (bg);
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (sr::line);
        g.drawRoundedRectangle (card.reduced (0.5f), 8.0f, 1.2f);

        // 标题条（折叠态 = 播放控制条底色，不再画 accent / 图标 / 标题文字）
        g.setColour (sr::titleBg);
        g.fillRoundedRectangle (card.withHeight (34.0f), 8.0f);
        g.fillRect (card.getX(), card.getY() + 20.0f, card.getWidth(), 14.0f);
        g.setColour (sr::line.withAlpha (0.7f));
        g.fillRect (card.getX() + 1.0f, card.getY() + 33.0f, card.getWidth() - 2.0f, 1.0f);

        if (collapsed) return;

        // accent 竖条 + 标题
        g.setColour (sr::accent);
        g.fillRoundedRectangle (card.getX() + 14.0f, card.getY() + 9.0f, 4.0f, 16.0f, 2.0f);

        const bool hasLogo = logo_.isValid();
        if (hasLogo)
            g.drawImage (logo_, juce::Rectangle<float> (card.getX() + 26, card.getY() + 6, 22, 22));

        g.setFont (mb::ui::font (14.0f, true));
        g.setColour (mb::ui::isDarkTheme() ? mb::ui::text : juce::Colours::white);
        g.drawText (mb::channelDisplayName (ch_) + mb::S("  ·  ")
                        + mb::tr ("播放器", "Player"),
                    juce::Rectangle<float> (card.getX() + (hasLogo ? 56.0f : 26.0f),
                                            card.getY(), 260.0f, 34.0f),
                    juce::Justification::centredLeft, false);

        g.setFont (mb::ui::font (9.0f));
        g.setColour (juce::Colour (sr::faint));
        g.drawText (mb::tr ("输出走本通道（推子/路由可控）· 旁通圆点 = 静音走带 · 双击列表曲目即播",
                            "Output follows this channel · bypass dot = muted timeline · double-click to play"),
                    juce::Rectangle<float> (card.getX() + 300.0f, card.getY(),
                                            card.getWidth() - 420.0f, 34.0f),
                    juce::Justification::centredLeft, false);

        // 旁通状态徽标：旁通圆点 = 静音走带（进度照走、无声、频谱停摆）
        if (player_ != nullptr && player_->muted.load())
        {
            const juce::String bt = mb::tr ("已旁通 · 静音走带中（点通道条上的圆点恢复）",
                                            "Bypassed · muted (click the dot on the strip to restore)");
            g.setFont (mb::ui::font (12.0f, true));
            const int tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), bt) + 28;
            auto r = juce::Rectangle<int> (card.getX() + (card.getWidth() - tw) / 2,
                                           card.getY() + 44, tw, 24);
            g.setColour (juce::Colour (0xe6b8860b));
            g.fillRoundedRectangle (r.toFloat(), 12.0f);
            g.setColour (juce::Colours::white);
            g.drawText (bt, r, juce::Justification::centred);
        }
    }

    void resized() override
    {
        const auto cardF = cardBounds();
        const auto card = cardF.toNearestInt();
        const int W = card.getWidth(), H = card.getHeight();
        const int bottom = card.getY() + H;

        layoutChrome (cardF, minimised());
        if (minimised())
        {
            // 折叠态：只保留播放控制三键（上一首 / 播放 / 下一首），
            // 竖直居中在 34px 控制条左缘；窗口按钮（还原/最大化/关闭）仍由基类放在右端
            int x = card.getX() + 6;
            prevBtn_.setBounds (x, card.getY() + 3, 28, 28);   x += 28 + 4;
            playBtn_.setBounds (x, card.getY() + 1, 32, 32);   x += 32 + 4;
            nextBtn_.setBounds (x, card.getY() + 3, 28, 28);
            return;
        }

        // ---- 左栏：顶部 添加音频 + 旁通，播放列表撑满其余高度 ---------------
        const int sx = card.getX() + 14, sw = 232;
        addBtn_.setBounds   (sx, card.getY() + 44, sw - 56, 26);
        bypassBtn_.setBounds (sx + sw - 52, card.getY() + 44, 52, 26);
        playlist_.setBounds (sx, card.getY() + 78, sw, juce::jmax (40, H - 78 - 44));

        // ---- 右区：频谱更高，走带整体贴底（不再悬空）------------------------
        const int rx = sx + sw + 14;
        const int rw = juce::jmax (80, card.getRight() - 14 - rx);

        visualizer_.setBounds (rx, card.getY() + 44, rw, juce::jmax (60, H - 44 - 116));

        // 时间标签在进度条上方（S-Rack 同款 "00:00 / 04:32"）
        timeLabel_.setBounds (rx + 4, bottom - 110, rw - 8, 14);
        seek_.setBounds (rx + 4, bottom - 94, rw - 8, 16);

        auto tr = juce::Rectangle<int> (rx, bottom - 74, rw, 44);
        prevBtn_.setBounds (tr.removeFromLeft (34).withSizeKeepingCentre (34, 34));
        tr.removeFromLeft (16);
        playBtn_.setBounds (tr.removeFromLeft (42).withSizeKeepingCentre (42, 42));
        tr.removeFromLeft (16);
        nextBtn_.setBounds (tr.removeFromLeft (34).withSizeKeepingCentre (34, 34));
        tr.removeFromLeft (16);

        // 左下角按钮组（从左到右）：播放模式 / 频谱 / 桌面歌词 / 歌词 / 偏移 / 音量
        // 窗口偏窄时整组按可用宽度等比收缩，避免新增按钮把后面的键挤出卡片
        constexpr int kGroupNeed = 76 + 48 + 96 + 48 + 48 + 56 + 4 * 6;
        const double k = tr.getWidth() > 0
                       ? juce::jmin (1.0, static_cast<double> (tr.getWidth()) / kGroupNeed)
                       : 1.0;
        auto takeL = [&tr, k] (int w)
        {
            auto r = tr.removeFromLeft (juce::jmax (20, juce::roundToInt (w * k)));
            tr.removeFromLeft (juce::jmax (0, juce::roundToInt (4 * k)));
            return r;
        };
        auto place = [&takeL] (juce::Button& b, int w)
        {
            const auto r = takeL (w);
            b.setBounds (r.withSizeKeepingCentre (r.getWidth(), 28));
        };
        place (modeBtn_,           76);
        place (spectrumBtn_,       48);
        place (desktopLyricsBtn_,  96);
        place (lyricBtn_,          48);
        place (offsetBtn_,         48);
        place (volumeBtn_,         56);

        // 歌词浮层：锁定频谱倒影区（基线 h-96，与 Visualizer::paint 同源）
        const auto vb = visualizer_.getLocalBounds();
        const float baseY = vb.getHeight() > 140 ? vb.getHeight() - 96.0f
                                                 : vb.getHeight() * 0.70f;
        const int reflTop = juce::roundToInt (baseY) + 5;
        lyrics_.setBounds (6, reflTop, juce::jmax (10, vb.getWidth() - 12),
                           juce::jmax (10, vb.getHeight() - reflTop - 6));

        pathLabel_.setBounds (card.getX() + 16, bottom - 24, W - 32, 16);

        // 向上弹窗：宽度与触发按钮完全一致、底边贴住按钮上沿
        const auto placePopup = [] (juce::Component& popup, const juce::Component& btn, int h)
        {
            popup.setBounds (btn.getX(), btn.getY() - h, btn.getWidth(), h);
        };
        placePopup (modePopup_,   modeBtn_,   166);
        placePopup (offsetPopup_, offsetBtn_, 240);
        placePopup (volumePopup_, volumeBtn_, 240);
    }

private:
    /** 正常态卡片尺寸（位置 / 收起 / 最大化由 FloatingCardPanel 处理）。 */
    juce::Point<float> normalSize() const override
    {
        const float W = (float) getWidth(), H = (float) getHeight();
        constexpr float margin = 22.0f;
        const float w = juce::jmin (900.0f, juce::jmax (320.0f, W - margin * 2.0f));
        const float h = juce::jmax (300.0f, H - margin * 2.0f);
        return { w, h };
    }

    /** 缩放下限：左栏 232 + 走带按钮组约 450 + 边距；
        频谱 / 进度 / 走带 / 路径四层至少 320 高。小窗口由基类钳到窗口内。 */
    juce::Point<float> minCardSize() const override { return { 760.0f, 320.0f }; }

    /** 折叠态 = 紧凑控制条：只装 播放三键（左）+ 还原键（右），无最大化/关闭。
        宽度 = 6 + 28+4+32+4+28（三键）+ 12 + 34（还原键）+ 6 ≈ 154，取 160 留余量。
        双击控制条空白处同样可还原。 */
    juce::Point<float> collapsedSize() const override { return { 160.0f, (float) kTitleH }; }

    /** 折叠态右上角只保留最小化键（此时为展开箭头），最大化 / 关闭隐藏。 */
    int collapsedChromeButtons() const override { return 1; }

    //==================================================================
    // 播放列表（S-Rack PlaylistComponent 复刻：当前曲高亮、右键菜单、空提示）
    //==================================================================
    struct PlaylistBox : juce::ListBox
    {
        void paint (juce::Graphics& g) override
        {
            sr::syncTheme();
            g.setColour (sr::listBg);
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 8.0f);
            ListBox::paint (g);
            auto* m = getModel();
            if (m == nullptr || m->getNumRows() == 0)
            {
                g.setColour (sr::empty);
                g.setFont (mb::ui::font (12.5f));
                g.drawText (mb::tr ("播放列表为空", "Playlist is empty"),
                            getLocalBounds(), juce::Justification::centred);
            }
        }
    };

    struct PlaylistModel : juce::ListBoxModel
    {
        explicit PlaylistModel (MediaPlayerPanel& o) : owner (o) {}
        int getNumRows() override
        {
            return owner.player_ != nullptr
                 ? static_cast<int> (owner.player_->files.size()) : 0;
        }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
        {
            sr::syncTheme();
            if (owner.player_ == nullptr
                || row < 0 || row >= static_cast<int> (owner.player_->files.size())) return;
            const bool cur = owner.player_->currentIdx.load() == row;

            if (cur)
            {
                g.setColour (sr::pillOn);
                g.fillRoundedRectangle (2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 5.0f);
            }
            g.setFont (mb::ui::font (12.5f));
            g.setColour (cur ? sr::accent : sr::text);
            const auto t = owner.player_->files[static_cast<size_t> (row)]
                               .getFileNameWithoutExtension();
            g.drawText (juce::String (row + 1) + mb::S(". ") + t,
                        juce::Rectangle<int> (10, 0, w - 16, h),
                        juce::Justification::centredLeft, true);
        }
        void listBoxItemClicked (int row, const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) owner.showRowMenu (row);   // 右键弹出菜单
        }
        void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu()) owner.playIndex (row);
        }
        // 列表空白处右键：只提供「清空列表」
        void backgroundClicked (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) owner.showRowMenu (-1);
        }
        MediaPlayerPanel& owner;
    };

    void playIndex (int row)
    {
        if (player_ == nullptr || row < 0 || row >= static_cast<int> (player_->files.size())) return;
        player_->loadTrack (row, true);
        refreshPlaylist();
        if (onChanged) onChanged();
    }

    void browseFiles()
    {
        chooser_ = std::make_unique<juce::FileChooser> (
            mb::tr ("选择音频文件", "Choose audio files"),
            juce::File::getSpecialLocation (juce::File::userMusicDirectory),
            "*.mp3;*.m4a;*.m4b;*.aac;*.wma;*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp4", true);
        const auto flags = juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectMultipleItems;
        chooser_->launchAsync (flags, [this] (const juce::FileChooser& fc)
        {
            if (player_ == nullptr) return;
            std::vector<juce::File> picked;
            for (const auto& f : fc.getResults())
                if (f.existsAsFile()) picked.push_back (f);
            if (picked.empty()) return;
            const bool wasEmpty = player_->files.empty();
            for (const auto& f : picked)
                player_->files.push_back (f);
            refreshPlaylist();
            if (wasEmpty) playIndex (0);           // 空列表首次添加：自动就位
            if (onChanged) onChanged();
        });
    }

    void removeAt (int row)
    {
        if (player_ == nullptr) return;
        const int n = static_cast<int> (player_->files.size());
        if (row < 0 || row >= n) return;
        const int cur = player_->currentIdx.load();
        if (row == cur)
            player_->stopAndUnload();          // 移除当前曲：停播卸载，列表保留
        player_->files.erase (player_->files.begin() + row);
        if (row < cur)
            player_->currentIdx.store (cur - 1, std::memory_order_relaxed);
        refreshPlaylist();
        reloadLyrics();
        if (onChanged) onChanged();
    }

    void clearAll()
    {
        if (player_ == nullptr) return;
        player_->resetAll();
        refreshPlaylist();
        reloadLyrics();
        if (onChanged) onChanged();
    }

    void showRowMenu (int row)
    {
        juce::PopupMenu m;
        if (row >= 0)
            m.addItem (1, mb::tr ("删除选中", "Delete selected"), true, false);
        m.addItem (2, mb::tr ("清空列表", "Clear list"), true, false);
        m.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (playlist_)
                            .withMousePosition(),
                        [this, row] (int result)
                        {
                            if (result == 1 && row >= 0) removeAt (row);
                            else if (result == 2)        clearAll();
                        });
    }

    void refreshPlaylist()
    {
        playlist_.updateContent();
        playlist_.repaint();
    }

    //==================================================================
    // 弹窗体系（S-Rack：互斥 + 基座外观 + 点击外部收起）
    //==================================================================
    template <typename PopupT>
    void togglePopup (PopupT& popup)
    {
        const bool show = ! popup.isVisible();
        closeAllPopups();
        popup.setVisible (show);
        if (show)
        {
            popup.toFront (false);       // 弹窗置于最上层，保证推子可拖动
            popup.syncFromPlayer();
        }
        syncPopupChrome();
    }

    void closeAllPopups()
    {
        offsetPopup_.setVisible (false);
        volumePopup_.setVisible (false);
        modePopup_.setVisible (false);
        syncPopupChrome();
    }

    // 弹窗打开时，把对应按钮切成「基座」外观（上方直角 + accent 高亮）
    void syncPopupChrome()
    {
        offsetBtn_.setAttached (offsetPopup_.isVisible());
        volumeBtn_.setAttached (volumePopup_.isVisible());
        modeBtn_.setAttached   (modePopup_.isVisible());
    }

    // 点击弹窗或它们的触发按钮以外区域时，统一收起
    void handleGlobalMouseDown (juce::Component* c)
    {
        if (c == nullptr) return;
        const auto inside = [c] (juce::Component& comp)
        {
            return c == &comp || comp.isParentOf (c);
        };
        if (inside (offsetPopup_) || inside (volumePopup_) || inside (modePopup_)
            || inside (offsetBtn_) || inside (volumeBtn_) || inside (modeBtn_))
            return;
        closeAllPopups();
    }

    struct DismissListener : juce::MouseListener
    {
        MediaPlayerPanel& owner;
        explicit DismissListener (MediaPlayerPanel& o) : owner (o) {}
        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.handleGlobalMouseDown (e.originalComponent);
        }
    };

    //==================================================================
    // 动作
    //==================================================================
    void applyVolume (float gain)
    {
        // 通道重排 / 预设切换后构造时缓存的裸指针可能已退役；实时取当前通道
        // 的播放器再写，保证推子作用的就是音频线程正在渲染的那个实例。
        player_ = &bank_.ensurePlayer (ch_);
        player_->volume.store (juce::jlimit (0.0f, 2.0f, gain), std::memory_order_relaxed);
        if (onChanged) onChanged();
    }

    void applyLyricOffset (double sec)
    {
        if (player_ == nullptr) return;
        player_->offsetMs.store (juce::roundToInt (sec * 1000.0), std::memory_order_relaxed);
        reloadLyrics();                       // 重新套偏移
        if (onChanged) onChanged();
    }

    void applyMode (int m)
    {
        if (player_ == nullptr) return;
        player_->mode.store (m, std::memory_order_relaxed);
        updateModeText();
        closeAllPopups();
        if (onChanged) onChanged();
    }

    /** 模式按钮显示当前模式名（S-Rack：点按钮弹出四选一列表）。 */
    void updateModeText()
    {
        const int m = player_ != nullptr ? player_->mode.load()
                                         : mb::MediaPlayerBank::kListLoop;
        switch (m)
        {
            case mb::MediaPlayerBank::kSequential: modeBtn_.setButtonText (mb::tr ("顺序播放", "Sequential")); break;
            case mb::MediaPlayerBank::kSingleLoop: modeBtn_.setButtonText (mb::tr ("单曲循环", "Repeat one")); break;
            case mb::MediaPlayerBank::kRandom:     modeBtn_.setButtonText (mb::tr ("随机播放", "Shuffle")); break;
            default:                               modeBtn_.setButtonText (mb::tr ("列表循环", "List loop")); break;
        }
    }

    //==================================================================
    // 旁通（与通道条插件圆点同源：音频播放内置插件 id = 9）
    //==================================================================
    bool currentBypassState() const
    {
        const int idx = engine_.pluginBank().builtInSlot (ch_, 9);
        if (idx >= 0) return engine_.pluginBank().pluginBypass (ch_, idx);
        return player_ != nullptr && player_->muted.load();
    }

    void setBypassState (bool on)
    {
        const int idx = engine_.pluginBank().builtInSlot (ch_, 9);
        if (idx >= 0) engine_.pluginBank().setBypass (ch_, idx, on);
        else if (player_ != nullptr)
            player_->muted.store (on, std::memory_order_relaxed);   // 插件被移除时兜底
        repaint();                       // 顶部旁通徽标显隐
    }

    //==================================================================
    // 歌词
    //==================================================================
    void reloadLyrics()
    {
        if (player_ == nullptr) return;
        const int idx = player_->currentIdx.load();
        if (idx < 0 || idx >= static_cast<int> (player_->files.size())) return;
        const auto& f = player_->files[static_cast<size_t> (idx)];
        // 新曲文件尚未就绪（刚添加 / 仍写入中）：保留上一曲歌词，等下一拍再试，
        // 避免切歌瞬间闪一下「未找到歌词」再显示正确歌词。
        if (! f.existsAsFile()) return;
        Lyrics probe;
        if (probe.loadForFile (f))
            probe.applyTimeOffset (player_->offsetMs.load() / 1000.0);
        // 成功→换上新歌词；确认无歌词→清空（正常显示「未找到歌词」）；
        // 文件存在但本次读取失败→保留旧歌词，下一拍重试，不闪「未找到歌词」。
        lyrics_.lyrics = std::move (probe);
    }

    //==================================================================
    // 频谱视觉区（S-Rack VisualizerComponent 同款：渐变底 + 气泡 +
    // LED 分段柱 + 倒影 + 基线；数据来自播放器 tap 环 → FFT）
    //==================================================================
    struct Visualizer : juce::Component
    {
        static constexpr int kFftSize  = 512;
        static constexpr int kBands    = 48;         // S-Rack numBars = 48

        struct Bubble { float x, y, speed, r, alpha, phase; };

        Visualizer()
        {
            fft = std::make_unique<juce::dsp::FFT> (9);
            window.resize (kFftSize);
            for (int i = 0; i < kFftSize; ++i)
                window[static_cast<size_t> (i)] =
                    0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / (kFftSize - 1));

            juce::Random rng (20261113);
            for (int i = 0; i < 40; ++i)
            {
                Bubble b;
                b.x = rng.nextFloat();
                b.y = rng.nextFloat();
                b.speed = 0.5f + rng.nextFloat() * 1.5f;
                b.r = 1.5f + rng.nextFloat() * 3.5f;
                b.alpha = 0.1f + rng.nextFloat() * 0.4f;
                b.phase = rng.nextFloat() * juce::MathConstants<float>::twoPi;
                bubbles.push_back (b);
            }
        }

        /** 面板定时器调用：active = 正在播放且未旁通。 */
        void pull (mb::MediaPlayerBank::Player* p)
        {
            if (p == nullptr) { decay(); return; }
            const auto total = p->tapWritten.load (std::memory_order_relaxed);
            if (total < 256) { decay(); return; }

            const int avail = static_cast<int> (juce::jmin<juce::uint64> (total,
                (juce::uint64) mb::MediaPlayerBank::Player::kTapFrames));
            const int take = juce::jmin (kFftSize, avail);
            for (int i = 0; i < take; ++i)
            {
                const int src = static_cast<int> ((total - static_cast<juce::uint64> (take) + i)
                    % static_cast<juce::uint64> (mb::MediaPlayerBank::Player::kTapFrames));
                const float m = 0.5f * (p->tapL[static_cast<size_t> (src)]
                                      + p->tapR[static_cast<size_t> (src)]);
                fftData[static_cast<size_t> (i)] = m * window[static_cast<size_t> (i)];
            }
            for (int i = take; i < kFftSize; ++i)
                fftData[static_cast<size_t> (i)] = 0.0f;

            fft->performRealOnlyForwardTransform (fftData.data(), true);
            const double nyquist = juce::jmax (8000.0, sampleRate * 0.5);
            for (int b = 0; b < kBands; ++b)
            {
                const double fLo = 40.0 * std::pow (400.0,  b / static_cast<double> (kBands));
                const double fHi = 40.0 * std::pow (400.0, (b + 1) / static_cast<double> (kBands));
                const int binLo = juce::jlimit (1, kFftSize / 2 - 1,
                                                static_cast<int> (fLo / nyquist * (kFftSize / 2)));
                const int binHi = juce::jlimit (binLo + 1, kFftSize / 2,
                                                static_cast<int> (fHi / nyquist * (kFftSize / 2)));
                float binMax = 0.0f;
                for (int k = binLo; k < binHi; ++k)
                {
                    const float re = fftData[static_cast<size_t> (k * 2)];
                    const float im = fftData[static_cast<size_t> (k * 2 + 1)];
                    binMax = juce::jmax (binMax, std::sqrt (re * re + im * im));
                }
                // dB 刻度：满幅正弦经汉宁窗后 bin 峰值 ≈ N/4 → 0 dB 满格，
                // -54 dB 到底。线性刻度会让低频段长期顶满不动。
                const double norm = binMax / (kFftSize * 0.25);
                const double dB = 20.0 * std::log10 (norm + 1e-6);
                const float level = juce::jlimit (0.0f, 1.0f, (float) ((dB + 54.0) / 54.0));
                levels[static_cast<size_t> (b)] = juce::jmax (
                    level, levels[static_cast<size_t> (b)] * 0.86f);
            }
        }

        void decay()
        {
            for (int b = 0; b < kBands; ++b)
            {
                levels[static_cast<size_t> (b)] *= 0.86f;
                peaks[static_cast<size_t> (b)] = juce::jmax (levels[static_cast<size_t> (b)],
                    peaks[static_cast<size_t> (b)] - 0.010f);
            }
        }

        void tick (double dt)
        {
            timeAccum += dt;
            for (int b = 0; b < kBands; ++b)
                peaks[static_cast<size_t> (b)] = juce::jmax (levels[static_cast<size_t> (b)],
                    peaks[static_cast<size_t> (b)] - 0.010f);
            const float hPix = juce::jmax (1.0f, static_cast<float> (getHeight()));
            for (auto& b : bubbles)
            {
                b.y -= static_cast<float> (b.speed / hPix);
                if (b.y < -0.02f)
                {
                    b.y = 1.02f;
                    b.x = juce::Random::getSystemRandom().nextFloat();
                }
            }
        }

        void paint (juce::Graphics& g) override
        {
            sr::syncTheme();
            const auto bounds = getLocalBounds().toFloat();
            const float w = bounds.getWidth(), h = bounds.getHeight();

            // 渐变底（深色青蓝 / 浅色淡蓝）
            juce::ColourGradient bg (sr::vizBg1, 0.0f, 0.0f,
                                     sr::vizBg2, 0.0f, h, false);
            bg.addColour (0.55, sr::vizBgMid);
            g.setGradientFill (bg);
            g.fillRoundedRectangle (bounds, 6.0f);

            // 上升气泡
            for (const auto& b : bubbles)
            {
                const float bx = (b.x + 0.012f * std::sin ((float) timeAccum * 0.9f + b.phase)) * w;
                const float by = b.y * h;
                g.setColour (sr::accent.withAlpha (b.alpha * 0.85f));
                g.fillEllipse (bx - b.r, by - b.r, b.r * 2.0f, b.r * 2.0f);
                g.setColour (juce::Colours::white.withAlpha (b.alpha * 0.30f));
                g.fillEllipse (bx - b.r * 0.35f, by - b.r * 0.5f, b.r * 0.7f, b.r * 0.7f);
            }

            // LED 频谱柱 + 倒影（S-Rack：基线 h-96，柱顶 0.34h）
            const float baseline = h > 140.0f ? h - 96.0f : h * 0.70f;
            const float barTop = h * 0.34f;
            drawLedBars (g, w, barTop, baseline);

            const float reflectH = h - baseline;
            if (reflectH > 4.0f)
            {
                g.saveState();
                g.reduceClipRegion (juce::Rectangle<int> (0, (int) baseline + 1, (int) w,
                                                          (int) reflectH - 2));
                g.addTransform (juce::AffineTransform::scale (1.0f, -1.0f)
                                    .translated (0.0f, 2.0f * baseline));
                drawLedBars (g, w, barTop, baseline);
                g.restoreState();
                g.setColour (sr::vizReflect.withAlpha (0.75f));
                g.fillRect (0.0f, baseline + 1.0f, w, reflectH - 2.0f);
            }

            // 基线
            g.setColour (sr::accent.withAlpha (0.55f));
            g.fillRect (0.0f, baseline, w, 2.0f);
        }

        void drawLedBars (juce::Graphics& g, float w, float barTop, float baseline)
        {
            const float unit = w / static_cast<float> (kBands);
            const float barW = unit * 0.9f;
            const float barInset = (unit - barW) * 0.5f;
            const float maxBar = baseline - barTop;
            if (maxBar < 12.0f) return;

            // 画布级垂直渐变：底绿 → 0.6 黄 → 顶红
            juce::ColourGradient vu (sr::green, 0.0f, baseline,
                                     sr::red, 0.0f, barTop, false);
            vu.addColour (0.6, sr::yellow);
            g.setGradientFill (vu);

            const float segPitch = 5.0f, segHeight = 3.5f;
            for (int i = 0; i < kBands; ++i)
            {
                const float x = static_cast<float> (i) * unit + barInset;
                const float hgt = juce::jlimit (0.0f, 1.0f, levels[static_cast<size_t> (i)]) * maxBar;

                if (hgt >= 1.0f)
                {
                    float yLo = 0.0f;
                    while (yLo + 0.5f < hgt)
                    {
                        g.fillRect (x, baseline - yLo - segHeight, barW, segHeight);
                        yLo += segPitch;
                    }
                }
                const float peakH = juce::jlimit (0.0f, 1.0f, peaks[static_cast<size_t> (i)]) * maxBar;
                if (peakH >= 1.0f)
                    g.fillRect (x, baseline - peakH - 2.0f, barW, 2.0f);
            }
        }

        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window;
        std::vector<Bubble> bubbles;
        double timeAccum = 0.0;
        double sampleRate = 48000.0;             // 面板定时器同步（bin 映射用真实采样率）
        std::array<float, kFftSize * 2> fftData {};
        std::array<float, kBands> levels {};
        std::array<float, kBands> peaks {};
    };

    //==================================================================
    // KTV 歌词浮层（S-Rack LyricsOverlay 同款：双行错位 + 深色衬底 +
    // 逐字红→黄→绿渐染；未唱白字带阴影）
    //==================================================================
    struct LyricsOverlay : juce::Component
    {
        Lyrics lyrics;

        void paint (juce::Graphics& g) override
        {
            sr::syncTheme();
            const auto* p = owner != nullptr ? owner->player_ : nullptr;
            auto& lyr = lyrics;

            if (! lyr.isLoaded())
            {
                const auto b = getLocalBounds().toFloat();
                const bool hasTrack = p != nullptr && p->currentIdx.load() >= 0;
                if (hasTrack)
                {
                    g.setColour (sr::dim);
                    g.setFont (mb::ui::font (13.0f));
                    g.drawText (mb::tr ("未找到歌词文件", "No lyrics file found"),
                                juce::Rectangle<float> (0.0f, b.getY(), b.getWidth(), b.getHeight() * 0.52f),
                                juce::Justification::centred);
                    g.setColour (sr::faint);
                    g.setFont (mb::ui::font (10.5f));
                    g.drawText (mb::tr ("把同名 .lrc 放到歌曲同目录",
                                        "Put a matching .lrc in the song folder"),
                                juce::Rectangle<float> (0.0f, b.getY() + b.getHeight() * 0.52f,
                                                        b.getWidth(), b.getHeight() * 0.48f),
                                juce::Justification::centred);
                }
                else
                {
                    g.setColour (sr::dim);
                    g.setFont (mb::ui::font (13.0f));
                    g.drawText (mb::tr ("未加载曲目", "No track loaded"),
                                b, juce::Justification::centred);
                }
                return;
            }

            const double t = p != nullptr ? p->positionSec() : 0.0;
            const int cur = lyr.lineIndexAt (t);
            const auto bounds = getLocalBounds().toFloat();
            const float w = bounds.getWidth(), h = bounds.getHeight();

            constexpr float rowGap = 14.0f, outerPad = 2.0f;
            const float rowH = juce::jmax (1.0f, (h - outerPad * 2.0f - rowGap) * 0.5f);
            const auto row1 = juce::Rectangle<float> (0.0f, outerPad, w, rowH);
            const auto row2 = juce::Rectangle<float> (0.0f, outerPad + rowH + rowGap, w, rowH);

            if (cur >= 0)
            {
                drawKtvLine (g, lyr.line (cur), t, row1, w * 0.36f, true);
                drawKtvLine (g, lyr.line (cur + 1), t, row2, w * 0.66f, false);
            }
            else if (lyr.getLineCount() > 0)
            {
                drawKtvLine (g, lyr.line (0), t, row2, w * 0.66f, false);   // 尚未开唱：下行预告
            }
        }

        static void drawKtvLine (juce::Graphics& g, const LyricLine* L, double t,
                                 juce::Rectangle<float> area, float centreX, bool current)
        {
            if (L == nullptr || L->text.trim().isEmpty()) return;

            const float baseH = juce::jmin (current ? 34.0f : 28.0f, area.getHeight() * 0.78f);
            juce::Font font = mb::ui::font (baseH, true);
            float textW = juce::GlyphArrangement::getStringWidth (font, L->text);
            const float maxW = juce::jmin (area.getWidth() * 0.94f,
                                           2.0f * juce::jmin (centreX, area.getWidth() - centreX) * 0.96f);
            if (textW > maxW)
            {
                font = font.withHeight (baseH * maxW / textW);
                textW = maxW;
            }

            const float textX = centreX - textW * 0.5f;
            const auto textRect = juce::Rectangle<float> (textX, area.getY(), textW + 4.0f, area.getHeight());

            // 深色衬底（与频谱倒影隔开；浅色主题用白色半透明）
            const auto backing = textRect.expanded (12.0f, 2.0f);
            g.setColour (sr::lyricBacking);
            g.fillRoundedRectangle (backing, backing.getHeight() * 0.30f);

            g.setFont (font);
            g.setColour (mb::ui::isDarkTheme()
                             ? juce::Colours::black.withAlpha (0.55f)
                             : juce::Colours::white.withAlpha (0.65f));
            g.drawText (L->text, textRect.translated (2.0f, 2.0f),
                        juce::Justification::centredLeft, false);
            g.setColour (current ? sr::lyricText : sr::lyricTextDim);
            g.drawText (L->text, textRect, juce::Justification::centredLeft, false);

            // 已唱渐染：红→黄→绿（逐字或整行按时间）
            float filled = 0.0f;
            if (current)
            {
                if (! L->words.empty())
                {
                    for (const auto& wd : L->words)
                    {
                        if (t >= wd.t1)
                            filled += juce::GlyphArrangement::getStringWidth (font, wd.text);
                        else if (t > wd.t0)
                        {
                            const float f = (float) ((t - wd.t0) / juce::jmax (0.05, wd.t1 - wd.t0));
                            filled += juce::GlyphArrangement::getStringWidth (font, wd.text)
                                    * juce::jlimit (0.0f, 1.0f, f);
                            break;
                        }
                        else break;
                    }
                }
                else if (L->t1 > L->t0)
                {
                    filled = textW * (float) juce::jlimit (0.0, 1.0, (t - L->t0) / (L->t1 - L->t0));
                }
                filled = juce::jlimit (0.0f, textW, filled);
            }

            if (filled > 0.5f)
            {
                g.saveState();
                g.reduceClipRegion (juce::Rectangle<float> (textX - 2.0f, area.getY() - 2.0f,
                                                            filled + 2.0f, area.getHeight() + 4.0f)
                                        .toNearestInt());
                // 渐变坐标固定在整行文字上：逐字点亮时颜色不跳动
                juce::ColourGradient sung (juce::Colour (sr::red), textRect.getX(), textRect.getY(),
                                           juce::Colour (sr::green), textRect.getRight(),
                                           textRect.getBottom(), false);
                sung.addColour (0.55, juce::Colour (sr::yellow));
                g.setGradientFill (sung);
                g.drawText (L->text, textRect, juce::Justification::centredLeft, false);
                g.restoreState();
            }
        }

        MediaPlayerPanel* owner = nullptr;
    };

    //==================================================================
    // 歌词偏移弹窗（S-Rack LyricOffsetPopup 同款：垂直参考线，
    // 中点 = 同步；把手向上拖 = 歌词提前，向下拖 = 歌词延后，±10s）
    //==================================================================
    struct LyricOffsetPopup : juce::Component
    {
        MediaPlayerPanel* owner = nullptr;
        bool dragging = false;

        LyricOffsetPopup() { setAlwaysOnTop (true); }

        void syncFromPlayer() { if (! dragging) repaint(); }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            const float w = b.getWidth(), h = b.getHeight();
            const bool narrow = w < 60.0f;
            drawVerticalPopupPanel (g, *this, mb::tr ("歌词偏移", "Lyric offset"));

            // 调音台同款推子卡：主题 card 底 + 边框，左侧参考值、右侧推子
            const auto card = juce::Rectangle<float> (4.0f, 24.0f, w - 8.0f,
                                                      h - 24.0f - 34.0f);
            g.setColour (mb::ui::card);
            g.fillRoundedRectangle (card, 6.0f);
            g.setColour (mb::ui::border);
            g.drawRoundedRectangle (card, 6.0f, 1.0f);

            const auto tr = trackRect();
            const juce::Colour thumbC = mb::ui::accent[0];

            // 上 / 下方向提示
            g.setFont (mb::ui::font (narrow ? 8.0f : 9.0f));
            g.setColour (mb::ui::meterGreen);
            g.drawText (mb::tr ("提前", "Earlier"),
                        juce::Rectangle<float> (0, card.getY() + 1.0f, w, 12.0f),
                        juce::Justification::centred);
            g.setColour (mb::ui::meterYellow);
            g.drawText (mb::tr ("延后", "Later"),
                        juce::Rectangle<float> (0, card.getBottom() - 13.0f, w, 12.0f),
                        juce::Justification::centred);

            // 左侧参考值：-10..+10（0 为主刻度），与通道推子卡同款
            for (int s = -10; s <= 10; s += 2)
            {
                const float fy = yForSeconds ((double) s);
                juce::String t;
                if (s % 5 == 0) t = juce::String (s > 0 ? "+" : "") + juce::String (s);
                mb::ui::drawFaderRefTick (g, card.getX(), fy, t, s == 0);
            }

            // 导轨 + 物理手柄（窄面板用略窄帽体）
            const double off = currentOffset();
            const float hy = yForSeconds (off);
            mb::ui::drawFaderRailAndCap (g, tr, hy, thumbC,
                                         narrow ? 20.0f : 26.0f, 16.0f);

            // 底部数值
            g.setFont (mb::ui::font (narrow ? 10.5f : 12.0f, true));
            g.setColour (std::abs (off) < 0.05 ? mb::ui::text : mb::ui::accent[0]);
            g.drawText (offText (off), juce::Rectangle<float> (0, h - 27.0f, w, 19.0f),
                        juce::Justification::centred);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragging = true;
            applyFromY (e.position.y);
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging) applyFromY (e.position.y);
        }
        void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    private:
        juce::Rectangle<float> trackRect() const
        {
            const auto b = getLocalBounds().toFloat();
            const float cx = b.getWidth() - (b.getWidth() < 60.0f ? 15.0f : 18.0f);
            const float top = 42.0f;
            const float bottom = b.getHeight() - 48.0f;
            return { cx - 2.0f, top, 4.0f, juce::jmax (20.0f, bottom - top) };
        }

        float yForSeconds (double s) const
        {
            const auto tr = trackRect();
            return (float) (tr.getY() + (juce::jlimit (-10.0, 10.0, s) / 10.0 * 0.5 + 0.5) * tr.getHeight());
        }

        void applyFromY (float y)
        {
            const auto tr = trackRect();
            const double f = juce::jlimit (0.0, 1.0, (double) (y - tr.getY()) / tr.getHeight());
            double v = (f - 0.5) * 20.0;                 // 上=-10(提前) 下=+10(延后)
            v = juce::roundToInt (v * 10.0) / 10.0;      // 0.1s 步进
            if (owner != nullptr) owner->applyLyricOffset (v);
            repaint();
        }

        double currentOffset() const
        {
            return owner != nullptr && owner->player_ != nullptr
                 ? owner->player_->offsetMs.load() / 1000.0 : 0.0;
        }

        static juce::String offText (double off)
        {
            // 纯 ASCII，窄面板放得下，也避开中文字面量编码问题
            if (std::abs (off) < 0.05) return "0.0s";
            return (off < 0 ? "-" : "+") + juce::String (std::abs (off), 1) + "s";
        }
    };

    //==================================================================
    // 音量弹窗（S-Rack VolumePopup 同款：垂直调节器，上=200% 下=0%，
    // 双击面板归位 100%；100% = 原始增益 1.0，上限 200% 保留增益余量）
    //==================================================================
    struct VolumePopup : juce::Component
    {
        MediaPlayerPanel* owner = nullptr;
        bool dragging = false;
        float lastPainted = -1.0f;

        VolumePopup() { setAlwaysOnTop (true); }

        // 由面板定时器调用：只在数值真的变化时重绘，避免空闲时反复重建 String
        void syncFromPlayer()
        {
            const float v = currentVolume();
            if (! dragging && std::abs (v * 100.0f - lastPainted) > 0.4f)
                repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            const float w = b.getWidth(), h = b.getHeight();
            const float vol = currentVolume();
            lastPainted = vol * 100.0f;

            drawVerticalPopupPanel (g, *this, juce::String());   // 无标题

            // —— 调音台通道同款推子卡：card 底 + 边框，左侧参考值、右侧推子 ——
            // 标题移除后推子区整体上移 16px，轨道更长
            const auto card = juce::Rectangle<float> (4.0f, 10.0f, w - 8.0f, h - 44.0f);
            g.setColour (mb::ui::card);
            g.fillRoundedRectangle (card, 6.0f);
            g.setColour (mb::ui::border);
            g.drawRoundedRectangle (card, 6.0f, 1.0f);

            const auto tr = trackRect();
            const juce::Colour thumbC = mb::ui::accent[0];

            // 左侧参考值刻度（与调音台通道共用同一绘制函数）
            for (int s = 0; s <= 200; s += 25)
            {
                const float fy = yForVolume ((float) s / 100.0f);
                mb::ui::drawFaderRefTick (g, card.getX(), fy,
                                          s % 50 == 0 ? juce::String (s) : juce::String(),
                                          s % 100 == 0);
            }

            // 导轨 + 物理手柄：与调音台通道、内置效果器共用同一段绘制
            const float hy = yForVolume (vol);
            mb::ui::drawFaderRailAndCap (g, tr, hy, thumbC);

            // 底部数值
            g.setFont (mb::ui::font (10.5f, true));
            g.setColour (vol > 1.995f ? mb::ui::text : mb::ui::accent[0]);
            g.drawText (juce::String (juce::roundToInt (vol * 100.0f)) + "%",
                        juce::Rectangle<float> (0, h - 27.0f, w, 19.0f), juce::Justification::centred);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragging = true;
            applyFromY (e.position.y);
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging) applyFromY (e.position.y);
        }
        void mouseUp (const juce::MouseEvent&) override { dragging = false; }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            if (owner != nullptr) owner->applyVolume (1.0f);   // 双击归位 100%
            repaint();
        }

    private:
        juce::Rectangle<float> trackRect() const
        {
            const auto b = getLocalBounds().toFloat();
            const float cx = b.getWidth() - 20.0f;   // 推子靠卡右缘（左侧留给参考值刻度）
            const float top = 22.0f;
            const float bottom = b.getHeight() - 46.0f;
            return { cx - 2.0f, top, 4.0f, bottom - top };
        }

        float yForVolume (float v) const
        {
            const auto tr = trackRect();
            return (float) (tr.getY() + (1.0 - (double) juce::jlimit (0.0f, 2.0f, v) * 0.5) * tr.getHeight());
        }

        void applyFromY (float y)
        {
            const auto tr = trackRect();
            const float f = juce::jlimit (0.0f, 1.0f, (float) ((y - tr.getY()) / tr.getHeight()));
            const float v = (float) juce::roundToInt ((1.0f - f) * 200.0f) / 100.0f;
            if (owner != nullptr) owner->applyVolume (v);
            repaint();
        }

        float currentVolume() const
        {
            return owner != nullptr && owner->player_ != nullptr
                 ? juce::jlimit (0.0f, 2.0f, owner->player_->volume.load()) : 0.8f;
        }
    };

    //==================================================================
    // 播放模式弹窗（S-Rack PlayModePopup 同款：向上弹出的四选一列表）
    //==================================================================
    struct PlayModePopup : juce::Component
    {
        MediaPlayerPanel* owner = nullptr;
        int hoverRow = -1;

        PlayModePopup() { setAlwaysOnTop (true); }

        void syncFromPlayer() { repaint(); }

        void paint (juce::Graphics& g) override
        {
            drawVerticalPopupPanel (g, *this, mb::tr ("播放模式", "Play mode"));

            const int cur = owner != nullptr && owner->player_ != nullptr
                          ? owner->player_->mode.load() : mb::MediaPlayerBank::kListLoop;
            for (int i = 0; i < 4; ++i)
            {
                const auto r = rowRect (i);
                const bool sel = (i == cur);
                const bool hot = (i == hoverRow);

                if (sel || hot)
                {
                    g.setColour (sel ? mb::ui::slotBg
                                    : mb::ui::accent[0].withAlpha (0.12f));
                    g.fillRoundedRectangle (r.reduced (3.0f, 2.0f), 6.0f);
                }
                if (sel)
                {
                    g.setColour (mb::ui::accent[0].withAlpha (0.90f));
                    g.drawRoundedRectangle (r.reduced (3.0f, 2.0f), 6.0f, 1.0f);
                    g.fillEllipse (r.getX() + 7.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f);
                }

                g.setColour (sel ? mb::ui::text : mb::ui::subText);
                g.setFont (mb::ui::font (10.5f, sel));
                g.drawText (modeName (i),
                            juce::Rectangle<float> (r.getX() + 17.0f, r.getY(),
                                                    r.getWidth() - 21.0f, r.getHeight()),
                            juce::Justification::centredLeft);
            }
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int r = rowAt (e.position.y);
            if (r != hoverRow) { hoverRow = r; repaint(); }
        }
        void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; repaint(); }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const int r = rowAt (e.position.y);
            if (r >= 0 && owner != nullptr) owner->applyMode (r);
        }

    private:
        // 弹窗高度收敛到 166px，保证向上弹出时不会越过面板顶部
        juce::Rectangle<float> rowRect (int i) const
        {
            return { 3.0f, 26.0f + (float) i * 34.0f, (float) getWidth() - 6.0f, 32.0f };
        }
        int rowAt (float y) const
        {
            for (int i = 0; i < 4; ++i)
            {
                const auto r = rowRect (i);
                if (r.contains (r.getCentreX(), y)) return i;
            }
            return -1;
        }
        static juce::String modeName (int i)
        {
            switch (i)
            {
                case mb::MediaPlayerBank::kSequential: return mb::tr ("顺序播放", "Sequential");
                case mb::MediaPlayerBank::kListLoop:   return mb::tr ("列表循环", "List loop");
                case mb::MediaPlayerBank::kSingleLoop: return mb::tr ("单曲循环", "Repeat one");
                default:                               return mb::tr ("随机播放", "Shuffle");
            }
        }
    };

    //==================================================================
    void timerCallback() override
    {
        if (player_ == nullptr) return;

        // 当前曲变化 → 歌词 + 列表高亮 + 路径
        const int idx = player_->currentIdx.load();
        if (idx != lastIdx_)
        {
            lastIdx_ = idx;
            reloadLyrics();
            refreshPlaylist();
            pathLabel_.setText (idx >= 0 && idx < static_cast<int> (player_->files.size())
                                    ? player_->files[static_cast<size_t> (idx)].getFullPathName()
                                    : juce::String(),
                                juce::dontSendNotification);
            offsetPopup_.syncFromPlayer();
            modePopup_.syncFromPlayer();
            if (onChanged) onChanged();
        }

        // 播放/暂停图标 + 旁通徽标
        const bool playing = player_->wantPlay.load();
        if (playing != lastPlaying_)
        {
            lastPlaying_ = playing;
            playBtn_.setIcon (playing ? SrIconButton::Pause : SrIconButton::Play);
        }
        if (player_->muted.load() != lastMuted_)
        {
            lastMuted_ = player_->muted.load();
            repaint();                          // 旁通徽标显隐
        }

        // 旁通开关与插件圆点保持同步（在别处切换旁通时面板跟随）
        const bool byp = currentBypassState();
        if (bypassBtn_.getToggleState() != byp)
            bypassBtn_.setToggleState (byp, juce::dontSendNotification);

        // 进度（S-Rack：单标签 "00:00 / 04:32"）
        const double len = player_->trackLengthSec.load();
        const double pos = player_->positionSec();
        timeLabel_.setText (fmtTime (pos) + " / " + fmtTime (len), juce::dontSendNotification);
        if (! seeking_)
            seek_.setValue (len > 0.0 ? juce::jlimit (0.0, len, pos) / len : 0.0,
                            juce::dontSendNotification);

        // 弹窗内容同步（有变化才重绘）
        volumePopup_.syncFromPlayer();

        // 频谱：正在播放且未旁通才喂新数据，否则自然衰减
        visualizer_.sampleRate = juce::jmax (8000.0, bank_.deviceRate());
        const bool active = player_->wantPlay.load() && ! player_->muted.load();
        visualizer_.pull (active ? player_ : nullptr);
        visualizer_.tick (1.0 / 30.0);
        visualizer_.repaint();
        lyrics_.repaint();
        pushDesktopLyrics();            // 桌面歌词：当前行 + 已唱进度 + 主题配色
    }

    /** 把当前行 / 时间 / 配色推给桌面歌词窗口（内部按内容去重，未打开时零开销）。 */
    void pushDesktopLyrics()
    {
        if (desktopLyrics_ == nullptr) return;
        sr::syncTheme();
        const double t = player_ != nullptr ? player_->positionSec() : 0.0;
        auto& lyr = lyrics_.lyrics;
        const LyricLine* line = lyr.isLoaded() ? lyr.line (lyr.lineIndexAt (t)) : nullptr;

        DesktopLyricsWindow::Palette pal;
        pal.base    = sr::lyricText;                    // 与播放器当前歌词文字同色
        pal.sung0   = sr::red;                          // 已唱渐变与 KTV 浮层一致
        pal.sungMid = sr::yellow;
        pal.sung1   = sr::green;
        pal.outline = mb::ui::isDarkTheme() ? juce::Colours::black.withAlpha (0.80f)
                                            : juce::Colours::white.withAlpha (0.92f);
        pal.hint    = sr::dim;
        desktopLyrics_->setLine (line, t, pal,
            lyr.isLoaded() ? juce::String() : mb::tr ("未找到歌词", "No lyrics"),
            &lyr);               // 整本歌词供窗口计算全曲统一字号
    }

    static juce::String fmtTime (double s)
    {
        if (s < 0.0 || std::isnan (s) || std::isinf (s)) s = 0.0;
        const int total = static_cast<int> (s);
        if (total >= 3600)
            return juce::String::formatted ("%d:%02d:%02d",
                                            total / 3600, (total % 3600) / 60, total % 60);
        return juce::String::formatted ("%02d:%02d", total / 60, total % 60);
    }

    //==================================================================
    mb::AudioEngine& engine_;
    mb::MediaPlayerBank& bank_;
    int ch_ = -1;
    mb::MediaPlayerBank::Player* player_ = nullptr;
    std::vector<std::function<void()>> textRefreshers_;   // 语言切换时刷新的缓存文案

    std::unique_ptr<PlaylistModel> listModel_;
    PlaylistBox playlist_;
    SrPillButton addBtn_ { "add" }, modeBtn_ { "mode" };
    SrPillButton bypassBtn_ { "bypass", true };
    SrPillButton spectrumBtn_ { "spectrum", true }, lyricBtn_ { "lyrics", true };
    SrPillButton desktopLyricsBtn_ { "desktopLyrics", true };   // 桌面歌词开关
    std::unique_ptr<DesktopLyricsWindow> desktopLyrics_;         // 独立透明顶层窗口
    SrPillButton offsetBtn_ { "off" }, volumeBtn_ { "vol" };

    SrIconButton prevBtn_ { SrIconButton::Prev };
    SrIconButton playBtn_ { SrIconButton::Play, true };
    SrIconButton nextBtn_ { SrIconButton::Next };

    juce::Slider seek_;
    juce::Label timeLabel_;
    bool seeking_ = false;
    SrSliderLook sliderLook_;

    Visualizer visualizer_;
    LyricsOverlay lyrics_;
    LyricOffsetPopup offsetPopup_;
    VolumePopup volumePopup_;
    PlayModePopup modePopup_;
    DismissListener dismissListener_ { *this };

    int lastIdx_ = -2;
    bool lastPlaying_ = false;
    bool lastMuted_ = false;

    juce::Label pathLabel_;
    juce::Image logo_;

    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE (MediaPlayerPanel)
};
