#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "RouteGraph.h"

//==============================================================================
// 配线架主题：段落化卡片。视觉重心放在「输入源 → 通道 → 输出目标」三段连线上。
// 深色（默认）与浅色（#CBDDFE 淡蓝紫底）两套配色，运行时经 applyTheme 切换；
// 颜色在 paint 处实时读取，控件构造期捕获的由切主题方负责刷新。
//==============================================================================
namespace mb::ui
{
    inline constexpr int kStripWidth     = 84;
    // 窗口宽度由「抽屉 + 可见通道条」推导，不再固定 1280 留一大片空白。
    // 下限保证顶栏图标 / 驱动胶囊 / 状态面板仍能排开；上限避免超出屏幕。
    inline constexpr int kMinWindowWidth  = 520;
    inline constexpr int kMinWindowHeight = 480;
    inline constexpr int kMaxWindowWidth  = 3200;
    inline constexpr int kMaxWindowHeight = 1800;
    inline constexpr int kDefaultWindowHeight = 720;
    inline constexpr int kTopBarHeight   = 52;
    inline constexpr int kRibbonHeight   = 36;
    inline constexpr int kDrawerWidth    = 88;
    inline constexpr int kNameBandHeight = 22;
    inline constexpr int kFooterHeight   = 28;
    inline constexpr int kCardHeight     = 176;
    inline constexpr int kHeaderHeight   = 40;
    inline constexpr int kSectionTitleH  = 20;
    inline constexpr int kRowHeight      = 20;

    // ---- 主题（深色默认 / 浅色参考用户截图 #CBDDFE 淡蓝紫底）----------------
    // 颜色是运行时变量：applyTheme 一次性翻转，所有 paint 处读到的都是当前主题。
    // 注意：控件构造期 setColour 捕获的颜色不会自动跟随，切主题后由
    // MainComponent::setLightTheme 负责重建通道条 / 重刷常驻控件。
    inline bool& isDarkTheme() noexcept { static bool dark = true; return dark; }

    // Studio One 风格深色主题
    inline juce::Colour background   { 0xff141518 };
    inline juce::Colour stripOdd     { 0xff1d1f23 };
    inline juce::Colour stripEven    { 0xff222428 };
    inline juce::Colour card         { 0xff2a2c31 };
    inline juce::Colour topBar       { 0xff1a1b1e };
    inline juce::Colour ribbon       { 0xff191a1d };
    inline juce::Colour border       { 0xff35383e };
    inline juce::Colour divider      { 0xff2b2d32 };
    inline juce::Colour text         { 0xffe6e8eb };
    inline juce::Colour subText      { 0xff9aa0a8 };
    inline juce::Colour faintText    { 0xff5d626a };
    inline juce::Colour sectionBg    { 0xff24262b };
    inline juce::Colour slotBg       { 0xff31343a };
    inline juce::Colour track        { 0xff101114 };
    inline juce::Colour meterOff     { 0xff1a1c20 };
    inline const juce::Colour meterGreen   { 0xff3ecf8e };
    inline const juce::Colour meterYellow  { 0xffeec643 };
    inline const juce::Colour meterRed     { 0xffef5350 };

    inline void applyTheme (bool dark)
    {
        isDarkTheme() = dark;
        if (dark)
        {
            background = juce::Colour (0xff141518); stripOdd  = juce::Colour (0xff1d1f23);
            stripEven  = juce::Colour (0xff222428); card      = juce::Colour (0xff2a2c31);
            topBar     = juce::Colour (0xff1a1b1e); ribbon    = juce::Colour (0xff191a1d);
            border     = juce::Colour (0xff35383e); divider   = juce::Colour (0xff2b2d32);
            text       = juce::Colour (0xffe6e8eb); subText   = juce::Colour (0xff9aa0a8);
            faintText  = juce::Colour (0xff5d626a); sectionBg = juce::Colour (0xff24262b);
            slotBg     = juce::Colour (0xff31343a); track     = juce::Colour (0xff101114);
            meterOff   = juce::Colour (0xff1a1c20);
        }
        else
        {
            background = juce::Colour (0xffcbddfe); stripOdd  = juce::Colour (0xffd4e2fc);
            stripEven  = juce::Colour (0xffdde9fd); card      = juce::Colour (0xffe9f0fe);
            topBar     = juce::Colour (0xffc3d5f8); ribbon    = juce::Colour (0xffc6d7f9);
            border     = juce::Colour (0xff9fb3dd); divider   = juce::Colour (0xffb3c5ea);
            text       = juce::Colour (0xff20304f); subText   = juce::Colour (0xff4c5e86);
            faintText  = juce::Colour (0xff8292b8); sectionBg = juce::Colour (0xffdce7fc);
            slotBg     = juce::Colour (0xffc9d8f7); track     = juce::Colour (0xffc2d1f4);
            meterOff   = juce::Colour (0xffb7c8ee);
        }
    }

    inline const juce::Colour sourceColour { 0xff9b6dd6 };   // P -> C 紫
    inline const juce::Colour sendColourC  { 0xff2aa9e0 };   // C -> C 蓝
    inline const juce::Colour outputColour { 0xff37b87a };   // C -> P 绿

    inline const juce::Colour accent[] =
    {
        juce::Colour (0xff2aa9e0), juce::Colour (0xff37b87a),
        juce::Colour (0xff9b6dd6), juce::Colour (0xfff0a442),
        juce::Colour (0xff3fc4c9), juce::Colour (0xffe8635a),
        juce::Colour (0xff7a8ce0), juce::Colour (0xffc07ad0),
        juce::Colour (0xffe86fa8), juce::Colour (0xff8bc34a),
        juce::Colour (0xffd8a04a), juce::Colour (0xff6f8fa8),
        juce::Colour (0xff7e57c2), juce::Colour (0xff00acc1),
        juce::Colour (0xff9ccc65)
    };
    inline constexpr int kAccentCount = 15;

    inline juce::Colour channelColour (int index) noexcept
    {
        return accent[((index % kAccentCount) + kAccentCount) % kAccentCount];
    }

    // ---- 通道槽位状态 -------------------------------------------------------
    // 正常 / 空槽（删除后保留数据，可再新建）/ 隐藏（从视图收起，可恢复）。
    // 删除时删除位整体置换到数组末尾（其余通道数据同步前移），空槽在布局中
    // 不占位，因此右侧通道紧靠左侧且不会「看起来像删掉最后一条」。
    enum SlotState : std::uint8_t { kSlotNormal = 0, kSlotEmpty = 1, kSlotHidden = 2 };

    inline std::vector<std::uint8_t>& channelSlots()
    {
        static std::vector<std::uint8_t> v;
        return v;
    }

    inline std::uint8_t slotState (int index) noexcept
    {
        const auto& v = channelSlots();
        if (index < 0 || index >= static_cast<int> (v.size())) return kSlotNormal;
        return v[static_cast<size_t> (index)];
    }

    inline void setSlotState (int index, std::uint8_t state)
    {
        if (index < 0) return;
        auto& v = channelSlots();
        if (static_cast<int> (v.size()) <= index) v.resize (static_cast<size_t> (index + 1), 0);
        v[static_cast<size_t> (index)] = state;
    }

    // ---- 通道边框样式（设置页可切换，便于 A/B 对比后回退）------------------
    // 0 = 无颜色边框：隐藏左侧纵线 + 顶部圆点（仅底部色块保留颜色提示）（默认）
    // 1 = 左侧颜色边框：输入 / 插件 / 输出各区域左侧独立细线，区域间隙自然断开；
    //     顶部圆点同样移除，通道编号靠左对齐
    // 2 = 全边框：通道色细线覆盖左侧 + 顶部 + 右侧，形成整条通道的边框（保留顶部圆点）
    inline int& channelBorderMode() noexcept
    {
        static int m = 0;
        return m;
    }

    // 兼容只读：仅「全边框」时为真（其余代码沿用，避免大面积改动）
    inline bool stripBorderEnabled() noexcept
    {
        return channelBorderMode() == 2;
    }

    // ---- 用户自定义通道颜色 / 推子卡高度（会话内共享，可持久化）------------
    inline std::vector<juce::Colour>& customChannelColours()
    {
        static std::vector<juce::Colour> v;
        return v;
    }

    /** 通道条颜色：用户自定义优先（ARGB 非 0），否则按索引取默认色。 */
    inline juce::Colour stripColour (int index) noexcept
    {
        const auto& v = customChannelColours();
        if (index >= 0 && index < static_cast<int> (v.size())
            && v[static_cast<size_t> (index)].getARGB() != 0)
            return v[static_cast<size_t> (index)];
        return channelColour (index);
    }

    /** 推子卡高度（全部通道共享）。<=0 表示自动：约为窗口高度一半。 */
    inline int& stripCardHeight() noexcept
    {
        static int h = -1;
        return h;
    }

    /** 通道条四个区域的高度（0=输入 1=插件 2=输出 3=推子）。<=0 表示自动。 */
    inline int& regionHeight (int which) noexcept
    {
        static int hs[4] = { -1, -1, -1, -1 };
        return hs[juce::jlimit (0, 3, which)];
    }

    // ---- 通道录音布防（R）：会话内共享，可持久化 ---------------------------
    inline std::vector<std::uint8_t>& channelArmState()
    {
        static std::vector<std::uint8_t> v;
        return v;
    }

    inline bool isChannelArmed (int index) noexcept
    {
        return index >= 0 && index < static_cast<int> (channelArmState().size())
            && channelArmState()[static_cast<size_t> (index)] != 0;
    }

    inline void setChannelArmed (int index, bool armed)
    {
        if (index < 0) return;
        auto& v = channelArmState();
        if (static_cast<int> (v.size()) <= index) v.resize (static_cast<size_t> (index + 1), 0);
        v[static_cast<size_t> (index)] = armed ? 1 : 0;
    }

    // ---- 通道制式（单声道 / 立体声）：决定电平表画单条还是 L/R 双条 ---------
    // 新建立体声通道时写入；即使暂未连接任何输入设备（或由其他通道互送立体声），
    // 也按双条显示。与槽位、布防一样按通道数据整体置换，重排 / 删除时不会错位。
    inline std::vector<std::uint8_t>& channelStereoState()
    {
        static std::vector<std::uint8_t> v;
        return v;
    }

    inline bool isChannelStereo (int index) noexcept
    {
        return index >= 0 && index < static_cast<int> (channelStereoState().size())
            && channelStereoState()[static_cast<size_t> (index)] != 0;
    }

    inline void setChannelStereo (int index, bool stereo)
    {
        if (index < 0) return;
        auto& v = channelStereoState();
        if (static_cast<int> (v.size()) <= index) v.resize (static_cast<size_t> (index + 1), 0);
        v[static_cast<size_t> (index)] = stereo ? 1 : 0;
    } 

    // ---- 通道「向右对齐」分组 -----------------------------------------------
    // 标记为右对齐的通道恒排在调音台最右侧；新建通道 / 后来才右对齐的通道
    // 都插到该组左边缘。组内可自由左右拖动调整顺序，但不能跨到普通通道一侧。
    // 与槽位、布防一样按通道数据整体置换，重排 / 删除时不会错位。
    inline std::vector<std::uint8_t>& rightAlignedState()
    {
        static std::vector<std::uint8_t> v;
        return v;
    }

    inline bool isChannelRightAligned (int index) noexcept
    {
        return index >= 0 && index < static_cast<int> (rightAlignedState().size())
            && rightAlignedState()[static_cast<size_t> (index)] != 0;
    }

    inline void setChannelRightAligned (int index, bool on)
    {
        if (index < 0) return;
        auto& v = rightAlignedState();
        if (static_cast<int> (v.size()) <= index) v.resize (static_cast<size_t> (index + 1), 0);
        v[static_cast<size_t> (index)] = on ? 1 : 0;
    }

    // 「向右对齐」之前所在的槽位（取消对齐时恢复用；-1 = 无记忆，回到分组左缘）
    inline std::vector<int>& rightAlignedOrigin()
    {
        static std::vector<int> v;
        return v;
    }

    inline int channelRightAlignedOrigin (int index) noexcept
    {
        return index >= 0 && index < static_cast<int> (rightAlignedOrigin().size())
                   ? rightAlignedOrigin()[static_cast<size_t> (index)] : -1;
    }

    inline void setChannelRightAlignedOrigin (int index, int pos)
    {
        if (index < 0) return;
        auto& v = rightAlignedOrigin();
        if (static_cast<int> (v.size()) <= index) v.resize (static_cast<size_t> (index + 1), -1);
        v[static_cast<size_t> (index)] = pos;
    }

    inline juce::Colour sendColour (mb::EdgeKind kind) noexcept
    {
        switch (kind)
        {
            case mb::EdgeKind::InputToChannel:   return sourceColour;
            case mb::EdgeKind::ChannelToChannel: return sendColourC;
            case mb::EdgeKind::ChannelToOutput:  return outputColour;
        }
        return subText;
    }

    // ---- 字体 --------------------------------------------------------------
    inline juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions ("Microsoft YaHei UI", size,
                                              bold ? juce::Font::bold : juce::Font::plain));
    }

    // ---- 增益换算 ----------------------------------------------------------
    inline float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }
    inline float gainToDb (float gain) noexcept
    {
        return 20.0f * std::log10 ((std::max) (1.0e-5f, gain));
    }

    inline float levelToMeter (float peak) noexcept
    {
        const float db = gainToDb (peak);
        return std::clamp ((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    }

    // ---- 推子皮肤：调音台通道条与所有内置插件共用 --------------------------
    // 颜色全部在 paint 时实时读取，切换深 / 浅主题无需重建控件。
    // 左侧参考值刻度：主刻度 subText，其余 faintText / divider。
    inline void drawFaderRefTick (juce::Graphics& g, float left, float y,
                                  const juce::String& text, bool major)
    {
        g.setFont (font (6.2f));
        g.setColour (major ? subText : faintText);
        g.drawText (text, left, y - 5.0f, 14.0f, 10.0f,
                    juce::Justification::centredRight, false);
        g.setColour (major ? subText : divider);
        g.fillRect (left + 14.0f, y - 0.5f, 3.0f, 1.0f);
    }

    /** 细槽导轨 + 物理手柄：投影 / 立体渐变帽体 / 顶缘高光 / 凹刻中心线 / 防滑纹。 */
    inline void drawFaderRailAndCap (juce::Graphics& g, const juce::Rectangle<float>& rail,
                                     float sliderPos, juce::Colour thumb,
                                     float capW = 26.0f, float capH = 18.0f)
    {
        const float cx = rail.getCentreX();
        g.setColour (track);
        g.fillRoundedRectangle (rail, 2.0f);
        g.setColour (thumb.withAlpha (0.30f));
        g.fillRoundedRectangle (rail.withBottom (juce::jmax (rail.getY(), sliderPos)), 2.0f);

        const auto cap = juce::Rectangle<float> (cx - capW * 0.5f, sliderPos - capH * 0.5f,
                                                 capW, capH);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (cap.translated (0.0f, 2.5f), 3.5f);
        g.setGradientFill (juce::ColourGradient (card.brighter (0.14f), 0.0f, cap.getY(),
                                                 slotBg.darker (0.30f), 0.0f, cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 3.5f);
        g.setColour (border);
        g.drawRoundedRectangle (cap.reduced (0.5f), 3.5f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawLine (cap.getX() + 3.5f, cap.getY() + 1.0f, cap.getRight() - 3.5f, cap.getY() + 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (cap.getX() + 4.0f, sliderPos - 0.2f, cap.getWidth() - 8.0f, 2.4f);
        g.setColour (thumb);
        g.fillRect (cap.getX() + 4.0f, sliderPos - 1.0f, cap.getWidth() - 8.0f, 2.0f);
        g.setColour (faintText);
        for (float gx : { cap.getX() + 3.0f, cap.getRight() - 4.5f })
            g.drawLine (gx, cap.getY() + 4.0f, gx, cap.getBottom() - 4.0f, 1.6f);
    }

    /** 纵向推子 LookAndFeel：railRightInset > 0 时导轨靠右，左侧留给参考值刻度。 */
    class FaderSkin : public juce::LookAndFeel_V4
    {
    public:
        juce::Colour thumb = accent[0];
        float railRightInset = 0.0f;
        float capW = 26.0f, capH = 18.0f;

        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPos, float, float,
                               juce::Slider::SliderStyle, juce::Slider&) override
        {
            const float cx = railRightInset > 0.0f
                           ? static_cast<float> (x + width) - railRightInset
                           : static_cast<float> (x) + static_cast<float> (width) * 0.5f;
            const auto rail = juce::Rectangle<float> (
                cx - 2.0f, static_cast<float> (y) + 10.0f, 4.0f,
                juce::jmax (1.0f, static_cast<float> (height) - 20.0f));
            drawFaderRailAndCap (g, rail, sliderPos, thumb, capW, capH);
        }
    };
}
