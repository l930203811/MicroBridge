#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <string>
#include <vector>

#include "AudioEngine.h"
#include "ChannelIcon.h"
#include "ChannelLinks.h"
#include "I18n.h"
#include "ModernMenu.h"
#include "RouteGraph.h"
#include "Theme.h"

//==============================================================================
// 细轨推子外观：与所有内置插件共用 Theme.h 的 mb::ui::FaderSkin，
// 通道条 / 效果器 / 播放器推子的导轨、手柄、配色由同一段代码绘制。
//==============================================================================
using FaderLookAndFeel = mb::ui::FaderSkin;

//==============================================================================
class RoundButton : public juce::Component
{
public:
    juce::String glyph;
    juce::Colour colour = mb::ui::accent[0];
    bool isToggle = false, state = false, dimmed = false;
    bool alwaysLit = false;                      // 双态同色：仅以字形区分（如 L+R / L）
    bool whiteGlyph = false;                     // 图标恒白（与制式键帽字形一致，不随通道色）
    std::function<void()> onClick;

    RoundButton() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool active = (isToggle && state) || alwaysLit;
        const float a = dimmed ? 0.35f : 1.0f;
        const auto r = bounds.reduced (1.0f);
        const float rad = 4.5f;

        // —— 物理键帽质感：底部投影（抬起感）+ 立体渐变帽体 + 顶缘高光 ——
        g.setColour (juce::Colours::black.withAlpha (0.45f * a));
        g.fillRoundedRectangle (r.translated (0.0f, 1.8f), rad);

        juce::Colour top, bottom, line, glyphC;
        if (active)                                   // 按下发光：通道色帽体
        {
            top    = colour.brighter (0.25f).withAlpha (a);
            bottom = colour.darker (0.35f).withAlpha (a);
            line   = colour.darker (0.55f).withAlpha (a);
            glyphC = juce::Colours::white.withAlpha (a);
        }
        else                                          // 抬起：深色帽体
        {
            top    = mb::ui::card.brighter (0.10f).withAlpha (a);
            bottom = mb::ui::slotBg.darker (0.30f).withAlpha (a);
            line   = mb::ui::border.withAlpha (a);
            glyphC = whiteGlyph ? juce::Colours::white.withAlpha (a)
                                : colour.withAlpha (a);
        }
        g.setGradientFill (juce::ColourGradient (top, 0.0f, r.getY(),
                                                 bottom, 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle (r, rad);
        g.setColour (line);
        g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);
        g.setColour (juce::Colours::white.withAlpha ((active ? 0.30f : 0.10f) * a));
        g.drawLine (r.getX() + rad, r.getY() + 1.0f, r.getRight() - rad, r.getY() + 1.0f);

        if (glyph == "SPK")                            // 喇叭图标
        {
            const auto c = r.getCentre();
            juce::Path spk;
            spk.startNewSubPath (c.x - 7.0f, c.y - 3.0f);
            spk.lineTo (c.x - 3.0f, c.y - 3.0f);
            spk.lineTo (c.x + 1.0f, c.y - 7.0f);
            spk.lineTo (c.x + 1.0f, c.y + 7.0f);
            spk.lineTo (c.x - 3.0f, c.y + 3.0f);
            spk.lineTo (c.x - 7.0f, c.y + 3.0f);
            spk.closeSubPath();
            g.setColour (juce::Colours::black.withAlpha (0.45f * a));
            g.fillPath (spk, juce::AffineTransform::translation (0.0f, 0.7f));  // 凹刻阴影
            g.setColour (glyphC);
            g.fillPath (spk);

            if (! active)                                  // 关闭态：静音叉
            {
                g.drawLine (c.x + 3.0f, c.y - 3.0f, c.x + 7.0f, c.y + 3.0f, 1.2f);
                g.drawLine (c.x + 7.0f, c.y - 3.0f, c.x + 3.0f, c.y + 3.0f, 1.2f);
            }
            return;
        }

        // 字形凹刻：暗影 + 本体
        g.setFont (mb::ui::font (10.0f, true));
        g.setColour (juce::Colours::black.withAlpha (0.50f * a));
        g.drawText (glyph, r.translated (0.0f, 0.8f).toNearestInt(),
                    juce::Justification::centred, false);
        g.setColour (glyphC);
        g.drawText (glyph, r.toNearestInt(), juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        // 触控板「长按」会被 Windows 翻译成右键（PopupMenu）。按钮只接受左键，
        // 否则调音时误触右键会立刻把 M / S / R / 喇叭 的状态翻转。
        if (ev.mods.isPopupMenu() || ev.mods.isRightButtonDown()) return;
        if (isToggle) state = !state;
        if (onClick) onClick();
        repaint();
    }
};

//==============================================================================
// 通道声像条：推子卡内 dB 读数下方的水平 Pan 条。
// 等功率增益在 RouteGraph::compile() 生效，此处只负责 -1..+1 取值与显示。
// 拖动手柄实时写入 graph（与推子同模式）；双击归中；值标签 C / Lxx / Rxx
// 画在手柄空侧，避免重叠。
//==============================================================================
class PanBar : public juce::Component
{
public:
    mb::RouteGraph* graph = nullptr;
    int index = -1;
    juce::Colour accent = mb::ui::accent[0];
    std::function<void()> onGestureEnd;   // 拖动结束 / 双击归中后通知（脏标记）

    PanBar() { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); }

    void paint (juce::Graphics& g) override
    {
        if (graph == nullptr) return;
        const auto b = getLocalBounds().toFloat();
        const float a = graph->isEnabled (index) ? 1.0f : 0.40f;

        // 轨道槽
        g.setColour (mb::ui::slotBg.withAlpha (a));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (mb::ui::border.withAlpha (a));
        g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 0.8f);

        // 中点刻度
        g.setColour (mb::ui::faintText.withAlpha (a));
        g.fillRect (b.getCentreX() - 0.5f, b.getY() + 2.5f, 1.0f, b.getHeight() - 5.0f);

        // 手柄：底部投影 + 通道色（居中为主题色，偏移后跟随通道色）
        const float pv = graph->pan (index);
        const float t = (pv + 1.0f) * 0.5f;
        const float hw = 12.0f;
        const auto handle = juce::Rectangle<float> (
            b.getX() + t * (b.getWidth() - hw), b.getY() + 1.5f, hw, b.getHeight() - 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.40f * a));
        g.fillRoundedRectangle (handle.translated (0.0f, 1.0f), 3.0f);
        g.setColour ((std::abs (pv) < 0.005f ? mb::ui::text : accent).withAlpha (a));
        g.fillRoundedRectangle (handle, 3.0f);
        g.setColour (mb::ui::border.withAlpha (a));
        g.drawRoundedRectangle (handle.reduced (0.5f), 3.0f, 0.8f);

        // 值标签：画在手柄的另一侧
        const int pct = juce::roundToInt (std::abs (pv) * 100.0f);
        const auto label = pct == 0 ? mb::S("C")
            : (pv < 0.0f ? mb::S("L") : mb::S("R")) + juce::String (pct);
        const auto labelRect = (t < 0.5f)
            ? juce::Rectangle<float> (handle.getRight() + 2.0f, b.getY(),
                                      b.getRight() - handle.getRight() - 2.0f, b.getHeight())
            : juce::Rectangle<float> (b.getX(), b.getY(),
                                      handle.getX() - b.getX() - 2.0f, b.getHeight());
        if (labelRect.getWidth() > 12.0f)
        {
            g.setFont (mb::ui::font (7.5f, true));
            g.setColour (mb::ui::faintText.withAlpha (a));
            g.drawText (label, labelRect.toNearestInt(), juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        // 触控板长按会被 Windows 翻译成右键，声像条只接受左键
        if (graph == nullptr || ev.mods.isPopupMenu() || ev.mods.isRightButtonDown()) return;
        dragging_ = true;
        apply (ev.position.x);
    }
    void mouseDrag (const juce::MouseEvent& ev) override
    {
        if (dragging_) apply (ev.position.x);
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging_ && onGestureEnd) onGestureEnd();
        dragging_ = false;
    }
    void mouseDoubleClick (const juce::MouseEvent& ev) override
    {
        if (graph == nullptr || ev.mods.isRightButtonDown()) return;
        graph->setPan (index, 0.0f);
        repaint();
        if (onGestureEnd) onGestureEnd();
    }

private:
    void apply (float x)
    {
        const float w = static_cast<float> (getWidth());
        const float t = juce::jlimit (0.0f, 1.0f, (x - 6.0f) / juce::jmax (1.0f, w - 12.0f));
        graph->setPan (index, t * 2.0f - 1.0f);
        repaint();
    }
    bool dragging_ = false;
};

//==============================================================================
// 快捷键与链接角标按钮：四个按钮（M S / R 喇叭）正中间的空档，上下两行：
//   上行：已绑定 = 分组名（如 A1）；未绑定 = 链形图标 🔗
//   下行：已设快捷键 = 快捷键文本；未设 = 键盘图标 ⌨️
// 点击任意位置打开「快捷键与链接」配置弹窗。
//==============================================================================
class LinkBadgeButton : public juce::Component
{
public:
    int channelIndex = 0;
    std::function<void()> onClick;

    LinkBadgeButton() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        // 聚合显示：取第一条已配置动作的码/键；配置了多个动作时追加 "+" 角标
        const auto link = mb::links::primaryOf (channelIndex);
        auto code = mb::links::codeString (link);
        auto hk   = mb::links::hotkeyText (link);
        int cfgCount = 0;
        for (int a = 0; a < mb::links::kActionCount; ++a)
        {
            const auto l = mb::links::linkOf (channelIndex, a);
            if (l.bound() || l.hasHotkey()) ++cfgCount;
        }
        if (cfgCount > 1)
        {
            if (code.isNotEmpty()) code += mb::S("+");
            if (hk.isNotEmpty())   hk   += mb::S("+");
        }
        const bool hover = isMouseOver();

        if (code.isNotEmpty() || hk.isNotEmpty() || hover)
        {
            g.setColour (mb::ui::accent[0].withAlpha (hover ? 0.32f : 0.20f));
            g.fillRoundedRectangle (r, 4.0f);
        }

        // 上行与 M S 按钮行水平对齐，下行与 R 喇叭行对齐，中段留空防挤压
        const auto top    = r.removeFromTop (r.getHeight() * 0.34f);
        const auto bottom = r.removeFromBottom (r.getHeight() * 0.34f);

        // 上行：分组名 / 链形图标
        if (code.isNotEmpty())
        {
            g.setColour (mb::ui::accent[0]);
            g.setFont (mb::ui::font (7.5f, true));
            g.drawText (code, top.toNearestInt(), juce::Justification::centred, false);
        }
        else
        {
            drawLinkIcon (g, top);
        }

        // 下行：快捷键文本 / 键盘图标
        if (hk.isNotEmpty())
        {
            g.setColour (mb::ui::accent[0]);
            g.setFont (mb::ui::font (7.5f, true));
            g.drawFittedText (hk, bottom.toNearestInt(), juce::Justification::centred, 1, 0.5f);
        }
        else
        {
            drawKeyboard (g, bottom);
        }
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        // 只接受左键：右键留给通道整体菜单
        if (ev.mods.isPopupMenu() || ev.mods.isRightButtonDown()) return;
        if (onClick) onClick();
    }

private:
    /** 迷你链形图标：两枚斜 45° 圆角胶囊环扣。 */
    void drawLinkIcon (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto c = r.getCentre();
        const float s  = juce::jmin (r.getWidth(), r.getHeight());
        const float ang = juce::MathConstants<float>::pi / 4.0f;
        const float off = s * 0.09f;

        g.setColour (mb::ui::subText.withAlpha (0.9f));
        for (int i = 0; i < 2; ++i)
        {
            const float d = (i == 0 ? -off : off);
            juce::Path capsule;
            capsule.addRoundedRectangle (-s * 0.30f, -s * 0.13f, s * 0.60f, s * 0.26f,
                                         s * 0.13f, true);
            capsule.applyTransform (juce::AffineTransform::rotation (ang)
                                        .translated (c.x + d, c.y + d));
            g.strokePath (capsule, juce::PathStrokeType (1.4f));
        }
    }

    /** 迷你键盘图标：外框 + 两排键点 + 空格条。 */
    void drawKeyboard (juce::Graphics& g, juce::Rectangle<float> r)
    {
        r = r.reduced (2.0f);
        g.setColour (mb::ui::subText.withAlpha (0.85f));
        g.drawRoundedRectangle (r, 2.0f, 1.0f);

        const float keyY1 = r.getY() + r.getHeight() * 0.22f;
        const float keyY2 = r.getY() + r.getHeight() * 0.46f;
        const float step  = r.getWidth() / 5.0f;
        for (int i = 0; i < 4; ++i)
        {
            const float cx = r.getX() + step * static_cast<float> (i) + step * 0.5f;
            g.fillEllipse (cx - 1.0f, keyY1 - 1.0f, 2.0f, 2.0f);
            g.fillEllipse (cx - 1.0f, keyY2 - 1.0f, 2.0f, 2.0f);
        }
        g.fillRoundedRectangle (r.getX() + r.getWidth() * 0.22f,
                                r.getY() + r.getHeight() * 0.68f,
                                r.getWidth() * 0.56f, 2.4f, 1.0f);
    }
};

//==============================================================================
// 单条通道（参考 Studio One 混音台，自上而下四个区域）：
//   头部（颜色点 + CH 号）
//   ① 输入源区（点击弹菜单，可多路求和）
//   ② 插件区（第一行固定「内置效果器」，往下是 VST3 插件架 + 添加）
//   ③ 输出源区
//   ④ 推子区（dB 读数 + 电平表 + 推子）
//   按钮格：上行 M S，下行 R（录音布防）+ 喇叭（通道激活）
//   名字条（双击重命名）
//
// 拖动区域顶部边缘（±4px）= 调整该区高度（全通道共享）；
// 区域内部一律普通箭头光标，左右拖动 = 移动通道位置。喇叭关闭的通道整体变暗且不发声。
//==============================================================================
class ChannelStrip : public juce::Component,
                     public juce::SettableTooltipClient,
                     private juce::Slider::Listener
{
public:
    std::function<void (const juce::String&)> onMessage;
    std::function<void()> onChanged;
    std::function<void (int ch, int idx)> onOpenPlugin;    // 双击插件行 / 菜单「打开界面」
    std::function<void (int ch, int idx)> onRemovePlugin;  // 先关编辑器窗口再移除
    std::function<void (int ch, int idx)> onPluginOffline; // 离线前回调：MainComponent 先关编辑器
    std::function<void (int from, int to)> onReorder;      // 左右拖动移动通道
    std::function<void (int ch, float x, float y, int slot)> onDragBegin; // 拖拽特效：开始
    std::function<void (float x, float y)> onDragMove;     // 拖拽特效：跟随鼠标
    std::function<void (int fromSlot, int toSlot)> onDragFlash; // 拖拽特效：换位瞬间
    std::function<void()> onDragEnd;                       // 拖拽特效：结束
    std::function<void (int ch)> onDeleteChannel;          // 右键「删除通道」
    std::function<void (int ch)> onDuplicateChannel;       // 右键「复制通道」
    std::function<void (int ch, bool on)> onSetRightAligned; // 右键「向右对齐」
    // 通道链接联动：SPK/S 状态统一入口（含同步组 + 互斥组处理）
    std::function<void (int ch, int action, bool on)> onLinkSetState;
    std::function<void (int ch, float gain)> onLinkVolume;   // 链接联动：同码推子跟随
    /** 打开「快捷键与链接」弹窗。defaultAction：通道无任何链接/快捷键时
        预选的动作（mb::links::kFx 等；-1 = 不干预，用默认「开关」）。
        插件菜单入口传 kFx（意图是控制整条效果链），通道级入口传 -1。 */
    std::function<void (int ch, int defaultAction)> onLinkChannel;
    std::function<void (int ch, bool hidden)> onSetHidden; // 右键「隐藏通道」/恢复单条
    std::function<void()> onShowAllChannels;               // 右键「显示全部通道」
    std::function<void (int ch)> onActivateSlot;           // 空槽「新建通道」
    std::function<void (int ch)> onPickCustomColour;       // 右键「自定义 RGB…」
    std::function<int (int visibleSlot)> onSlotToChannel;  // 可见序号 → 真实通道索引
    std::function<int()> onVisibleCount;                   // 当前可见槽位数
    // 打开「输入源 / 输出目标」选择浮层（MainComponent 弹 SourcePicker 卡片）
    std::function<void (int ch, bool input)> onOpenSource;
    // 打开「添加插件」选择浮层（MainComponent 弹 PluginPicker 卡片）
    std::function<void (int ch)> onOpenPlugins;

    ChannelStrip (int index, mb::RouteGraph& graph, mb::AudioEngine& engine)
        : index_ (index), graph_ (graph), engine_ (engine), catalog_ (engine.catalog())
    {
        // paint() 以 fillAll 覆盖全部 bounds（背景色），声明不透明
        // 后 JUCE 在局部重绘时不再逐层重画父链背景，可见窗口的渲染开销显著下降。
        setOpaque (true);
        accent_ = mb::ui::stripColour (index_);
        iconId_ = mb::channelIcon (index_);
        lf_.thumb = accent_;

        fader_.setSliderStyle (juce::Slider::LinearVertical);
        fader_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader_.setRange (-60.0, 12.0, 0.1);
        fader_.setValue (static_cast<double> (mb::ui::gainToDb (graph_.fader (index_))),
                         juce::dontSendNotification);
        fader_.setLookAndFeel (&lf_);
        fader_.setPopupMenuEnabled (false);      // 推子自身也不要弹 JUCE 右键菜单
        fader_.addMouseListener (this, false);   // 转发推子上的双击（恢复 0 dB）
        fader_.addListener (this);
        addAndMakeVisible (fader_);

        // 声像条：推子卡内 dB 读数下方，拖动实时写 graph，双击归中
        panBar_.graph = &graph_;
        panBar_.index = index_;
        panBar_.accent = accent_;
        panBar_.onGestureEnd = [this] { if (onChanged) onChanged(); };
        addAndMakeVisible (panBar_);

        muteBtn_.glyph = mb::S("M");
        muteBtn_.colour = mb::ui::meterRed;
        muteBtn_.isToggle = true;
        muteBtn_.state = graph_.isMuted (index_);
        muteBtn_.onClick = [this]
        {
            graph_.setMuted (index_, muteBtn_.state);
            changed (mb::tr ("已更新静音", "Mute updated"));
        };
        addAndMakeVisible (muteBtn_);

        soloBtn_.glyph = mb::S("S");
        soloBtn_.colour = mb::ui::meterYellow;
        soloBtn_.isToggle = true;
        soloBtn_.state = graph_.isSolo (index_);
        soloBtn_.onClick = [this]
        {
            if (onLinkSetState) onLinkSetState (index_, mb::links::kSolo, soloBtn_.state);
            else graph_.setSolo (index_, soloBtn_.state);
            changed (mb::tr ("已更新独奏", "Solo updated"));
        };
        addAndMakeVisible (soloBtn_);

        recBtn_.glyph = mb::S("R");
        recBtn_.colour = mb::ui::meterRed;
        recBtn_.isToggle = true;
        recBtn_.state = mb::ui::isChannelArmed (index_);
        recBtn_.onClick = [this]
        {
            mb::ui::setChannelArmed (index_, recBtn_.state);   // R = 录音布防，配合底部录音机
            changed (recBtn_.state
                     ? mb::tr ("已布防：通道将录入录音机", "Armed for recorder")
                     : mb::tr ("已解除布防", "Disarmed"));
        };
        addAndMakeVisible (recBtn_);

        spkBtn_.glyph = mb::S("SPK");
        spkBtn_.colour = mb::ui::accent[6];       // 与顶部制式键帽同色，不跟随通道颜色
        spkBtn_.whiteGlyph = true;                // 喇叭图标恒白，与制式键帽字形一致
        spkBtn_.isToggle = true;
        spkBtn_.state = graph_.isEnabled (index_);
        spkBtn_.onClick = [this]
        {
            if (onLinkSetState)
                onLinkSetState (index_, mb::links::kPower, spkBtn_.state);
            else
            {
                graph_.setEnabled (index_, spkBtn_.state);  // 喇叭 = 通道激活开关
                updateDim();
            }
            changed (spkBtn_.state ? mb::tr ("通道已激活", "Channel enabled")
                                   : mb::tr ("通道已关闭", "Channel disabled"));
        };
        addAndMakeVisible (spkBtn_);

        // 快捷键与链接角标：键盘图标 / 快捷键文本 / 链接码，点击进配置弹窗
        linkBadge_.channelIndex = index_;
        linkBadge_.onClick = [this] { if (onLinkChannel) onLinkChannel (index_, -1); };
        addAndMakeVisible (linkBadge_);

        // 头部「声道制式」切换：L+R（立体声）⇄ L（单声道）。切到立体声后，
        // 单声道麦克风输入会自动复制为 L/R（居中）；电平表条数随之变化。
        // 不入右键菜单，避免菜单过长（向右对齐等低频操作只在右键菜单）。
        monoBtn_.isToggle = true;
        monoBtn_.colour = mb::ui::accent[6];   // 柔和靛蓝：与 M/S 同款键帽风格，避免纯绿的突兀感
        monoBtn_.alwaysLit = true;             // 双态同色：立体声 L+R / 单声道 L 仅字形不同
        monoBtn_.state = mb::ui::isChannelStereo (index_);
        monoBtn_.glyph = monoBtn_.state ? mb::S("L+R") : mb::S("L");
        monoBtn_.onClick = [this]
        {
            const bool toStereo = ! mb::ui::isChannelStereo (index_);
            mb::ui::setChannelStereo (index_, toStereo);     // 电平表单/双条
            graph_.setChannelStereo (index_, toStereo);      // 路由内核展开 L/R 边
            monoBtn_.state = toStereo;
            monoBtn_.glyph = toStereo ? mb::S("L+R") : mb::S("L");
            changed (toStereo
                     ? mb::tr ("通道已切换为立体声（单声道输入将复制为 L/R）",
                               "Channel set to stereo (mono input duplicated to L/R)")
                     : mb::tr ("通道已切换为单声道", "Channel set to mono"));
        };
        addAndMakeVisible (monoBtn_);
    }

    ~ChannelStrip() override
    {
        fader_.removeMouseListener (this);
        fader_.removeListener (this);
        fader_.setLookAndFeel (nullptr);
    }

    int channelIndex() const noexcept { return index_; }
    void applyLanguage() { invalidate(); }

    /** 窗口卷起折叠态：不绘制推子卡（dB 读数 / 刻度 / 电平表 / 推子），
        底部只保留 M/S/R/喇叭按钮格与名称带；同时停掉 20 Hz 电平局部重绘。 */
    void setFoldedCompact (bool f)
    {
        if (foldedCompact_ == f) return;
        foldedCompact_ = f;
        panBar_.setVisible (! f);       // 折叠态连声像条一起隐藏
        metersIdle_ = false;            // 展开后重新起跳，避免残留的静默跳过
        repaint();
    }

    /** 20 Hz 电平刷新：只重绘电平表 LED 窄条（组件坐标 x≈18..43），
     *  且通道静默（电平与峰值保持都归零）时完全跳过重绘。
     *  云电脑/远程桌面的合成与编码按「每帧脏区面积」计费，静止通道零开销
     *  是可见窗口 CPU 的决定性优化。静默期峰值保持线冻结在原地（LED 段
     *  粒度内不可感知），信号恢复或 invalidate() 后自动恢复衰减。 */
    void repaintMeters()
    {
        if (foldedCompact_) return;      // 折叠态不显示电平，也不产生局部脏区
        const auto slot = mb::ui::slotState (index_);
        if (slot != mb::ui::kSlotNormal || faderRegion_.isEmpty()) return;

        const bool silent = graph_.isMuted (index_) || ! graph_.isEnabled (index_);
        float la = 0.0f, lb = 0.0f;
        if (! silent)
        {
            // 电平表为推子后表显：输入端口电平乘通道推子增益（保留 L/R 立体声
            // 细节），与通道总峰值（引擎已乘推子，含插件 / 互送）取大者——
            // 拉低推子电平跟着降，播放器插件发声时电平同样跳动。
            int pa = -1, pb = -1;
            bool stereoPair = false;
            meterPorts (pa, pb, stereoPair);
            const float fg = graph_.fader (index_);
            const float total = engine_.channelPeak (index_);
            if (mb::ui::isChannelStereo (index_))
            {
                // 立体声通道：L/R 分别读通道缓冲峰值（引擎已含推子/效果/互送）
                la = mb::ui::levelToMeter (engine_.channelPeakL (index_));
                lb = mb::ui::levelToMeter (engine_.channelPeakR (index_));
            }
            else if (pa >= 0)
            {
                // 单声道通道：永远单条（跟随通道制式）。双通道设备接入时
                // 内核已把 L+R 求和进通道 L 缓冲，total 即为通道真实内容。
                la = mb::ui::levelToMeter (juce::jmax (engine_.inputPortPeak (pa) * fg, total));
            }
            else
            {
                la = mb::ui::levelToMeter (total);
            }
        }
        const bool idle = la < 0.004f && lb < 0.004f
                       && peakHold_[0] <= 0.012f && peakHold_[1] <= 0.012f;
        if (idle && metersIdle_) return;         // 已静默且画过最后一帧 → 零开销
        metersIdle_ = idle;

        const int top = juce::roundToInt (faderRegion_.getY()) + 38;
        const int hgt = juce::roundToInt (faderRegion_.getHeight()) - 44;
        repaint (juce::Rectangle<int> (18, top, 25, juce::jmax (20, hgt)));
    }

    /** RGB 浮层确定后回写颜色（与预设色共用同一持久化路径）。 */
    void applyCustomColour (juce::Colour c)
    {
        accent_ = c.withAlpha (static_cast<juce::uint8> (255));
        auto& cols = mb::ui::customChannelColours();
        if (static_cast<int> (cols.size()) <= index_)
            cols.resize (static_cast<size_t> (index_ + 1));
        cols[static_cast<size_t> (index_)] = accent_;
        lf_.thumb = accent_;                      // 推子仍跟随通道色
        panBar_.accent = accent_;                 // 声像手柄跟随通道色
        panBar_.repaint();
        spkBtn_.colour = mb::ui::accent[6];       // 喇叭键帽恒定，不受换色影响
        updateDim();
        changed (mb::tr ("已更换通道颜色", "Colour updated"));
    }

    /** 选择预设图标后回写（空串 = 恢复默认无图标）。 */
    void applyCustomIcon (const juce::String& id)
    {
        iconId_ = id;
        auto& icons = mb::customChannelIcons();
        while (icons.size() <= index_) icons.add (juce::String());
        icons.set (index_, id);
        invalidate();
    }

    /** 槽位状态变化后由 MainComponent 调用，触发重绘。 */
    void refreshSlot() { invalidate(); }

    /** 拖拽中把该通道条「抬起」：高亮描边 + 顶部抓手，不改动 bounds/transform。 */
    void setDragging (bool on)
    {
        if (dragging_ == on) return;
        dragging_ = on;
        repaint();
    }

    /** 新建通道后由外部调用：弹出命名框并预填 CH 编号（保持编辑状态，
        回车或点击其他位置即保存预填编号；Esc 取消）。 */
    void promptRename() { beginRenameWith (mb::channelDisplayName (index_)); }

    /** 联动方（音量链接）写回本条推子时不走通知，避免再来一轮 sliderValueChanged。 */
    void syncFaderFromGraph()
    {
        fader_.setValue (static_cast<double> (mb::ui::gainToDb (graph_.fader (index_))),
                         juce::dontSendNotification);
        fader_.repaint();
    }

    std::function<void (int)> onShowFx;      // 打开通道效果器面板（内置插件行 / 添加菜单）

    /** 通道被拖动到新位置后调用：更新索引并整体刷新。 */
    void setChannelIndex (int i)
    {
        index_ = i;
        syncFromGraph();
    }

    void syncFromGraph()
    {
        accent_ = mb::ui::stripColour (index_);           // 颜色可能被会话/预设更新
        iconId_ = mb::channelIcon (index_);               // 图标随会话/预设恢复
        panBar_.index = index_;
        panBar_.accent = accent_;
        spkBtn_.colour = mb::ui::accent[6];       // 喇叭键帽不与通道色联动
        muteBtn_.state = graph_.isMuted (index_);
        soloBtn_.state = graph_.isSolo (index_);
        fader_.setValue (static_cast<double> (mb::ui::gainToDb (graph_.fader (index_))),
                         juce::dontSendNotification);
        recBtn_.state = mb::ui::isChannelArmed (index_);
        spkBtn_.state = graph_.isEnabled (index_);
        monoBtn_.colour = mb::ui::accent[6];     // 跟随主题色板，与喇叭键帽保持一致
        monoBtn_.state = mb::ui::isChannelStereo (index_);
        monoBtn_.glyph = monoBtn_.state ? mb::S("L+R") : mb::S("L");
        monoBtn_.repaint();
        updateDim();
        muteBtn_.repaint();
        soloBtn_.repaint();
        recBtn_.repaint();
        spkBtn_.repaint();
        invalidate();                        // accent_/状态可能变了 → 静态层重建
    }

    /** 喇叭关闭：整体变暗（按钮、推子手柄、电平、文字同步降透明）。 */
    void updateDim()
    {
        const bool on = graph_.isEnabled (index_);
        muteBtn_.dimmed = soloBtn_.dimmed = recBtn_.dimmed = ! on;
        spkBtn_.dimmed = false;
        lf_.thumb = on ? accent_ : accent_.withAlpha (0.35f);
        fader_.repaint();
    }

    //==========================================================================
    // 渲染分两层：静态层（背景 / 区域 / 插件行 / 按钮格 / 脊线 / 名字条）只在
    // 状态变化时重画进缓存位图；20 Hz 电平刷新只 blit 缓存 + 重画推子卡。
    // 云电脑软渲染下整条 paint 主体逐帧执行的文本/路径开销是 CPU 大头。
    void paint (juce::Graphics& g) override
    {
        const int w = getWidth(), h = getHeight();

        // 缓存必须按「总设备缩放」建：系统 DPI 缩放 × 界面缩放（如 1.25 × 1.1 = 1.375）。
        // JUCE 8 Windows 渲染变换 = peer 平台缩放(DPI) × handlePaint 里按
        // 「peer 逻辑尺寸 / 顶层组件尺寸」补的比例。取两者乘积即真实设备缩放，
        // 只取界面缩放会让缓存仍被 DPI 拉伸重采样（表现为文字发虚）。
        float devScale = 1.0f;
        if (auto* peer = getPeer())
        {
            const auto pb = peer->getBounds();
            const auto cb = peer->getComponent().getLocalBounds();
            if (cb.getWidth() > 0 && cb.getHeight() > 0)
                devScale = (float) ((double) pb.getWidth()  / (double) cb.getWidth()
                                    * peer->getPlatformScaleFactor());
        }
        devScale = juce::jlimit (0.01f, 10.0f, devScale);
        const int pw = juce::jmax (1, juce::roundToInt (w * devScale));
        const int ph = juce::jmax (1, juce::roundToInt (h * devScale));

        if (staticDirty_ || staticCache_.isNull()
            || staticCache_.getWidth() != pw || staticCache_.getHeight() != ph)
        {
            staticCache_ = juce::Image (juce::Image::ARGB, pw, ph, true);
            juce::Graphics ig (staticCache_);
            if (devScale > 1.001f || devScale < 0.999f)
                ig.addTransform (juce::AffineTransform::scale (devScale));
            drawStatic (ig, w, h);
            staticDirty_ = false;
        }
        // 目标矩形乘总缩放后与源像素 1:1，避免重采样
        if (devScale > 1.001f || devScale < 0.999f)
            g.drawImageTransformed (staticCache_,
                                    juce::AffineTransform::scale (1.0f / devScale));
        else
            g.drawImageAt (staticCache_, 0, 0);

        const auto slot = mb::ui::slotState (index_);
        if (slot == mb::ui::kSlotHidden || slot == mb::ui::kSlotEmpty) return;

        // 逐帧动态：推子卡（dB 读数 + 电平表 LED + 峰值保持 + 推子手柄）。
        // 折叠态只露底部控制带，推子区会与按钮格重叠，整体不绘制。
        if (! foldedCompact_)
            drawFaderCard (g, faderRegion_);

        // 通道色纵线画在推子卡之上（贯穿卡区连到名字条）；任何局部/全量
        // 重绘路径都会重画，不受静态缓存与全局缩放影响。
        // 横向被裁得只剩窄条（贴视口右缘）的通道不画：几像素的纵线看起来
        // 像混音台右边缘的一条孤立杂线，删通道后还会「挪」到别的通道上。
        bool sliver = false;
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        {
            const int viewRight = vp->getViewPositionX() + vp->getWidth();
            sliver = (viewRight - getX()) < 40;
        }
        if (! sliver)
            drawSpine (g, w, h);

        // 喇叭关闭：整体压暗（子组件由 dimmed 标志各自变暗）
        if (! graph_.isEnabled (index_))
        {
            g.setColour (juce::Colours::black.withAlpha (0.16f));
            g.fillAll();
        }

        // 拖拽态：整体提亮 + 白色描边 + 顶部抓手，明确「这一条正被拖动」
        if (dragging_)
        {
            g.setColour (accent_.withAlpha (0.16f));
            g.fillRect (0, 0, w, h);
            g.setColour (juce::Colours::white.withAlpha (0.90f));
            g.drawRect (1, 1, w - 2, h - 2, 2);
            g.setColour (accent_);
            g.fillRoundedRectangle (juce::Rectangle<float> (
                10.0f, 4.0f, static_cast<float> (w - 20), 5.0f), 2.5f);
        }
    }

    /** 静态层内容：除推子卡、压暗、拖拽高亮外的全部绘制（含命中热区重建）。 */
    void drawStatic (juce::Graphics& g, int w, int h)
    {
        pluginRowHits_.clear();
        pluginDotHits_.clear();
        sourceHits_.clear();
        inputAdd_ = {};  outputAdd_ = {};  pluginAdd_ = {};
        colourDot_ = {}; nameBand_ = {};

        g.fillAll (index_ % 2 == 0 ? mb::ui::stripEven : mb::ui::stripOdd);

        // 空槽（删除后占位）/ 隐藏：不绘制四区内容
        const auto slot = mb::ui::slotState (index_);
        if (slot == mb::ui::kSlotHidden) return;
        if (slot == mb::ui::kSlotEmpty) { drawEmptySlot (g, w, h); return; }

        const auto L = computeLayout();
        inputRegion_  = { 4.0f, static_cast<float> (L.yIn),  w - 8.0f, static_cast<float> (L.hIn) };
        pluginRegion_ = { 4.0f, static_cast<float> (L.yPl),  w - 8.0f, static_cast<float> (L.hPl) };
        outputRegion_ = { 4.0f, static_cast<float> (L.yOut), w - 8.0f, static_cast<float> (L.hOut) };
        faderRegion_  = { 4.0f, static_cast<float> (L.yFd),  w - 8.0f, static_cast<float> (L.hFd) };

        drawHeader (g, w);

        // 第一行：输入源（顶部「+」按钮 + 已添加连接列表）
        // 三个区域左侧纵线统一跟随通道色 accent_（与右侧电平/推子、名字条一致）
        drawRegionFrame (g, inputRegion_, {}, accent_);
        drawSourceRegion (g, inputRegion_, true);

        // 第二行：插件（内置效果器 + VST3）
        drawRegionFrame (g, pluginRegion_, {}, accent_);
        drawInserts (g, pluginRegion_);

        // 第三行：输出源
        drawRegionFrame (g, outputRegion_, {}, accent_);
        drawSourceRegion (g, outputRegion_, false);

        // 按钮格背景（通道色纵线已移到动态层：paint() 中 drawFaderCard 之后
        // 调用 drawSpine，避免推子卡背景在缩放/全量重绘时盖住纵线）
        drawButtonGrid (g, h - mb::ui::kNameBandHeight - kButtonGridH, w);

        drawNameBand (g, h - mb::ui::kNameBandHeight, mb::ui::kNameBandHeight, w);

        // 分组分隔改由 StripContainer 在两组间隙画灰+黑斑马线带，
        // 不在通道条边缘画彩色细线，避免被误认为通道边框。
    }

    /** 状态变化后使静态层失效（下次 paint 重建缓存）。 */
    void invalidate()
    {
        staticDirty_ = true;
        repaint();
    }

    void resized() override
    {
        staticDirty_ = true;                 // 尺寸变化 → 静态层缓存重建
        const auto L = computeLayout();
        panBar_.setBounds (8, L.yFd + 21, getWidth() - 16, 13);   // dB 读数下方横贯声像条
        fader_.setBounds (getWidth() - 34, L.yFd + 40, 26, juce::jmax (40, L.hFd - 54));   // 推子靠推子卡右缘（声像条下方）

        const int gridTop = getHeight() - mb::ui::kNameBandHeight - kButtonGridH + 3;
        muteBtn_.setBounds (4,  gridTop,      22, 22);   // 上行：M S（左右拉开）
        soloBtn_.setBounds (58, gridTop,      22, 22);
        recBtn_.setBounds  (4,  gridTop + 28, 22, 22);   // 下行：R 喇叭 + 快捷键角标
        spkBtn_.setBounds  (58, gridTop + 28, 22, 22);
        // 四个按钮（M S / R 喇叭）的几何中心：x=42；角标整块跨两行按钮，
        // 上行图标对齐 M S 行中心（gridTop+11）、下行对齐 R 喇叭行中心（gridTop+39）
        linkBadge_.setBounds (32, gridTop + 4, 20, 42);
        monoBtn_.setBounds (getWidth() - 24, 3, 20, 18);    // 头部右上角：声道制式切换
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        // fader_ 通过 addMouseListener(this,false) 把 down/drag/up/doubleClick
        // 全部转发到本组件，且转发的 ev.position 是**相对 Slider 的局部坐标**
        // （JUCE HierarchyChecker::eventWithNearestParent 不做坐标换算）。
        // 这些事件不能拿去匹配本组件热区，否则会误触发输入/插件/名字条动作。
        if (ev.eventComponent == &fader_)
        {
            if (ev.mods.isPopupMenu() || ev.mods.isRightButtonDown())
            {
                // 静止右键 / 触控板长按 → 通用菜单；正在拖推子时的长按误触 → 抑制
                if (! faderDragging_) showGeneralMenu();
                faderPointerDown_ = false;
            }
            else
            {
                faderPointerDown_ = true;
                faderDragging_ = false;
            }
            return;
        }
        if (ev.eventComponent != this)
        {
            dragMode_ = DragMode::None;
            return;
        }

        const auto p = ev.position;
        const auto slot = mb::ui::slotState (index_);
        const bool wantMenu = ev.mods.isPopupMenu() || ev.mods.isRightButtonDown();

        // 空槽：左键直接新建；右键给最小菜单（新建 / 显示全部通道）
        if (slot == mb::ui::kSlotEmpty)
        {
            if (wantMenu) showEmptySlotMenu();
            else if (onActivateSlot) onActivateSlot (index_);
            return;
        }
        if (slot == mb::ui::kSlotHidden) return;

        // 右键：按「鼠标所在区域」只显示该区域自己的菜单。
        // 插件行 > 插件区 > 输入区 > 输出区；其余（推子 / 电平 / 头部 /
        // 按钮格 / 名字条）统一给通用菜单。
        if (wantMenu)
        {
            if (faderDragging_) return;       // 拖推子时长按被系统翻译成右键
            for (const auto& hit : pluginRowHits_)
                if (hit.rect.contains (p)) { showPluginMenu (hit.index); return; }
            if (pluginRegion_.contains (p)) { showPluginRegionMenu();  return; }
            if (inputRegion_.contains (p))  { openSource (true);    return; }
            if (outputRegion_.contains (p)) { openSource (false);   return; }
            showGeneralMenu();
            return;
        }

        // 插件区滚动条：按住滑块拖动浏览；点击轨道空白把滑块吸附到点击处
        if (! pluginScrollTrack_.isEmpty() && pluginScrollTrack_.contains (p))
        {
            pluginScrollDrag_ = true;
            pluginScrollDragStartY_ = p.y;
            if (! pluginScrollThumb_.contains (p))
                scrollPluginsToPoint (p.y);
            pluginScrollDragStartPos_ = pluginScroll_;
            return;
        }

        // 顶部颜色点现已改为「仅指示」（换色入口在底部色块），不再响应点击
        if (inputAdd_.contains (p))     { openSource (true);  return; }
        if (outputAdd_.contains (p))    { openSource (false); return; }
        if (pluginAdd_.contains (p))    { openPlugins(); return; }

        // 已添加的输入/输出连接行：整行单击 = 打开选择浮层（可切换/移除），
        // 行尾「×」= 直接移除
        for (const auto& h : sourceHits_)
            if (h.rect.contains (p))
            {
                if (! h.removeRect.isEmpty() && h.removeRect.contains (p))
                {
                    graph_.removeEdge (h.edge.src, h.edge.dst, h.edge.kind);
                    changed (mb::tr ("已移除连接", "Connection removed"));
                }
                else
                {
                    openSource (h.input);
                }
                return;
            }

        // 插件行左键：行首小圆点 = 旁通 / 取消旁通；点击插件名（行其余部分）
        // = 打开插件界面。双击名区会先来两次 mouseDown（打开界面幂等，无副作用），
        // 双击圆点的两次 toggle 正好互相抵消，不会误开界面或错乱状态。
        for (const auto& hit : pluginRowHits_)
            if (hit.rect.contains (p))
            {
                for (const auto& dot : pluginDotHits_)
                    if (dot.rect.contains (p))
                    {
                        togglePluginBypass (dot.index);
                        return;
                    }
                if (onOpenPlugin) onOpenPlugin (index_, hit.index);
                return;
            }

        // 底部名称条：左侧色块单击换色（双击名称在 mouseDoubleClick 里重命名）；
        // 不再从这里发起左右拖动换位（换位保留在通道电平 / 空白区域）。
        if (nameBand_.contains (p))
        {
            if (colourSwatch_.contains (p)) showColourMenu (colourSwatch_.getCentre());
            return;
        }

        // 区域空白处按住：横向拖动或长按后左右拖动 = 移动通道；
        // 纵向调高只在区域「顶部边缘」生效（见 regionEdgeAt）
        dragMode_ = DragMode::Pending;
        dragStartX_ = p.x;
        dragStartY_ = p.y;
        dragStartTime_ = juce::Time::getMillisecondCounter();
        dragChannelPos_ = index_;
        resizeRegion_ = regionEdgeAt (p);             // 仅顶边命中才可调高
        const auto L = computeLayout();
        const int hs[] = { L.hIn, L.hPl, L.hOut, L.hFd };
        dragStartRegionH_ = resizeRegion_ >= 0 ? hs[resizeRegion_] : 0;
    }

    void mouseDrag (const juce::MouseEvent& ev) override
    {
        // 插件区滑块拖动：独立于 dragMode_（按下时未进入通道重排 / 调高流程）
        if (pluginScrollDrag_ && ev.eventComponent == this)
        {
            const float maxScroll = static_cast<float> (juce::jmax (0, pluginRowsTotal_ - pluginRowsVisible_));
            const float trackH = pluginScrollTrack_.getHeight();
            if (maxScroll > 0.0f && trackH > 0.0f)
            {
                const float thumbH = juce::jmax (16.0f, trackH * pluginRowsVisible_
                                                       / static_cast<float> (juce::jmax (1, pluginRowsTotal_)));
                const float dy = ev.position.y - pluginScrollDragStartY_;
                pluginScroll_ = juce::jlimit (0.0f, maxScroll,
                                              pluginScrollDragStartPos_
                                                + dy / juce::jmax (1.0f, trackH - thumbH) * maxScroll);
                invalidate();
            }
            return;
        }

        // 推子转发的 drag：只用来标记「用户正在拖推子」，不参与通道重排 / 区域调高
        // （转发的坐标是 Slider 局部的）。长按被系统翻译成右键时据此抑制菜单。
        if (ev.eventComponent == &fader_)
        {
            faderDragging_ = true;
            return;
        }
        if (ev.eventComponent != this || dragMode_ == DragMode::None) return;

        if (dragMode_ == DragMode::Pending)
        {
            const float dx = ev.position.x - dragStartX_;
            const float dy = ev.position.y - dragStartY_;
            const bool longPress = juce::Time::getMillisecondCounter() - dragStartTime_ >= 320;

            // 主轴判定：横向为主 → 移动通道；纵向为主 → 调区域高度。
            // 长按（>=320ms）后即便只有很小的横向位移也进入移动模式。
            if (std::abs (dx) > 6.0f && std::abs (dx) >= std::abs (dy))
                dragMode_ = DragMode::Reorder;
            else if (std::abs (dy) > 6.0f)
                dragMode_ = DragMode::Resize;
            else if (longPress && std::abs (dx) >= 2.0f)
                dragMode_ = DragMode::Reorder;
            else return;
        }

        if (dragMode_ == DragMode::Reorder)
        {
            // 第一次真正移动才开启特效层：单击 / 双击名字条不会闪一下浮层
            if (! dragNotified_) beginDrag (ev.position.x, ev.position.y);

            // 隐藏通道不占可见槽位，因此要先把「可见序号」映射回真实通道索引。
            const float globalX = ev.position.x
                                + static_cast<float> (getBoundsInParent().getX());
            const int slots = onVisibleCount ? juce::jmax (1, onVisibleCount())
                                             : graph_.channelCount();
            const int visSlot = juce::jlimit (0, slots - 1,
                juce::roundToInt (globalX / static_cast<float> (mb::ui::kStripWidth)));
            if (onDragMove) onDragMove (globalX, ev.position.y);

            const int target = onSlotToChannel ? onSlotToChannel (visSlot) : visSlot;
            if (target >= 0 && target < graph_.channelCount()
                && target != dragChannelPos_ && onReorder)
            {
                onReorder (dragChannelPos_, target);
                if (onDragFlash) onDragFlash (dragVisSlot_, visSlot);
                dragVisSlot_ = visSlot;
                dragChannelPos_ = target;
            }
            return;
        }

        static constexpr int minH[] = { 34, 44, 34, 120 };
        if (resizeRegion_ >= 0)
        {
            mb::ui::regionHeight (resizeRegion_)
                = juce::jlimit (minH[resizeRegion_], 600,
                                dragStartRegionH_ + juce::roundToInt (ev.position.y - dragStartY_));
            resized();
            invalidate();                    // 区域高度变了 → 静态层重建
        }
    }

    /** 把插件区滑块吸附到轨道上的目标 y（点击轨道空白 / 拖动共用的换算）。 */
    void scrollPluginsToPoint (float trackY)
    {
        const float maxScroll = static_cast<float> (juce::jmax (0, pluginRowsTotal_ - pluginRowsVisible_));
        const float trackH = pluginScrollTrack_.getHeight();
        if (maxScroll <= 0.0f || trackH <= 0.0f) return;
        const float thumbH = juce::jmax (16.0f, trackH * pluginRowsVisible_
                                               / static_cast<float> (juce::jmax (1, pluginRowsTotal_)));
        const float target = juce::jlimit (pluginScrollTrack_.getY() + thumbH * 0.5f,
                                           pluginScrollTrack_.getBottom() - thumbH * 0.5f,
                                           trackY);
        pluginScroll_ = (target - pluginScrollTrack_.getY() - thumbH * 0.5f)
                      / juce::jmax (1.0f, trackH - thumbH) * maxScroll;
        invalidate();
    }

    /** 滚轮悬停在插件区上：滚动插件列表（溢出时），其余区域交给基类。 */
    void mouseWheelMove (const juce::MouseEvent& ev, const juce::MouseWheelDetails& wheel) override
    {
        if (pluginRegion_.contains (ev.position)
            && pluginRowsTotal_ > pluginRowsVisible_ && pluginRowsVisible_ > 0)
        {
            const float maxScroll = static_cast<float> (pluginRowsTotal_ - pluginRowsVisible_);
            const float step = 2.0f;    // 每格滚动 2 行
            const float dir = wheel.isReversed ? 1.0f : -1.0f;
            const float before = pluginScroll_;
            pluginScroll_ = juce::jlimit (0.0f, maxScroll, pluginScroll_ + dir * wheel.deltaY * step);
            if (pluginScroll_ != before) invalidate();
            return;
        }
        Component::mouseWheelMove (ev, wheel);
    }

    void mouseUp (const juce::MouseEvent& ev) override
    {
        faderPointerDown_ = false;
        faderDragging_ = false;
        pluginScrollDrag_ = false;
        if (ev.eventComponent != this) return;
        if (dragNotified_)
        {
            dragNotified_ = false;
            if (onDragEnd) onDragEnd();
        }
        dragMode_ = DragMode::None;
    }

    /** 真正进入「移动通道」时通知外层打开拖拽特效层（纵向调高不触发）。 */
    void beginDrag (float localX, float localY)
    {
        if (dragNotified_) return;
        dragNotified_ = true;
        const float gx = localX + static_cast<float> (getBoundsInParent().getX());
        const int slots = onVisibleCount ? juce::jmax (1, onVisibleCount())
                                         : graph_.channelCount();
        dragVisSlot_ = juce::jlimit (0, slots - 1,
            juce::roundToInt (gx / static_cast<float> (mb::ui::kStripWidth)));
        if (onDragBegin) onDragBegin (index_, gx, localY, dragVisSlot_);
    }

    void mouseMove (const juce::MouseEvent& ev) override
    {
        // 推子转发的 move 用的是 Slider 局部坐标，不能拿来改通道条光标
        if (ev.eventComponent != this)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        const auto slot = mb::ui::slotState (index_);
        if (slot == mb::ui::kSlotEmpty)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);  // 点击新建通道
            return;
        }
        if (slot != mb::ui::kSlotNormal)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        const auto p = ev.position;
        bool overHit = inputAdd_.contains (p) || outputAdd_.contains (p)
                    || pluginAdd_.contains (p);
        int sourceIdx = -1;
        for (int i = 0; i < static_cast<int> (sourceHits_.size()); ++i)
            if (sourceHits_[static_cast<size_t> (i)].rect.contains (p))
            { overHit = true; sourceIdx = i; break; }

        bool overPluginRow = false;
        int pluginIdx = -1;
        for (const auto& hit : pluginRowHits_)
            if (hit.rect.contains (p)) { overPluginRow = true; pluginIdx = hit.index; break; }

        auto cursor = juce::MouseCursor::NormalCursor;
        if (overHit || overPluginRow)
            cursor = juce::MouseCursor::PointingHandCursor;   // 可点击：切换 / 添加 / 开关插件
        else if (nameBand_.contains (p))
            cursor = juce::MouseCursor::PointingHandCursor;  // 底部名称条：换色 / 重命名（手指）
        else if (regionEdgeAt (p) >= 0)
            cursor = juce::MouseCursor::UpDownResizeCursor;     // 区域顶边：拖动调高
        setMouseCursor (cursor);

        // 悬停提示：插件行 = 「备注 (插件名)」；源区行 = 完整连接标签
        //（行内文字会被省略号截断，提示里始终可见完整的备注名与原名）
        juce::String tip;
        if (pluginIdx >= 0)
        {
            const auto note = engine_.pluginBank().pluginNote (index_, pluginIdx);
            const auto name = engine_.pluginBank().pluginName (index_, pluginIdx);
            tip = note.isEmpty() ? name : note + mb::S(" (") + name + mb::S(")");
        }
        else if (sourceIdx >= 0)
        {
            tip = edgeLabel (sourceHits_[static_cast<size_t> (sourceIdx)].edge);
        }
        setTooltip (tip);
    }

    void mouseDoubleClick (const juce::MouseEvent& ev) override
    {
        // 双击推子 → 恢复默认 0.0 dB（同步回调 sliderValueChanged 写回图，
        // 那里已触发 onChanged，此处只补提示消息，避免重复 syncDevices）
        if (ev.eventComponent == &fader_)
        {
            fader_.setValue (0.0, juce::sendNotificationSync);
            if (onMessage) onMessage (mb::tr ("推子已恢复 0.0 dB", "Fader reset to 0.0 dB"));
            repaint();
            return;
        }

        if (nameText_.contains (ev.position)) beginRename();

        // 双击插件行 → 打开插件编辑器窗口
        for (const auto& hit : pluginRowHits_)
            if (hit.rect.contains (ev.position))
            {
                if (onOpenPlugin) onOpenPlugin (index_, hit.index);
                return;
            }
    }

private:
    struct RowHit
    {
        juce::Rectangle<float> rect;
        int index = 0;
    };

    struct SourceHit
    {
        juce::Rectangle<float> rect;
        juce::Rectangle<float> removeRect;   // 行尾「×」：点击直接移除
        mb::RouteEdge edge;
        bool input = true;
    };

    static constexpr int kHeaderH     = 24;      // 头部高度
    static constexpr int kButtonGridH = 56;      // 底部按钮格高度（两行按钮 + 留白）

    // 通道色纵线：与四个卡片区左边缘同 x、同线宽；名字条左边缘也对齐到这里
    static constexpr float kSpineX = 4.0f;
    static constexpr float kSpineW = 1.5f;

    enum class DragMode { None, Pending, Resize, Reorder };

    struct RegionLayout { int yIn, hIn, yPl, hPl, yOut, hOut, yFd, hFd; };

    //==========================================================================
    // 四区布局：①输入（自动高度 = 剩余空间）②插件 ③输出 ④推子。
    // 插件/输出/推子高度会话内共享（mb::ui::regionHeight），拖动任一区域
    // 改变其高度，输入区吸收剩余空间。
    RegionLayout computeLayout() const
    {
        const int top = kHeaderH + 3;
        const int gridTop = getHeight() - mb::ui::kNameBandHeight - kButtonGridH;
        // 三个区域之间有 3 个 4px 间隙；预留它们，保证推子区底边落在按钮格上方 4px，
        // 不再与 M / S 按钮重叠。
        const int avail = juce::jmax (180, gridTop - 16 - top);

        auto stored = [] (int which) { return mb::ui::regionHeight (which); };
        auto grab = [&] (int which, int def, int minH)
        {
            const int s = stored (which);
            return juce::jlimit (minH, 600, s > 0 ? s : def);
        };

        int hFd  = grab (3, avail / 2, 120);
        int hPl  = grab (1, 100, 44);
        // 「+ 输入 / + 输出」默认等高：输出区未被手动拖动过时，与输入区对半
        // 分配剩余空间，避免输出区只有 48px 而输入区很高、两栏看起来不齐。
        // 用户拖过输出区后仍以存储值为准（输入区吸收剩余）。
        const int storedOut = stored (2);
        int hOut = storedOut > 0 ? juce::jlimit (34, 600, storedOut)
                                 : juce::jmax (34, (avail - hPl - hFd) / 2);
        int hIn  = avail - hPl - hOut - hFd;

        if (hIn < 34) { hFd  = juce::jmax (120, hFd  - (34 - hIn)); hIn = avail - hPl - hOut - hFd; }
        if (hIn < 34) { hPl  = juce::jmax (44,  hPl  - (34 - hIn)); hIn = avail - hPl - hOut - hFd; }
        if (hIn < 34) { hOut = juce::jmax (34,  hOut - (34 - hIn)); hIn = avail - hPl - hOut - hFd; }
        hIn = juce::jmax (34, hIn);

        RegionLayout l;
        l.yIn = top;               l.hIn = hIn;
        l.yPl = l.yIn + hIn + 4;   l.hPl = hPl;
        l.yOut = l.yPl + hPl + 4;  l.hOut = hOut;
        l.yFd = l.yOut + hOut + 4; l.hFd = hFd;
        return l;
    }

    int regionAt (const juce::Point<float>& p) const
    {
        if (inputRegion_.contains (p))  return 0;
        if (pluginRegion_.contains (p)) return 1;
        if (outputRegion_.contains (p)) return 2;
        if (faderRegion_.contains (p))  return 3;
        return -1;
    }

    /** 区域「顶部边缘」命中：返回可调高的区域号（插件/输出/推子），否则 -1。
        只有顶边 4px 内才显示调高光标、才允许拖动调高；区域内部一律箭头。
        输入区高度自适应（吸收剩余空间），其顶边不可调。 */
    int regionEdgeAt (const juce::Point<float>& p) const
    {
        const juce::Rectangle<float>* regs[] = { &inputRegion_, &pluginRegion_,
                                                 &outputRegion_, &faderRegion_ };
        for (int r = 1; r < 4; ++r)
        {
            const auto& reg = *regs[static_cast<size_t> (r)];
            if (p.x >= reg.getX() && p.x < reg.getRight()
                && p.y >= reg.getY() && p.y < reg.getY() + 4.0f)
                return r;
        }
        return -1;
    }

    //==========================================================================
    void drawHeader (juce::Graphics& g, int w)
    {
        // 顶部颜色点已全面移除（任何边框样式下都不再显示）：颜色指示由左侧纵线 /
        // 全边框 / 底部色块承担，换色入口在底部色块。通道编号直接靠左对齐。
        const int textX = 7;

        g.setFont (mb::ui::font (10.0f, true));
        g.setColour (mb::ui::text);
        g.drawText (mb::S("CH") + juce::String (index_ + 1), textX, 2, w - textX - 4, 20,
                    juce::Justification::centredLeft, false);

        // 向右对齐状态由右键菜单（勾选态）呈现与切换

        g.setColour (mb::ui::divider);
        g.fillRect (4, kHeaderH - 1, w - 8, 1);
    }

    /** 区域外框：卡片底 + 可选「≡ 标题」行。
        左侧通道色纵线由 drawSpine() 统一绘制，保证四个区域同 x / 同线宽。 */
    void drawRegionFrame (juce::Graphics& g, const juce::Rectangle<float>& r,
                          const juce::String& title, juce::Colour colour)
    {
        g.setColour (mb::ui::card.withAlpha (0.55f));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (mb::ui::border.withAlpha (0.7f));
        g.drawRoundedRectangle (r, 5.0f, 0.8f);

        if (title.isEmpty()) return;

        g.setFont (mb::ui::font (7.8f, true));
        g.setColour (colour);
        g.drawText (juce::String::fromUTF8 ("\xe2\x89\xa1 ") + title,
                    static_cast<int> (r.getX()) + 9, static_cast<int> (r.getY()) + 1,
                    static_cast<int> (r.getWidth()) - 14, 13,
                    juce::Justification::centredLeft, false);
    }

    /** 源区（输入/输出）：顶部「输入 +」按钮，下方逐行列出已添加的连接（点击行移除）。 */
    void drawSourceRegion (juce::Graphics& g, const juce::Rectangle<float>& region, bool input)
    {
        // 顶部「+」按钮
        const auto btn = juce::Rectangle<float> (region.getX() + 5.0f, region.getY() + 3.0f,
                                                 region.getWidth() - 10.0f, 16.0f);
        if (input) inputAdd_ = btn; else outputAdd_ = btn;

        // 「+ 输入 / + 输出」按钮与标题不再跟随通道色，统一主题黑白灰
        g.setColour (mb::ui::slotBg.withAlpha (0.85f));
        g.fillRoundedRectangle (btn, 3.0f);
        g.setColour (mb::ui::border.withAlpha (0.8f));
        g.drawRoundedRectangle (btn.reduced (0.5f), 3.0f, 0.8f);
        g.setFont (mb::ui::font (8.8f, true));
        g.setColour (mb::ui::text);
        g.drawText (input ? mb::S("+  ") + mb::tr ("输入", "Input")
                          : mb::S("+  ") + mb::tr ("输出", "Output"),
                    btn, juce::Justification::centred, false);

        // 已添加的连接列表
        float rowY = btn.getBottom() + 3.0f;
        constexpr float rowH = 15.0f;
        int shown = 0;
        for (const auto& e : collectEdges (input))
        {
            if (rowY + rowH > region.getBottom() - 2.0f) break;
            const auto row = juce::Rectangle<float> (region.getX() + 5.0f, rowY,
                                                     region.getWidth() - 10.0f, rowH - 1.0f);
            // 整行 = 单击切换设备；行尾 15px 的「×」= 直接移除该连接
            sourceHits_.push_back ({ row,
                                     juce::Rectangle<float> (row.getRight() - 15.0f, row.getY(),
                                                             15.0f, row.getHeight()),
                                     e, input });

            g.setColour (mb::ui::sectionBg.withAlpha (0.85f));
            g.fillRoundedRectangle (row, 3.0f);
            // 行首色条：通道互送（C→C）显示「对端通道」的颜色——
            // 输入行 = 来源通道色，输出行 = 送往的目标通道色；
            // 设备端口直连（P→C / C→P）用中性灰，不跟随通道色
            auto barColour = mb::ui::faintText;
            if (e.kind == mb::EdgeKind::ChannelToChannel)
                barColour = mb::ui::stripColour (input ? e.src : e.dst);
            g.setColour (barColour);
            g.fillRoundedRectangle (row.getX(), row.getY(), 2.5f, row.getHeight(), 1.2f);

            g.setFont (mb::ui::font (8.2f));
            g.setColour (mb::ui::text);
            g.drawText (edgeLabel (e), row.getX() + 7.0f, row.getY(),
                        row.getWidth() - 18.0f, row.getHeight(),
                        juce::Justification::centredLeft, true);

            g.setFont (mb::ui::font (8.5f));
            g.setColour (mb::ui::faintText);
            g.drawText (mb::S("\xc3\x97"), row.getRight() - 12.0f, row.getY(),
                        10.0f, row.getHeight(), juce::Justification::centred, false);
            rowY += rowH;
            ++shown;
        }

        const auto edges = collectEdges (input);
        if (edges.empty())
        {
            if (rowY + 12.0f < region.getBottom())
            {
                g.setFont (mb::ui::font (8.2f));
                g.setColour (mb::ui::faintText);
                g.drawText (mb::tr ("未连接", "Not connected"),
                            region.getX(), rowY, region.getWidth(), 12,
                            juce::Justification::centred, false);
            }
        }
        else if (shown < static_cast<int> (edges.size()))
        {
            g.setFont (mb::ui::font (7.2f));
            g.setColour (mb::ui::faintText);
            g.drawText (mb::S("+") + juce::String (static_cast<int> (edges.size()) - shown),
                        static_cast<int> (region.getX()) + 8,
                        static_cast<int> (region.getBottom()) - 13,
                        40, 11, juce::Justification::centredLeft, false);
        }
    }

    /** 插件区内容：第一行固定「内置效果器」，往下是 VST3 插件架 + 「+ 添加」。 */
    void drawInserts (juce::Graphics& g, const juce::Rectangle<float>& region)
    {
        if (region.getHeight() < 40.0f) return;

        const int rowH = 18;
        const int count = engine_.pluginBank().pluginCount (index_);

        // 「+ 插件」按钮：置于区域顶部，宽度随文本收紧（裁掉多余宽度）
        const auto addF = mb::ui::font (8.6f, true);
        const auto addTxt = mb::S("+ ") + mb::tr ("插件", "Plugin");
        juce::GlyphArrangement aga;
        aga.addLineOfText (addF, addTxt, 0.0f, 0.0f);
        const float addTw = aga.getNumGlyphs() > 0
                              ? aga.getBoundingBox (0, aga.getNumGlyphs(), true).getWidth() : 28.0f;
        // 与「+ 输入 / + 输出」按钮的文字起点对齐：源区按钮是整宽居中排版
        // （region.x+5、宽 region.w-10、字号 8.8），插件键帽保持按文本收紧的宽度，
        // 只平移 x 让两个「+」落在同一竖线上，避免「+ 插件」看起来贴左。
        const auto srcF = mb::ui::font (8.8f, true);
        juce::GlyphArrangement sga;
        sga.addLineOfText (srcF, mb::S("+  ") + mb::tr ("输入", "Input"), 0.0f, 0.0f);
        const float srcTw = sga.getNumGlyphs() > 0
                              ? sga.getBoundingBox (0, sga.getNumGlyphs(), true).getWidth() : 38.0f;
        const float srcTextX = region.getX() + 5.0f
                             + juce::jmax (0.0f, region.getWidth() - 10.0f - srcTw) * 0.5f;
        const float addW = juce::jmin (region.getWidth() - 12.0f, addTw + 16.0f);
        const float addX = juce::jlimit (region.getX() + 2.0f,
                                         region.getRight() - addW - 2.0f,
                                         srcTextX - (addW - addTw) * 0.5f);
        const auto add = juce::Rectangle<float> (addX, region.getY() + 2.0f,
                                                 addW, 14.0f);
        pluginAdd_ = add;
        // 物理键帽：投影 + 渐变帽体 + 顶缘高光，中性灰描边与文字（不跟随通道色）
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRoundedRectangle (add.translated (0.0f, 1.4f), 4.0f);
        g.setGradientFill (juce::ColourGradient (mb::ui::card.brighter (0.10f), 0.0f, add.getY(),
                                                 mb::ui::slotBg.darker (0.32f), 0.0f, add.getBottom(), false));
        g.fillRoundedRectangle (add, 4.0f);
        g.setColour (mb::ui::border.withAlpha (0.80f));
        g.drawRoundedRectangle (add.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawLine (add.getX() + 4.0f, add.getY() + 1.0f, add.getRight() - 4.0f, add.getY() + 1.0f);
        g.setFont (addF);
        g.setColour (mb::ui::text);
        g.drawText (addTxt, add, juce::Justification::centred, false);

        float rowY = region.getY() + 19.0f;

        // —— 插件行：自带插件与第三方 VST3 统一列出（圆点颜色区分）——
        const float listBottom = region.getBottom() - 4.0f;
        const float listH = listBottom - rowY;
        const int visibleRows = juce::jmax (0, static_cast<int> (listH / rowH));
        const int maxScroll = juce::jmax (0, count - visibleRows);
        pluginScroll_ = juce::jlimit (0.0f, static_cast<float> (maxScroll), pluginScroll_);
        pluginRowsTotal_ = count;                  // 供滚轮 / 拖滑块在 paint 外换算
        pluginRowsVisible_ = visibleRows;
        const bool overflow = maxScroll > 0;
        const float sliderW = overflow ? 9.0f : 0.0f;
        const float contentRight = region.getRight() - 4.0f - sliderW;
        pluginScrollTrack_ = juce::Rectangle<float>();
        pluginScrollThumb_ = juce::Rectangle<float>();
        if (overflow && listH > 20.0f)             // 区域过小不设滑块（画不下也点不到）
            pluginScrollTrack_ = juce::Rectangle<float> (region.getRight() - 12.0f, rowY,
                                                         8.0f, listH);

        // 视口裁剪：滚动后行只画在列表区内（顶部 / 底部半行截断）
        juce::Rectangle<int> listClip (static_cast<int> (region.getX()),
                                       static_cast<int> (rowY),
                                       static_cast<int> (region.getWidth()),
                                       static_cast<int> (listH) + 1);
        g.saveState();
        g.reduceClipRegion (listClip);

        const int first = static_cast<int> (pluginScroll_);
        const float frac = pluginScroll_ - static_cast<float> (first);
        float y = rowY - frac * static_cast<float> (rowH);
        int drawn = 0;
        for (int i = first; i < count && drawn < visibleRows; ++i, ++drawn, y += static_cast<float> (rowH))
        {
            const bool isBuiltIn = engine_.pluginBank().pluginBuiltInId (index_, i) > 0;
            const bool bypass = engine_.pluginBank().pluginBypass (index_, i);
            const bool offline = engine_.pluginBank().pluginOffline (index_, i);
            const auto note = engine_.pluginBank().pluginNote (index_, i);
            const auto name = engine_.pluginBank().pluginName (index_, i);

            // 行宽随内容收紧：圆点 + 文本（旁通时额外留出 BYP 标签），裁掉多余宽度
            const auto rowF = mb::ui::font (8.6f, ! bypass && ! offline);
            const auto label = note.isEmpty() ? name
                             : note + mb::S(" (") + name + mb::S(")");
            juce::GlyphArrangement rga;
            rga.addLineOfText (rowF, label, 0.0f, 0.0f);
            const float tw = rga.getNumGlyphs() > 0
                               ? rga.getBoundingBox (0, rga.getNumGlyphs(), true).getWidth() : 0.0f;
            const float bypW = (bypass && ! offline) ? 30.0f : 0.0f;
            // 行宽要把「完整文本」算进去：旧写法行宽只留 17+tw+8，绘制时又一律扣 30，
            // 中文三字名（如「播放器」）末尾会被削掉一个字，只显示「播放」。
            const float textW = tw + 2.0f;
            const float maxRowW = juce::jmax (1.0f, contentRight - region.getX() - 6.0f);
            const float rowW = juce::jlimit (juce::jmin (44.0f, maxRowW), maxRowW,
                                             17.0f + textW + bypW + 8.0f);
            const auto row = juce::Rectangle<float> (region.getX() + 6.0f, y,
                                                     rowW, rowH - 1.0f);
            pluginRowHits_.push_back ({ row, i });

            // 物理键帽行：投影 + 渐变帽体 + 顶缘高光 + 细边框
            g.setColour (juce::Colours::black.withAlpha (0.30f));
            g.fillRoundedRectangle (row.translated (0.0f, 1.2f), 3.0f);
            g.setGradientFill (juce::ColourGradient (mb::ui::card.brighter (0.08f), 0.0f, row.getY(),
                                                     mb::ui::slotBg.darker (0.26f), 0.0f, row.getBottom(), false));
            g.fillRoundedRectangle (row, 3.0f);
            g.setColour (mb::ui::border.withAlpha (0.9f));
            g.drawRoundedRectangle (row.reduced (0.5f), 3.0f, 0.8f);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawLine (row.getX() + 3.0f, row.getY() + 1.0f, row.getRight() - 3.0f, row.getY() + 1.0f);

            // 旁通圆点：独立热区（点击圆点 = 旁通开关，点击行其余部分 = 打开界面）
            const auto dot = juce::Rectangle<float> (row.getX() + 5.0f,
                                                     row.getCentreY() - 4.0f, 8.0f, 8.0f);
            pluginDotHits_.push_back ({ dot, i });
            if (offline)
            {
                // 离线：空心灰点
                g.setColour (mb::ui::border);
                g.drawEllipse (dot.reduced (0.5f), 1.0f);
            }
            else
            {
                g.setColour (bypass ? mb::ui::faintText
                                    : (isBuiltIn ? mb::ui::accent[0] : mb::ui::meterGreen));
                g.fillEllipse (dot);
                g.setColour (mb::ui::border);
                g.drawEllipse (dot.reduced (0.5f), 0.8f);
            }

            // 有备注时显示「备注 (插件名)」，与设备备注同一格式；
            // 自带 / VST3 标签已移除，整行空间留给插件名（右侧仅留旁通提示）
            g.setFont (rowF);
            g.setColour (bypass || offline ? mb::ui::faintText : mb::ui::text);
            g.drawText (label,
                        row.getX() + 17.0f, row.getY(),
                        juce::jmax (0.0f, row.getWidth() - 17.0f - bypW - 4.0f), row.getHeight(),
                        juce::Justification::centredLeft, false);

            if (bypass && ! offline)
            {
                g.setFont (mb::ui::font (7.2f));
                g.setColour (mb::ui::meterYellow);
                g.drawText (mb::tr ("旁通", "BYP"), row.getRight() - 30.0f, row.getY(),
                            26.0f, row.getHeight(), juce::Justification::centredRight, false);
            }
        }
        g.restoreState();

        if (count == 0)
        {
            g.setFont (mb::ui::font (9.0f));
            g.setColour (mb::ui::faintText);
            g.drawText (mb::tr ("尚未添加插件", "No plugins added"),
                        region.getX(), rowY, region.getWidth(),
                        static_cast<int> (listBottom - rowY),
                        juce::Justification::centred, false);
        }

        // —— 右侧滚动条：插件超出可视行数时出现，拖动 / 滚轮浏览 ——
        if (overflow && pluginScrollTrack_.getHeight() > 20.0f)
        {
            const float trackH = pluginScrollTrack_.getHeight();
            const float thumbH = juce::jmax (16.0f, trackH * visibleRows / static_cast<float> (count));
            const float ty = pluginScrollTrack_.getY()
                           + (trackH - thumbH) * (pluginScroll_ / static_cast<float> (maxScroll));
            pluginScrollThumb_ = juce::Rectangle<float> (pluginScrollTrack_.getX() + 1.5f, ty,
                                                         5.0f, thumbH);
            g.setColour (mb::ui::track.withAlpha (0.6f));
            g.fillRoundedRectangle (pluginScrollTrack_, 3.5f);
            g.setColour (mb::ui::faintText.withAlpha (0.9f));
            g.fillRoundedRectangle (pluginScrollThumb_, 2.5f);
        }
    }

    std::vector<mb::RouteEdge> collectEdges (bool input) const
    {
        std::vector<mb::RouteEdge> out;
        for (const auto& e : graph_.edges())
        {
            if (input)
            {
                if (e.kind == mb::EdgeKind::InputToChannel && e.dst == index_) out.push_back (e);
                else if (e.kind == mb::EdgeKind::ChannelToChannel && e.dst == index_) out.push_back (e);
            }
            else
            {
                if (e.kind == mb::EdgeKind::ChannelToOutput && e.src == index_) out.push_back (e);
                else if (e.kind == mb::EdgeKind::ChannelToChannel && e.src == index_) out.push_back (e);
            }
        }
        return out;
    }

    juce::String edgeLabel (const mb::RouteEdge& e) const
    {
        switch (e.kind)
        {
            case mb::EdgeKind::InputToChannel:
                return catalog_.portLabel (true, e.src);

            case mb::EdgeKind::ChannelToOutput:
                return catalog_.portLabel (false, e.dst);

            case mb::EdgeKind::ChannelToChannel:
            {
                const int other = e.src == index_ ? e.dst : e.src;
                auto t = mb::S("CH") + juce::String (other + 1) + mb::S("  ")
                       + mb::channelDisplayName (other);
                if (e.side != -1 || graph_.isStereo (e.src) || graph_.isStereo (e.dst))
                    t += e.side == 0 ? mb::S("  · L")          // 声道侧标记
                       : e.side == 1 ? mb::S("  · R")
                                     : mb::S("  · L+R");
                return t;
            }
        }
        return {};
    }

    //==========================================================================
    /** 电平表端口布局：按「输入设备」判定而非端口凑对。
        通道输入线来自同一立体声设备的 L+R → 立体声两条；
        否则每条输入线一条（电平表最多画两条，取最先连入的）。 */
    void meterPorts (int& portA, int& portB, bool& stereoPair) const
    {
        portA = portB = -1;
        stereoPair = false;

        std::vector<int> ports;
        for (const auto& e : graph_.edges())
            if (e.kind == mb::EdgeKind::InputToChannel && e.dst == index_)
                ports.push_back (e.src);
        if (ports.empty()) return;

        const auto& ips = engine_.catalog().inputPorts();
        auto info = [&ips] (int port) -> const mb::PortInfo*
        { return port >= 0 && port < static_cast<int> (ips.size()) ? &ips[static_cast<size_t> (port)] : nullptr; };

        // 同一设备的 L(0) + R(1) 都连入 → 立体声对
        for (int a : ports)
            for (int b : ports)
            {
                if (a == b) continue;
                const auto* ia = info (a);
                const auto* ib = info (b);
                if (ia != nullptr && ib != nullptr
                    && ia->deviceIndex == ib->deviceIndex
                    && ia->channelIndex == 0 && ib->channelIndex == 1)
                {
                    portA = a; portB = b;
                    stereoPair = true;
                    return;
                }
            }

        portA = ports.front();                  // 单输入线：一条电平（该端口峰值）
    }

    /** 推子区：顶部居中 dB 读数 + 左侧参考值刻度 + 电平表 + 推子。 */
    void drawFaderCard (juce::Graphics& g, const juce::Rectangle<float>& region)
    {
        const int y = juce::roundToInt (region.getY());
        const int cardH = juce::roundToInt (region.getHeight());

        g.setColour (mb::ui::card);
        g.fillRoundedRectangle (region, 6.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (region, 6.0f, 1.0f);
        // 左侧色线由 drawSpine() 统一绘制（与其余三区同为 1.5px），此处不再单独画粗线

        // dB 读数：通道顶部水平居中
        const bool on = graph_.isEnabled (index_);
        g.setFont (mb::ui::font (13.0f, true));
        g.setColour (on ? mb::ui::text : mb::ui::faintText);
        g.drawText (juce::String (mb::ui::gainToDb (graph_.fader (index_)), 1) + mb::S(" dB"),
                    region.getX(), y + 2, region.getWidth(), 17,
                    juce::Justification::centred, false);

        const int faderTop = y + 40;         // dB 读数(2..19) + 声像条(21..34) 之下
        const int faderH = cardH - 56;
        if (faderH < 40) return;

        // 左侧参考值刻度（电平表与推子共用同一 dB 纵轴；与内置插件共用绘制函数）
        const float dbs[] = { 12.0f, 0.0f, -6.0f, -12.0f, -24.0f, -48.0f, -60.0f };
        for (const float db : dbs)
        {
            const float yy = static_cast<float> (faderTop)
                           + static_cast<float> (faderH) * (12.0f - db) / 72.0f;
            mb::ui::drawFaderRefTick (g, 0.0f, yy,
                                      juce::String (static_cast<int> (db)), db == 0.0f);
        }

        // 电平表：LED 分段 + 峰值保持。同一立体声设备的 L+R 两条，
        // 单输入线一条；推子后表显——输入端口峰值乘通道推子增益，
        // 与通道总峰值（引擎已乘推子）取大者，拉推子电平即时跟随。
        const bool silent = graph_.isMuted (index_) || ! on;
        const float fg = graph_.fader (index_);
        int pa = -1, pb = -1;
        bool stereoPair = false;
        meterPorts (pa, pb, stereoPair);
        const int barW = 9, gap = 3;
        const int meterX = 20;

        auto drawBar = [&] (int x, float peak, int holdIdx)
        {
            const float level = silent ? 0.0f : mb::ui::levelToMeter (peak);
            const auto meter = juce::Rectangle<float> (static_cast<float> (x),
                                                       static_cast<float> (faderTop),
                                                       static_cast<float> (barW),
                                                       static_cast<float> (faderH));
            g.setColour (mb::ui::meterOff);
            g.fillRoundedRectangle (meter, 2.0f);

            constexpr float segH = 4.0f;
            const int segs = juce::jmax (1, static_cast<int> (faderH / segH));
            for (int i = 0; i < segs; ++i)
            {
                const float segLevel = static_cast<float> (i + 1) / static_cast<float> (segs);
                if (segLevel > level + 1.0e-4f) break;
                const auto seg = juce::Rectangle<float> (
                    meter.getX() + 1.5f, meter.getBottom() - (i + 1) * segH + 1.0f,
                    barW - 3.0f, segH - 1.5f);
                g.setColour (segLevel > 0.86f ? mb::ui::meterRed
                           : (segLevel > 0.70f ? mb::ui::meterYellow : mb::ui::meterGreen));
                g.fillRoundedRectangle (seg, 1.0f);
            }

            // 峰值保持线（缓慢下落）
            auto& hold = peakHold_[static_cast<size_t> (holdIdx)];
            hold = juce::jmax (level, hold - 0.0035f);
            if (hold > 0.01f)
            {
                const float hy = meter.getBottom() - meter.getHeight() * hold;
                g.setColour (hold > 0.86f ? mb::ui::meterRed
                           : (hold > 0.70f ? mb::ui::meterYellow
                                           : juce::Colours::white.withAlpha (0.8f)));
                g.fillRect (meter.getX() + 1.0f, hy - 1.0f, meter.getWidth() - 2.0f, 2.0f);
            }
        };

        const float totalPeak = engine_.channelPeak (index_);
        if (mb::ui::isChannelStereo (index_))
        {
            // 立体声通道：L/R 分别读通道缓冲峰值（引擎已含推子/效果/互送），
            // 条数与建通道时选的制式一致；即使未连输入（或来自互送）也双条显示。
            drawBar (meterX, engine_.channelPeakL (index_), 0);
            drawBar (meterX + barW + gap, engine_.channelPeakR (index_), 1);
            g.setFont (mb::ui::font (6.5f, true));
            g.setColour (mb::ui::faintText);
            g.drawText (mb::S("L"), meterX - 1, faderTop + faderH + 2, barW + 2, 8,
                        juce::Justification::centred, false);
            g.drawText (mb::S("R"), meterX + barW + gap - 1, faderTop + faderH + 2, barW + 2, 8,
                        juce::Justification::centred, false);
        }
        else if (pa >= 0)
        {
            // 单声道通道：永远单条（跟随通道制式）。双通道设备接入时内核已把
            // L+R 求和进通道 L 缓冲，单条即通道真实内容，不再按端口显示双条。
            drawBar (meterX + 5, juce::jmax (engine_.inputPortPeak (pa) * fg, totalPeak), 0);
        }
        else
        {
            drawBar (meterX + 5, totalPeak, 0);
        }
    }

    /** 底部按钮格：独立底色，让 M/S 与 R/喇叭 两行明确落在自己的区域内。 */
    void drawButtonGrid (juce::Graphics& g, int y, int w)
    {
        g.setColour (mb::ui::card.withAlpha (0.32f));
        g.fillRect (0, y, w, kButtonGridH);
        g.setColour (mb::ui::divider);
        g.fillRect (4, y, w - 8, 1);

        // 快捷键与链接角标由 linkBadge_ 组件绘制（R 与喇叭之间的空档）
    }

    /** 通道色纵线：所有线段统一在 x = kSpineX、线宽 kSpineW(1.5px)，
        与四个卡片区左边缘、底部彩色名字条左边缘严格对齐。

        · 分段（默认）：圆点 → 输入区一段；插件 / 输出各一段（间隙断开）；
          推子区一段不再收口，向下穿过 M/S 按钮格直接连到彩色名字条顶边。
        · 边框：左侧 + 顶部 + 右侧的贯通轮廓，左线落到名字条左上角。
    */
    void drawSpine (juce::Graphics& g, int w, int h)
    {
        const float t = kSpineW;
        const float bandTop = static_cast<float> (h - mb::ui::kNameBandHeight);

        if (mb::ui::channelBorderMode() == 2)            // 全边框：左 + 上 + 右
        {
            g.setColour (accent_.withAlpha (graph_.isEnabled (index_) ? 0.85f : 0.30f));
            g.fillRect (0.0f, 0.0f, t, bandTop);                        // 左
            g.fillRect (0.0f, 0.0f, static_cast<float> (w), t);          // 上
            g.fillRect (static_cast<float> (w) - t, 0.0f, t, bandTop);   // 右
            return;
        }

        if (mb::ui::channelBorderMode() == 0)            // 无颜色边框：不画任何纵线
            return;

        const float x = kSpineX;
        g.setColour (accent_.withAlpha (0.95f));

        // ① 头部颜色圆点 → 输入区顶：起点埋进圆点左侧，视觉上「线从圆点长出」
        const float dotY = 10.0f;                       // dot = (2, 4, 14, 14)
        const float inTop = inputRegion_.getY() + 1.0f;
        if (inTop > dotY)
            g.fillRect (x, dotY, t, inTop - dotY);

        // ② 输入 / 插件 / 输出：独立短段，上下各留 1px，区域间隙保持断开
        const juce::Rectangle<float>* segments[] =
            { &inputRegion_, &pluginRegion_, &outputRegion_ };
        for (const auto* r : segments)
            g.fillRect (x, r->getY() + 1.0f, t, r->getHeight() - 2.0f);

        // ③ 推子区：上留 1px，向下贯穿按钮格，一直连到彩色名字条顶边
        const float from = faderRegion_.getY() + 1.0f;
        if (bandTop > from)
            g.fillRect (x, from, t, bandTop - from);
    }

    void drawNameBand (juce::Graphics& g, int y, int h, int w)
    {
        // 左边缘对齐：全边框模式对齐左框线 x=0，其余模式对齐分段纵线 kSpineX
        const float left = mb::ui::channelBorderMode() == 2 ? 0.0f : kSpineX;
        const auto band = juce::Rectangle<float> (left, static_cast<float> (y),
                                                  static_cast<float> (w) - left,
                                                  static_cast<float> (h));
        // 整条命中热区（光标 / 重命名基准）；底部本身就是「颜色区域」
        nameBand_ = juce::Rectangle<float> (0.0f, static_cast<float> (y),
                                            static_cast<float> (w), static_cast<float> (h));
        // 左侧换色按钮（单击换色）：贴左缘、上下铺满；选中图标后按钮加宽，
        // 图标画进按钮内部（白色，随名称），名称紧跟按钮之后
        const float swW = iconId_.isNotEmpty() ? 28.0f : 13.0f;
        colourSwatch_ = juce::Rectangle<float> (left, static_cast<float> (y),
                                               swW, static_cast<float> (h));
        nameText_ = juce::Rectangle<float> (left + swW + 7.0f, static_cast<float> (y),
                                            static_cast<float> (w) - left - swW - 7.0f,
                                            static_cast<float> (h));

        // 整条底色 = 通道色（即「颜色区域」），让换色入口一目了然
        g.setColour (accent_);
        g.fillRect (band);

        // 左侧色块：提亮 + 描边，提示「这里可点开取色」
        g.setColour (accent_.brighter (0.18f));
        g.fillRoundedRectangle (colourSwatch_, 3.0f);
        g.setColour (mb::ui::border.withAlpha (0.6f));
        g.drawRoundedRectangle (colourSwatch_, 3.0f, 1.0f);

        // 色块与名称之间的分隔，明确两个可点击区
        g.setColour (mb::ui::border.withAlpha (0.5f));
        g.drawLine (colourSwatch_.getRight() + 1.0f, static_cast<float> (y) + 3.0f,
                    colourSwatch_.getRight() + 1.0f, static_cast<float> (y + h) - 3.0f);

        // 图标绘制在按钮内居中（矢量自绘，白色与名称同色）
        if (iconId_.isNotEmpty())
        {
            const float pad = juce::jmax (0.0f, (static_cast<float> (h) - 14.0f) / 2.0f);
            const auto iconR = juce::Rectangle<float> (
                                       colourSwatch_.getX() + (swW - 15.0f) / 2.0f,
                                       static_cast<float> (y),
                                       15.0f, static_cast<float> (h))
                                    .reduced (0.0f, pad);
            mb::ui::ChannelIcon::draw (g, iconId_, iconR, juce::Colours::white);
        }

        g.setFont (mb::ui::font (9.0f, true));
        g.setColour (juce::Colours::white);
        g.drawText (mb::channelDisplayName (index_),
                    nameText_.toNearestInt(), juce::Justification::centredLeft, false);
    }

    /** 空槽占位：仅在空槽仍被显示时绘制（默认布局已把空槽收起，不占可见位置）。 */
    void drawEmptySlot (juce::Graphics& g, int w, int h)
    {
        const float bandY = static_cast<float> (h - mb::ui::kNameBandHeight);
        const auto box = juce::Rectangle<float> (10.0f, 38.0f,
                                                 static_cast<float> (w - 20),
                                                 bandY - 50.0f);
        g.setColour (mb::ui::card.withAlpha (0.35f));
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (mb::ui::border.withAlpha (0.85f));
        g.drawRoundedRectangle (box, 6.0f, 1.0f);

        g.setFont (mb::ui::font (26.0f, true));
        g.setColour (mb::ui::faintText);
        g.drawText (mb::S("+"),
                    juce::Rectangle<int> (0, static_cast<int> (box.getCentreY()) - 30, w, 36),
                    juce::Justification::centred, false);
        g.setFont (mb::ui::font (9.0f));
        g.drawText (mb::tr ("新建通道", "New channel"),
                    juce::Rectangle<int> (0, static_cast<int> (box.getCentreY()) + 4, w, 18),
                    juce::Justification::centred, false);

        g.setColour (mb::ui::slotBg);
        g.fillRect (0.0f, bandY, static_cast<float> (w),
                    static_cast<float> (mb::ui::kNameBandHeight));
        g.setFont (mb::ui::font (9.0f, true));
        g.setColour (mb::ui::faintText);
        g.drawText (mb::tr ("＋ 空槽", "＋ Empty"), 0, static_cast<int> (bandY),
                    w, mb::ui::kNameBandHeight, juce::Justification::centred, false);
    }

    //==========================================================================
    bool hasInputEdge (int port) const
    {
        for (const auto& e : graph_.edges())
            if (e.kind == mb::EdgeKind::InputToChannel && e.dst == index_ && e.src == port)
                return true;
        return false;
    }

    bool hasCCEdge (int src, int dst) const
    {
        for (const auto& e : graph_.edges())
            if (e.kind == mb::EdgeKind::ChannelToChannel && e.src == src && e.dst == dst)
                return true;
        return false;
    }

    bool hasOutputEdge (int port) const
    {
        for (const auto& e : graph_.edges())
            if (e.kind == mb::EdgeKind::ChannelToOutput && e.src == index_ && e.dst == port)
                return true;
        return false;
    }

    /** 菜单条目短名：通道对备注优先（备注替代通道名），
        否则剥掉设备名前缀，只显示通道名（L / R / Input 3 …）。 */
    juce::String portShortName (const mb::PortInfo& p) const
    {
        const auto cn = catalog_.channelNoteForPort (p);
        if (cn.isNotEmpty()) return cn;
        if (p.channelName.isNotEmpty()) return p.channelName;
        juce::String s = p.name;
        if (p.deviceName.isNotEmpty() && s.startsWith (p.deviceName))
            s = s.substring (p.deviceName.length());
        s = s.trimCharactersAtStart (mb::S(" \u00B7\u3000 "));
        return s.isEmpty() ? p.name : s;
    }

    /** 打开「输入源 / 输出目标」选择界面：优先走 SourcePicker 浮层，
        未接线（独立使用 / 测试）时退回旧的多级菜单。 */
    void openSource (bool input)
    {
        if (onOpenSource) { onOpenSource (index_, input); return; }
        showSourceMenu (input);
    }

    void showSourceMenu (bool input)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (input ? mb::tr ("输入源 · 点击条目可移除", "Inputs · click to remove")
                                     : mb::tr ("输出目标 · 点击条目可移除", "Outputs · click to remove"));

        const auto edges = collectEdges (input);
        const int removeBase = 100;
        for (size_t i = 0; i < edges.size(); ++i)
            menu.addItem (removeBase + static_cast<int> (i),
                          mb::tr ("移除  ", "Remove  ") + edgeLabel (edges[i]));
        if (! edges.empty()) menu.addSeparator();

        if (input)
        {
            // 输入设备按设备分组：每个设备一个子菜单，内含单声道端口与立体声对。
            // （设备设置页「辅助输入设备」可多选，未勾选的设备不在这里出现）
            menu.addSectionHeader (mb::tr ("输入设备", "Input devices"));
            const auto& ports = catalog_.inputPorts();
            juce::StringArray devNames;
            std::vector<juce::PopupMenu> devSubs;
            auto subFor = [&] (const juce::String& t) -> juce::PopupMenu&
            {
                int i = devNames.indexOf (t);
                if (i < 0) { devNames.add (t); devSubs.emplace_back(); i = static_cast<int> (devSubs.size()) - 1; }
                return devSubs[static_cast<size_t> (i)];
            };
            int added = 0;
            for (int p = 0; p < static_cast<int> (ports.size()); ++p)
            {
                if (! catalog_.portVisible (true, p)) continue;
                auto& sub = subFor (ports[static_cast<size_t> (p)].deviceName);
                if (! hasInputEdge (p))
                {
                    sub.addItem (1000 + p, portShortName (ports[static_cast<size_t> (p)]));
                    ++added;
                }
                // 立体声 = 同一设备相邻 L/R 端口对（偶数 p 与 p+1）成对接入
                if (p % 2 == 0 && p + 1 < static_cast<int> (ports.size())
                    && ! hasInputEdge (p) && ! hasInputEdge (p + 1))
                {
                    // 通道对有备注时只显示一次备注名，避免「备注 + 备注」
                    const auto cn = catalog_.channelNoteForPort (ports[static_cast<size_t> (p)]);
                    sub.addItem (8000 + p, cn.isNotEmpty() ? cn
                                       : portShortName (ports[static_cast<size_t> (p)])
                                       + mb::S(" + ") + portShortName (ports[static_cast<size_t> (p + 1)]));
                    ++added;
                }
            }
            for (int i = 0; i < devNames.size(); ++i)
                menu.addSubMenu (devNames[i], devSubs[static_cast<size_t> (i)], true);
            if (added == 0)
                menu.addItem (-1, mb::tr ("（全部已连接）", "(all connected)"), false);

            // 其他通道折叠为子菜单，保持顶层清爽
            menu.addSeparator();
            juce::PopupMenu chSub;
            int channels = 0;
            for (int c = 0; c < graph_.channelCount(); ++c)
            {
                if (c == index_ || hasCCEdge (c, index_)) continue;
                chSub.addItem (3000 + c, mb::S("CH") + juce::String (c + 1) + mb::S("   ")
                                         + mb::channelDisplayName (c));
                ++channels;
            }
            menu.addSubMenu (mb::tr ("其他通道作输入源", "Other channels as input"),
                             chSub, channels > 0);
            if (channels == 0)
                menu.addItem (-2, mb::tr ("（无可连接通道）", "(no channels available)"), false);
        }
        else
        {
            // 输出设备按驱动类型分组（WASAPI / ASIO …），条目用通道短名
            menu.addSectionHeader (mb::tr ("输出设备", "Output devices"));
            const auto& ports = catalog_.outputPorts();
            int added = 0;
            juce::StringArray groups;
            std::vector<juce::PopupMenu> subs;
            auto subFor = [&] (const juce::String& t) -> juce::PopupMenu&
            {
                int i = groups.indexOf (t);
                if (i < 0) { groups.add (t); subs.emplace_back(); i = static_cast<int> (subs.size()) - 1; }
                return subs[static_cast<size_t> (i)];
            };
            for (size_t p = 0; p < ports.size(); ++p)
            {
                if (! catalog_.portVisible (false, static_cast<int> (p))) continue;
                if (hasOutputEdge (static_cast<int> (p))) continue;
                subFor (ports[p].typeName).addItem (5000 + static_cast<int> (p), portShortName (ports[p]));
                ++added;
            }
            for (int i = 0; i < groups.size(); ++i)
                menu.addSubMenu (groups[i], subs[static_cast<size_t> (i)], true);
            if (added == 0)
                menu.addItem (-1, mb::tr ("（全部已连接）", "(all connected)"), false);

            menu.addSeparator();
            juce::PopupMenu chSub;
            int channels = 0;
            for (int c = 0; c < graph_.channelCount(); ++c)
            {
                if (c == index_ || hasCCEdge (index_, c)) continue;
                chSub.addItem (7000 + c, mb::S("CH") + juce::String (c + 1) + mb::S("   ")
                                         + mb::channelDisplayName (c));
                ++channels;
            }
            menu.addSubMenu (mb::tr ("送进其他通道作输入源", "Feed another channel"),
                             chSub, channels > 0);
            if (channels == 0)
                menu.addItem (-2, mb::tr ("（无可连接通道）", "(no channels available)"), false);
        }

        juce::Component::SafePointer<ChannelStrip> safe (this);
        menu.showMenuAsync (mb::ui::menuOptionsFor (*this),
            [safe, input, edges, removeBase] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                if (result >= removeBase && result < removeBase + static_cast<int> (edges.size()))
                {
                    const auto& e = edges[static_cast<size_t> (result - removeBase)];
                    self->graph_.removeEdge (e.src, e.dst, e.kind);
                    self->changed (mb::tr ("已移除连接", "Connection removed"));
                    return;
                }

                mb::RouteEdge e { -1, self->index_, mb::EdgeKind::InputToChannel, 1.0f };
                if (result >= 8000)     // 立体声对：p 与 p+1 成对接入
                {
                    const int p = result - 8000;
                    self->addEdge ({ p, self->index_, mb::EdgeKind::InputToChannel, 1.0f });
                    self->addEdge ({ p + 1, self->index_, mb::EdgeKind::InputToChannel, 1.0f });
                    return;
                }
                if (result >= 7000)      { e.src = self->index_; e.dst = result - 7000; e.kind = mb::EdgeKind::ChannelToChannel; }
                else if (result >= 5000) { e.src = self->index_; e.dst = result - 5000; e.kind = mb::EdgeKind::ChannelToOutput; }
                // 输入菜单选「其他通道」作输入源：src=对方通道，dst=本通道。
                // 旧代码只写 dst，src 留着初始的 -1，validate 以
                // "source channel out of range" 拒绝——表现为「添加不成功」。
                else if (result >= 3000) { e.src = result - 3000; e.dst = self->index_; e.kind = mb::EdgeKind::ChannelToChannel; }
                else if (result >= 1000) { e.src = result - 1000; e.kind = mb::EdgeKind::InputToChannel; }
                else return;
                if (! input && e.kind == mb::EdgeKind::InputToChannel) return;
                if (input && e.kind == mb::EdgeKind::ChannelToOutput) return;
                self->addEdge (e);
            });
    }

    void addEdge (const mb::RouteEdge& e)
    {
        std::string why;
        if (graph_.tryAddEdge (e, why))
            changed (mb::tr ("已建立连接", "Connection added"));
        else
            changed (mb::tr ("连接被拒绝：", "Rejected: ") + juce::String (why));
    }

    //==========================================================================
    // 菜单 ID：同一菜单树内必须全局唯一（子菜单项 ID 也会回到 showMenuAsync 回调）
    enum MenuId : int
    {
        kAddInput = 1, kAddPlugin = 2, kAddOutput = 3, kRename = 4,
        kChangeColour = 130,                   // 打开统一「更换颜色」页面
        kRemoveConnBase = 200,                 // 200.. 移除已建立连接
        kHide = 300, kShowAll = 301,
        kRevealBase = 400,                    // 400.. 显示某条隐藏通道
        kDelete = 500,
        kDuplicate = 600,                     // 复制通道
        kAlignRight = 601,                    // 向右对齐（勾选态）
        kLinkChannel = 602,                   // 快捷键与链接…
        kPluginRemove = 900, kPluginOpen = 901, kPluginBypass = 902,
        kPluginOffline = 903, kPluginNote = 904, kPluginHotkey = 905, kPluginReveal = 906,
        kSlotNew = 950,
        kPluginItemBase = 2000                // 2000.. 插件区右键里每条插件的子菜单（每条 8 个槽位）
    };

    /** 打开统一「更换颜色」页面（九宫格预设 + 同页自定义 RGB）。
        菜单回调 / 鼠标回调里都可能销毁菜单组件，统一延后一帧并用 SafePointer。 */
    void openColourPage()
    {
        juce::Component::SafePointer<ChannelStrip> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (auto* self = safe.getComponent())
                if (self->onPickCustomColour) self->onPickCustomColour (self->index_);
        });
    }

    /** 通用菜单（推子 / 电平 / 头部 / 按钮格 / 名字条右键）：
        去掉了属于各区域自己的「添加输入 / 添加插件 / 添加输出」，只保留
        与整条通道有关的操作，另加「复制通道」「向右对齐」。 */
    void appendGeneralMenuItems (juce::PopupMenu& m)
    {
        m.addItem (kDuplicate,  mb::tr ("复制通道", "Duplicate channel"));
        m.addItem (kAlignRight,
                   mb::ui::isChannelRightAligned (index_)
                       ? mb::tr ("取消向右对齐", "Remove right alignment")
                       : mb::tr ("向右对齐", "Align right"),
                   true, mb::ui::isChannelRightAligned (index_));   // 勾选态呈现
        m.addSeparator();

        ctxEdges_.clear();
        const auto inEdges  = collectEdges (true);
        const auto outEdges = collectEdges (false);
        if (! inEdges.empty() || ! outEdges.empty())
        {
            juce::PopupMenu rm;
            int id = kRemoveConnBase;
            for (const auto& e : inEdges)
            {
                ctxEdges_.push_back (e);
                rm.addItem (id++, mb::tr ("移除  ", "Remove  ") + edgeLabel (e));
            }
            for (const auto& e : outEdges)
            {
                ctxEdges_.push_back (e);
                rm.addItem (id++, mb::tr ("移除  ", "Remove  ") + edgeLabel (e));
            }
            m.addSubMenu (mb::tr ("移除连接…", "Remove connection…"), rm, true);
        }

        m.addItem (kChangeColour, mb::tr ("颜色与图标…", "Colour & icon…"));

        m.addSeparator();
        {
            const auto link = mb::links::primaryOf (index_);
            const auto hk   = mb::links::hotkeyText (link);
            const auto code = mb::links::codeString (link);
            const auto suffix = hk.isNotEmpty() ? mb::S("  (") + hk + mb::S(")")
                               : code.isNotEmpty() ? mb::S("  (") + code + mb::S(")")
                                                   : juce::String();
            m.addItem (kLinkChannel, mb::tr ("快捷键与链接…", "Hotkeys & links…") + suffix);
        }

        m.addSeparator();
        m.addItem (kRename, mb::tr ("重命名通道", "Rename channel"));
        m.addItem (kHide,   mb::tr ("隐藏通道", "Hide channel"));

        juce::PopupMenu reveal;
        int hidden = 0;
        for (int c = 0; c < graph_.channelCount(); ++c)
            if (mb::ui::slotState (c) == mb::ui::kSlotHidden)
            {
                reveal.addItem (kRevealBase + c,
                                mb::S("CH") + juce::String (c + 1) + mb::S("  ·  ")
                                    + mb::channelDisplayName (c));
                ++hidden;
            }
        if (hidden > 0)
        {
            reveal.addSeparator();
            reveal.addItem (kShowAll, mb::tr ("显示全部通道", "Show all channels"));
            m.addSubMenu (mb::tr ("显示已隐藏通道", "Show hidden channels"), reveal, true);
        }
        else
            m.addItem (kShowAll, mb::tr ("显示全部通道", "Show all channels"), false);

        m.addSeparator();
        m.addItem (kDelete, mb::tr ("删除通道", "Delete channel"));
    }

    /** 所有右键菜单共用的结果分发。 */
    void handleMenuResult (int result)
    {
        if (result == kAddInput)  { openSource (true);  return; }
        if (result == kAddOutput) { openSource (false); return; }
        if (result == kAddPlugin) { openPlugins();          return; }
        if (result == kRename)    { beginRename();          return; }
        if (result == kChangeColour) { openColourPage();    return; }
        if (result == kDelete)    { if (onDeleteChannel) onDeleteChannel (index_); return; }
        if (result == kDuplicate)
        {
            if (onDuplicateChannel) onDuplicateChannel (index_);
            return;
        }
        if (result == kAlignRight)
        {
            if (onSetRightAligned)
                onSetRightAligned (index_, ! mb::ui::isChannelRightAligned (index_));
            return;
        }
        if (result == kLinkChannel) { if (onLinkChannel) onLinkChannel (index_, -1); return; }
        if (result == kHide)      { if (onSetHidden) onSetHidden (index_, true);   return; }
        if (result == kShowAll)   { if (onShowAllChannels) onShowAllChannels();    return; }
        if (result >= kRevealBase && result < kRevealBase + 32)
        {
            if (onSetHidden) onSetHidden (result - kRevealBase, false);
            return;
        }
        if (result >= kRemoveConnBase && result < kRemoveConnBase + 256)
        {
            const auto i = static_cast<size_t> (result - kRemoveConnBase);
            if (i < ctxEdges_.size())
            {
                graph_.removeEdge (ctxEdges_[i].src, ctxEdges_[i].dst, ctxEdges_[i].kind);
                changed (mb::tr ("已移除连接", "Connection removed"));
            }
            return;
        }
    }

    //==========================================================================
    // 插件开 / 关：VST3 走 PluginRack 的 bypass；自带插件没有
    // AudioPluginInstance，音频实际由内置效果链 FxParams 的模块开关控制，
    // 两者必须同步写，否则 UI 显示「旁通」但声音还在（或反过来）。
    void applyPluginBypass (int idx, bool bypass)
    {
        auto& bank = engine_.pluginBank();
        if (idx < 0 || idx >= bank.pluginCount (index_)) return;
        bank.setBypass (index_, idx, bypass);

        const auto bid = bank.pluginBuiltInId (index_, idx);
        if (bid > 0)
        {
            auto params = engine_.fxParams (index_);
            mb::applyBuiltInToParams (params, bid, ! bypass);   // on = 未旁通
            engine_.setFxParams (index_, params);
        }
    }

    /** 旁通开关（行首圆点单击 / 菜单 / 全局快捷键共用）。 */
    void togglePluginBypass (int idx)
    {
        const auto name = engine_.pluginBank().pluginName (index_, idx);
        const bool wasBypassed = engine_.pluginBank().pluginBypass (index_, idx);
        applyPluginBypass (idx, ! wasBypassed);
        changed ((wasBypassed ? mb::tr ("已启用插件：", "Plugin enabled: ")
                              : mb::tr ("已旁通插件：", "Plugin bypassed: "))
                 + name);
    }

    //==========================================================================
    // 插件行右键：该插件自己的操作，固定顺序：
    // 旁通 → 离线 → 打开界面 → 备注 → 快捷键与链接 → 查看目录 → 删除
    void showPluginMenu (int idx)
    {
        auto& bank    = engine_.pluginBank();
        const auto name    = bank.pluginName (index_, idx);
        const bool bypass  = bank.pluginBypass (index_, idx);
        const bool offline = bank.pluginOffline (index_, idx);
        const bool builtIn = bank.pluginBuiltInId (index_, idx) > 0;
        const auto note    = bank.pluginNote (index_, idx);

        juce::PopupMenu menu;
        menu.addSectionHeader (note.isEmpty() ? name : note + mb::S(" (") + name + mb::S(")"));
        menu.addItem (kPluginBypass, bypass ? mb::tr ("启用插件", "Enable plugin")
                                            : mb::tr ("旁通插件", "Bypass plugin"));
        menu.addItem (kPluginOffline, offline ? mb::tr ("取消离线", "Bring online")
                                              : mb::tr ("离线插件", "Take offline"), ! builtIn);
        menu.addItem (kPluginOpen, mb::tr ("打开插件界面", "Open editor window"), ! offline);
        menu.addItem (kPluginNote, mb::tr ("添加备注", "Add note…"));
        // 复用通道级「快捷键与链接」弹窗：链接码是通道粒度，
        // 「效果」码控制整条插件链（内置效果 + VST3）的旁通联动。
        menu.addItem (kPluginHotkey, mb::tr ("快捷键与链接", "Hotkey & link"));
        menu.addItem (kPluginReveal, mb::tr ("查看目录", "Reveal in folder"), ! builtIn);
        menu.addSeparator();
        menu.addItem (kPluginRemove, mb::tr ("删除插件", "Remove plugin"));

        juce::Component::SafePointer<ChannelStrip> safe (this);
        menu.showMenuAsync (mb::ui::menuOptionsFor (*this),
            [safe, idx] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                auto& bank = self->engine_.pluginBank();
                if (idx >= bank.pluginCount (self->index_)) return;

                if (result == kPluginRemove)
                {
                    if (self->onRemovePlugin) self->onRemovePlugin (self->index_, idx);
                    self->changed (mb::tr ("已删除插件", "Plugin removed"));
                    return;
                }
                if (result == kPluginOpen)
                {
                    if (self->onOpenPlugin) self->onOpenPlugin (self->index_, idx);
                    return;
                }
                if (result == kPluginBypass)
                {
                    // 与圆点单击走同一套写入（含自带插件效果链开关同步）
                    self->togglePluginBypass (idx);
                    return;
                }
                if (result == kPluginOffline)
                {
                    self->togglePluginOffline (idx);
                    return;
                }
                if (result == kPluginNote)
                {
                    self->showPluginNoteEditor (idx);
                    return;
                }
                if (result == kPluginHotkey)
                {
                    // 插件入口：意图是控制整条效果链，默认预选「效果」动作
                    if (self->onLinkChannel) self->onLinkChannel (self->index_, mb::links::kFx);
                    return;
                }
                if (result == kPluginReveal)
                {
                    const auto f = juce::File (bank.pluginIdentifier (self->index_, idx));
                    if (f.existsAsFile())           f.revealToUser();
                    else if (f.isDirectory())       f.startAsProcess();
                    else self->changed (mb::tr ("插件文件不存在", "Plugin file not found"));
                    return;
                }
            });
    }

    //==========================================================================
    // 离线切换：离线 = 卸载实例释放资源（状态暂存，取消离线时原样回灌）。
    // 自带插件没有独立实例，不走离线（菜单里已禁用）。
    void togglePluginOffline (int idx)
    {
        auto& bank = engine_.pluginBank();
        if (idx < 0 || idx >= bank.pluginCount (index_)) return;
        const auto name = bank.pluginName (index_, idx);

        if (! bank.pluginOffline (index_, idx))
        {
            // 先经 MainComponent 关闭编辑器窗口（实例即将退役，窗口持有裸指针）
            if (onPluginOffline) onPluginOffline (index_, idx);
            if (! bank.markOffline (index_, idx)) return;
            invalidate();
            changed (mb::tr ("已离线插件：", "Plugin offline: ") + name);
            return;
        }

        // 取消离线：按 identifier（退化按名字）重建实例
        const auto ident = bank.pluginIdentifier (index_, idx);
        auto desc = engine_.pluginCatalog().findByIdentifier (ident);
        if (desc.name.isEmpty()) desc = engine_.pluginCatalog().findByName (name);
        if (desc.name.isEmpty())
        {
            changed (mb::tr ("未找到插件文件，无法取消离线：", "Plugin file not found: ") + name);
            return;
        }
        const double sr = engine_.sampleRate();
        const int    bs = engine_.bufferSizeSamples();
        juce::String err;
        auto inst = engine_.pluginCatalog().create (desc, err, sr, bs);
        if (inst == nullptr)
        {
            changed (mb::tr ("插件加载失败：", "Plugin load failed: ")
                     + (err.isNotEmpty() ? err : name));
            return;
        }
        const auto why = bank.restoreOffline (index_, idx, std::move (inst), sr, bs);
        if (why.isNotEmpty()) { changed (mb::tr ("插件恢复失败：", "Restore failed: ") + why); return; }
        invalidate();
        changed (mb::tr ("已恢复插件：", "Plugin restored: ") + name);
    }

    //==========================================================================
    // 插件备注：异步输入框（JUCE_MODAL_LOOPS_PERMITTED=0），保存后行内
    // 立即按「备注 (插件名)」重绘，随会话持久化。
    void showPluginNoteEditor (int idx)
    {
        auto& bank = engine_.pluginBank();
        if (idx < 0 || idx >= bank.pluginCount (index_)) return;
        const auto name = bank.pluginName (index_, idx);

        auto* aw = new juce::AlertWindow (
            mb::tr ("插件备注", "Plugin note"), name, juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("note", bank.pluginNote (index_, idx),
                           mb::tr ("备注（显示为「备注 (插件名)」，清空即恢复）",
                                   "Note (shown as \"note (plugin)\"; empty = none)"));
        aw->addButton (mb::tr ("保存", "Save"),   1);
        aw->addButton (mb::tr ("取消", "Cancel"), 0);

        juce::Component::SafePointer<ChannelStrip> safe (this);
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create ([safe, idx, aw] (int r)
            {
                if (r == 1 && safe != nullptr)
                {
                    if (auto* ed = aw->getTextEditor ("note"))
                    {
                        auto& bank = safe->engine_.pluginBank();
                        if (idx < bank.pluginCount (safe->index_))
                        {
                            bank.setPluginNote (safe->index_, idx, ed->getText());
                            safe->invalidate();
                            safe->changed (mb::tr ("已更新插件备注", "Plugin note updated"));
                        }
                    }
                }
            }),
            true);
    }

    /** 插件区空白处右键：只列插件相关操作（添加 + 每条插件的独立子菜单）。 */
    void showPluginRegionMenu()
    {
        auto& bank = engine_.pluginBank();
        juce::PopupMenu m;
        m.addSectionHeader (mb::S("CH") + juce::String (index_ + 1)
                            + mb::S("  ·  ") + mb::tr ("插件", "PLUGINS"));
        m.addItem (kAddPlugin, mb::tr ("添加插件…", "Add plugin…"));

        const int count = bank.pluginCount (index_);
        if (count > 0)
        {
            m.addSeparator();
            m.addSectionHeader (mb::tr ("已添加的插件", "INSTALLED PLUGINS"));
            for (int i = 0; i < count; ++i)
            {
                const bool builtIn = bank.pluginBuiltInId (index_, i) > 0;
                const bool offline = bank.pluginOffline (index_, i);
                const bool bypass  = bank.pluginBypass (index_, i);
                juce::PopupMenu sub;
                sub.addItem (kPluginItemBase + i * 8,     mb::tr ("打开界面", "Open editor"), ! offline);
                sub.addItem (kPluginItemBase + i * 8 + 1, bypass ? mb::tr ("启用插件", "Enable plugin")
                                                                 : mb::tr ("旁通插件", "Bypass plugin"));
                sub.addItem (kPluginItemBase + i * 8 + 2, offline ? mb::tr ("取消离线", "Bring online")
                                                                  : mb::tr ("离线插件", "Take offline"),
                            ! builtIn);
                sub.addItem (kPluginItemBase + i * 8 + 3, mb::tr ("添加备注", "Add note…"));
                sub.addItem (kPluginItemBase + i * 8 + 4, mb::tr ("快捷键与链接", "Hotkey & link"));
                sub.addItem (kPluginItemBase + i * 8 + 5, mb::tr ("查看目录", "Reveal in folder"), ! builtIn);
                sub.addSeparator();
                sub.addItem (kPluginItemBase + i * 8 + 6, mb::tr ("删除插件", "Remove plugin"));
                const auto note = bank.pluginNote (index_, i);
                const auto name = bank.pluginName (index_, i);
                m.addSubMenu (note.isEmpty() ? name : note + mb::S(" (") + name + mb::S(")"),
                              sub, true);
            }
        }
        else
        {
            m.addItem (-1, mb::tr ("（尚未添加插件）", "(no plugins)"), false);
        }

        juce::Component::SafePointer<ChannelStrip> safe (this);
        m.showMenuAsync (mb::ui::menuOptionsFor (*this),
            [safe] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                if (result == kAddPlugin) { self->showAddPluginMenu(); return; }
                if (result < kPluginItemBase) return;
                const int idx = (result - kPluginItemBase) / 8;
                const int act = (result - kPluginItemBase) % 8;
                auto& bank = self->engine_.pluginBank();
                if (idx >= bank.pluginCount (self->index_)) return;

                switch (act)
                {
                    case 0:
                        if (self->onOpenPlugin) self->onOpenPlugin (self->index_, idx);
                        break;
                    case 1:
                        self->togglePluginBypass (idx);
                        break;
                    case 2:
                        self->togglePluginOffline (idx);
                        break;
                    case 3:
                        self->showPluginNoteEditor (idx);
                        break;
                    case 4:
                        // 插件入口：意图是控制整条效果链，默认预选「效果」动作
                        if (self->onLinkChannel) self->onLinkChannel (self->index_, mb::links::kFx);
                        break;
                    case 5:
                    {
                        const auto f = juce::File (bank.pluginIdentifier (self->index_, idx));
                        if (f.existsAsFile())     f.revealToUser();
                        else if (f.isDirectory()) f.startAsProcess();
                        else self->changed (mb::tr ("插件文件不存在", "Plugin file not found"));
                        break;
                    }
                    case 6:
                        if (self->onRemovePlugin) self->onRemovePlugin (self->index_, idx);
                        self->changed (mb::tr ("已删除插件", "Plugin removed"));
                        break;
                    default:
                        break;
                }
            });
    }

    /** 通用菜单（原「完整通道菜单」已按区域拆分，这里只保留通道级操作）。 */
    void showGeneralMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader (mb::S("CH") + juce::String (index_ + 1)
                            + mb::S("  ·  ") + mb::channelDisplayName (index_));
        appendGeneralMenuItems (m);

        juce::Component::SafePointer<ChannelStrip> safe (this);
        m.showMenuAsync (mb::ui::menuOptionsFor (*this),
            [safe] (int result)
            {
                if (auto* self = safe.getComponent()) self->handleMenuResult (result);
            });
    }

    /** 空槽右键：只给「新建通道」与「显示全部通道」。 */
    void showEmptySlotMenu()
    {
        bool anyHidden = false;
        for (int c = 0; c < graph_.channelCount(); ++c)
            if (mb::ui::slotState (c) == mb::ui::kSlotHidden) { anyHidden = true; break; }

        juce::PopupMenu m;
        m.addItem (kSlotNew, mb::tr ("新建通道…", "New channel…"));
        m.addItem (kShowAll, mb::tr ("显示全部通道", "Show all channels"), anyHidden);

        juce::Component::SafePointer<ChannelStrip> safe (this);
        m.showMenuAsync (mb::ui::menuOptionsFor (*this),
            [safe] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                if (result == kSlotNew)
                {
                    if (self->onActivateSlot) self->onActivateSlot (self->index_);
                }
                else if (result == kShowAll)
                {
                    if (self->onShowAllChannels) self->onShowAllChannels();
                }
            });
    }

    /** 打开「添加插件」界面：优先走 PluginPicker 浮层（左自带 / 右第三方），
        MainComponent 未接线时退回旧式菜单。 */
    void openPlugins()
    {
        if (onOpenPlugins) { onOpenPlugins (index_); return; }
        showAddPluginMenu();
    }

public:
    /** 按 id 添加插件：1..kBuiltInPluginCount = 自带模块；1000+i = VST3 目录索引。
        PluginPicker 浮层与旧菜单共用此入口（MainComponent 也会回调）。 */
    void addPluginById (int result)
    {
        // 自带插件：登记到插件链并同步内置效果链开关
        if (result >= 1 && result <= mb::kBuiltInPluginCount)
        {
            auto params = engine_.fxParams (index_);
            mb::applyBuiltInToParams (params, result, true);
            engine_.setFxParams (index_, params);
            const auto err = engine_.pluginBank().addBuiltIn (index_, result);
            changed (err.isEmpty()
                 ? mb::tr ("已添加插件：", "Plugin added: ")
                   + mb::builtInPluginName (result)
                 : err);
            return;
        }

        if (result < 1000) return;
        const auto desc = engine_.pluginCatalog().get (result - 1000);
        juce::String err;
        auto inst = engine_.pluginCatalog().create (
            desc, err, engine_.sampleRate(), engine_.bufferSizeSamples());
        if (inst == nullptr)
        {
            changed (mb::tr ("插件加载失败：", "Plugin load failed: ") + err);
            return;
        }
        err = engine_.pluginBank().addPlugin (index_, std::move (inst),
                                              engine_.sampleRate(),
                                              engine_.bufferSizeSamples(),
                                              nullptr, desc.fileOrIdentifier);
        changed (err.isEmpty()
                 ? mb::tr ("已添加插件：", "Plugin added: ") + desc.name
                 : mb::tr ("插件加载失败：", "Plugin load failed: ") + err);
    }

    void showAddPluginMenu()
    {
        const auto& cat = engine_.pluginCatalog();
        const auto& bank = engine_.pluginBank();

        juce::PopupMenu menu;
        // —— 自带插件（内置效果链模块，无重复添加）——
        menu.addSectionHeader (mb::tr ("内置插件", "Built-in plugins"));
        for (const auto& p : mb::builtInPluginTable)
        {
            bool added = false;
            for (int i = 0; i < bank.pluginCount (index_); ++i)
                if (bank.pluginBuiltInId (index_, i) == p.id) { added = true; break; }
            menu.addItem (p.id, mb::S("  ") + mb::builtInPluginName (p.id)
                              + (added ? mb::tr ("  ·  已添加", "  ·  added") : juce::String()),
                          ! added);
        }

        // —— 第三方插件（VST3 扫描库，按厂商分组）——
        if (cat.count() > 0)
        {
            menu.addSeparator();
            menu.addSectionHeader (mb::tr ("第三方插件 · VST3", "Third-party plugins · VST3"));
            const int base = 1000;
            juce::StringArray vendors;
            std::vector<juce::PopupMenu> subs;
            for (int i = 0; i < cat.count(); ++i)
            {
                const auto d = cat.get (i);
                const auto vendor = d.manufacturerName.isNotEmpty() ? d.manufacturerName
                                                                    : mb::tr ("其他", "Other");
                int vi = vendors.indexOf (vendor);
                if (vi < 0) { vendors.add (vendor); subs.emplace_back(); vi = static_cast<int> (subs.size()) - 1; }
                subs[static_cast<size_t> (vi)].addItem (base + i, d.name);
            }
            for (int i = 0; i < vendors.size(); ++i)
                menu.addSubMenu (vendors[i], subs[static_cast<size_t> (i)], true);
        }

        juce::Component::SafePointer<ChannelStrip> safe (this);
        menu.showMenuAsync (mb::ui::menuOptionsFor (*this),
            [safe] (int result)
            {
                if (auto* self = safe.getComponent())
                    self->addPluginById (result);
            });
    }

    //==========================================================================
    // 颜色：底部色块单击与右键「更换颜色…」统一打开同一张取色页面
    void showColourMenu (juce::Point<float> = {}) { openColourPage(); }

    void beginRename()  { beginRenameWith (mb::channelDisplayName (index_)); }

    void beginRenameWith (const juce::String& initial)
    {
        if (nameEditor_ != nullptr) return;
        renaming_ = true;

        nameEditor_ = std::make_unique<juce::TextEditor>();
        nameEditor_->setText (initial, false);
        nameEditor_->setSelectAllWhenFocused (true);
        nameEditor_->setJustification (juce::Justification::centred);
        nameEditor_->setIndents (4, 0);
        nameEditor_->onReturnKey  = [this] { commitRename(); };
        nameEditor_->onEscapeKey  = [this] { closeRename(); };
        nameEditor_->onFocusLost  = [this] { if (renaming_) commitRename(); };
        addAndMakeVisible (nameEditor_.get());
        auto band = nameText_;
        if (band.isEmpty())    // 尚未绘制过（新建后立即命名）：按布局兜底定位
            band = juce::Rectangle<float> (30.0f,
                                           static_cast<float> (getHeight() - mb::ui::kNameBandHeight),
                                           juce::jmax (1.0f, static_cast<float> (getWidth()) - 34.0f),
                                           static_cast<float> (mb::ui::kNameBandHeight));
        nameEditor_->setBounds (band.toNearestInt());
        nameEditor_->grabKeyboardFocus();
    }

    void commitRename()
    {
        if (nameEditor_ == nullptr || ! renaming_) return;
        renaming_ = false;
        const auto t = nameEditor_->getText().trim();

        auto& names = mb::customChannelNames();
        while (names.size() <= index_) names.add (juce::String());
        // 留空不写空串：保持名字表与通道一一对应（空串会退回按位置取名，
        // 删除中间通道时会让名字整体错位）
        names.set (index_, t.isNotEmpty() ? t : mb::channelName (index_));

        closeRename();
        changed (mb::tr ("已重命名", "Renamed"));
    }

    void closeRename()
    {
        renaming_ = false;
        if (nameEditor_ == nullptr) return;
        removeChildComponent (nameEditor_.get());
        nameEditor_.reset();
    }

    void sliderValueChanged (juce::Slider* slider) override
    {
        if (slider != &fader_) return;
        const float gain = mb::ui::dbToGain (static_cast<float> (fader_.getValue()));
        graph_.setFader (index_, gain);
        if (onLinkVolume) onLinkVolume (index_, gain);   // 同码音量链接：推子联动
        if (onChanged) onChanged();
        repaint();
    }

    void changed (const juce::String& message)
    {
        if (onMessage) onMessage (message);
        if (onChanged) onChanged();
        invalidate();                        // 通用变更（连接增删等）→ 静态层重建
    }

    //==========================================================================
    int index_;
    mb::RouteGraph& graph_;
    mb::AudioEngine& engine_;
    mb::DeviceCatalog& catalog_;
    juce::Colour accent_;
    juce::String iconId_;                        // 名称左侧预设图标（空串 = 默认无图标）

    FaderLookAndFeel lf_;
    juce::Slider fader_;
    PanBar panBar_;               // 推子卡内声像条（双击归中）
    RoundButton muteBtn_, soloBtn_, recBtn_, spkBtn_;
    LinkBadgeButton linkBadge_;   // 快捷键与链接角标（键盘图标 / 快捷键 / 链接码）
    RoundButton monoBtn_;         // 头部「声道制式」切换（L+R = 立体声 / L = 单声道）
    float peakHold_[2] = { 0.0f, 0.0f };                 // 电平峰值保持
    bool metersIdle_ = false;                            // 电平已静默且完成收尾重绘
    bool foldedCompact_ = false;                         // 窗口卷起：隐藏推子卡 / 电平

    std::vector<RowHit> pluginRowHits_;
    std::vector<RowHit> pluginDotHits_;               // 插件行首旁通圆点热区
    std::vector<SourceHit> sourceHits_;
    juce::Rectangle<float> inputAdd_, outputAdd_, pluginAdd_, colourDot_, nameBand_;
    juce::Rectangle<float> colourSwatch_, nameText_;   // 底部：左色块（单击换色）/ 右名称（双击重命名）
    juce::Rectangle<float> inputRegion_, pluginRegion_, outputRegion_, faderRegion_;

    // 插件区滚动：插件超出可视行数时右侧出现滑块（拖动 / 滚轮浏览）
    float pluginScroll_ = 0.0f;                    // 滚动行数（可含小数，绘制时取整+余量）
    int pluginRowsTotal_ = 0, pluginRowsVisible_ = 0;   // 最近一次绘制的行列快照
    juce::Rectangle<float> pluginScrollTrack_, pluginScrollThumb_;
    bool pluginScrollDrag_ = false;
    float pluginScrollDragStartY_ = 0.0f, pluginScrollDragStartPos_ = 0.0f;

    // 静态层缓存：状态变化时整层重画，20 Hz 电平刷新只 blit（见 paint/drawStatic）
    juce::Image staticCache_;
    bool staticDirty_ = true;

    DragMode dragMode_ = DragMode::None;
    int resizeRegion_ = -1, dragStartRegionH_ = 0, dragChannelPos_ = 0;
    int ctxRegion_ = -1;                          // 右键菜单所在区域
    std::vector<mb::RouteEdge> ctxEdges_;         // 右键菜单快照（输入/输出连接）
    float dragStartX_ = 0.0f, dragStartY_ = 0.0f;
    std::uint32_t dragStartTime_ = 0;            // 长按判定起点
    bool dragNotified_ = false;                   // 拖拽特效层是否已开启
    bool dragging_ = false;                       // 本条正被拖动（高亮描边）
    bool faderPointerDown_ = false;               // 推子上按下了主键（长按判定）
    bool faderDragging_ = false;                  // 正在拖推子 → 抑制长按误触右键
    int dragVisSlot_ = 0;                         // 上一次换位的目标可见槽位
    bool renaming_ = false;
    std::unique_ptr<juce::TextEditor> nameEditor_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
