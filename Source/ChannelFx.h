#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

#include "I18n.h"

//==============================================================================
// 通道内置效果链（参考主流机架的自研内置效果器思路）
//
// · 每通道一条插入式效果链：噪声门 -> 高通 -> 三段 EQ -> 压缩 -> 驱动
//   -> 延迟 -> 混响 -> 限制器。全部使用 JUCE 基础 DSP 模块，不引入 VST3 SDK。
// · 参数经不可变快照发布（atomic<shared_ptr>），音频线程按通道读取；
//   系数重算在音频线程内完成且仅发生在参数实际变化的那一个回调里。
// · 效果插在「通道求和之后、出边之前」：C->C 互送与 C->P 输出听到的都是
//   处理后的信号；推子增益仍在出边上，与效果链无关。
//==============================================================================
namespace mb
{
    struct FxParams
    {
        bool  hpfOn       = false;
        float hpfHz       = 90.0f;

        float eqLowDb     = 0.0f;      // low shelf  @180 Hz
        float eqMidDb     = 0.0f;      // peak       @eqMidHz
        float eqMidHz     = 1000.0f;   // 中频中心频率（200 Hz ~ 5 kHz）
        float eqHighDb    = 0.0f;      // high shelf @5.2 kHz

        bool  gateOn      = false;
        float gateThreshDb = -52.0f;

        bool  compOn      = false;
        float compThreshDb = -24.0f;
        float compRatio   = 3.0f;
        float compAtkMs   = 12.0f;     // 启动 1..100 ms
        float compRelMs   = 180.0f;    // 释放 30..1000 ms

        bool  driveOn     = false;     // 激励驱动（tanh 饱和）
        float driveAmt    = 3.0f;      // 1..10
        float driveMix    = 0.35f;     // 0..1

        bool  delayOn     = false;     // 延迟（反馈为 0 时无回声，仅单次延迟）
        float delayMs     = 240.0f;    // 10..1000 ms
        float delayFb     = 0.0f;      // 反馈 0..0.95（默认 0 = 干净延迟，无回音；拉高产生回声）
        float delayMix    = 0.25f;     // 0..1

        bool  reverbOn    = false;
        float reverbMix   = 0.22f;
        float reverbSize  = 0.5f;

        bool  limitOn     = false;
        float limitCeilDb = -1.0f;

        bool  duckOn       = false;    // 侧链闪避：源通道出声时自动压低本通道
        int   duckSrcCh    = -1;       // 源通道（-1 = 未指定；引擎忽略 == 自身）
        float duckThreshDb = -28.0f;   // 源电平超过此值开始闪避
        float duckDepthDb  = -12.0f;   // 压低量（0 ~ -24 dB）
        float duckAtkMs    = 15.0f;    // 启动
        float duckRelMs    = 350.0f;   // 释放

        bool any() const noexcept
        {
            return hpfOn || gateOn || compOn || reverbOn || limitOn
                || driveOn || delayOn || duckOn
                || std::fabs (eqLowDb)  > 0.05f
                || std::fabs (eqMidDb)  > 0.05f
                || std::fabs (eqHighDb) > 0.05f;
        }

        bool sameAs (const FxParams& o) const noexcept
        {
            return std::memcmp (this, &o, sizeof (FxParams)) == 0;
        }
    };

    //--------------------------------------------------------------------------
    // 自带插件：内置效果链按模块拆分为独立「插件」，用户经「+ 插件」逐个添加，
    // 不再自动预设到通道。id 映射到 FxParams 的对应开关。
    // id 9 = 音频播放：独立播放引擎（MediaPlayerBank），不走 FxParams。
    //--------------------------------------------------------------------------
    struct BuiltInPlugin { int id; const char* zh; const char* en; };

    inline constexpr BuiltInPlugin builtInPluginTable[] =
    {
        { 1, "噪声门",   "Noise Gate" },
        { 2, "高通滤波", "High-Pass"  },
        { 3, "三段均衡", "3-Band EQ"  },
        { 4, "压缩器",   "Compressor" },
        { 7, "延迟",     "Delay"      },
        { 5, "混响",     "Reverb"     },
        { 8, "激励驱动", "Drive"      },
        { 6, "限制器",   "Limiter"    },
        { 9, "播放器",   "Player"       },
        { 10, "闪避",    "Ducker"     },
    };
    inline constexpr int kBuiltInPluginCount = 10;

    inline juce::String builtInPluginName (int id)
    {
        for (const auto& p : builtInPluginTable)
            if (p.id == id) return tr (p.zh, p.en);
        return {};
    }

    /** 把自带插件的添加 / 移除映射到 FxParams 开关（on = 添加，off = 移除）。 */
    inline void applyBuiltInToParams (FxParams& p, int id, bool on)
    {
        switch (id)
        {
            case 1: p.gateOn = on; break;
            case 2: p.hpfOn  = on; break;
            case 3:
                if (on)
                {
                    if (std::fabs (p.eqLowDb) < 0.05f && std::fabs (p.eqMidDb) < 0.05f
                        && std::fabs (p.eqHighDb) < 0.05f)
                        p.eqMidDb = 2.0f;               // 全零时给一个可听的初始量
                }
                else
                {
                    p.eqLowDb = p.eqMidDb = p.eqHighDb = 0.0f;
                }
                break;
            case 4: p.compOn   = on; break;
            case 5: p.reverbOn = on; break;
            case 6: p.limitOn  = on; break;
            case 7: p.delayOn  = on; break;   // 时间/反馈/湿度取 FxParams 默认或用户上次值
            case 8: p.driveOn  = on; break;
            case 10: p.duckOn = on; break;   // 源通道/深度等取 FxParams 当前值或默认
            default: break;
        }
    }

    //------------------------------------------------------------------
    // 真联动立体声动态处理：门 / 压缩 / 限制器共享 L/R 检测器，同一增益应用于
    // 两路 —— 避免声场被独立压缩 / 限制器拉歪。线性模块（HPF/EQ）与逐点模块
    // （驱动 / 延迟）逐通道独立处理即可，无需联动。
    //------------------------------------------------------------------
    class LinkedDynamics
    {
    public:
        void prepare (double sr) noexcept { sr_ = sr > 8000.0 ? sr : 48000.0; reset(); }

        void reset() noexcept
        {
            env_ = 0.0f; gateGain_ = 1.0f; compGain_ = 1.0f;
        }

        void apply (const FxParams& p) noexcept
        {
            p_ = p;
            const double sr = sr_;
            // 峰值包络平滑系数（攻击快、释放慢）
            envAtk_  = smoothing (1.0,  sr);    // ~1 ms 上升
            envRel_  = smoothing (120.0, sr);   // ~120 ms 下降
            gateAtk_ = smoothing (2.0,  sr);
            gateRel_ = smoothing (80.0, sr);
            compAtk_ = smoothing (p.compAtkMs > 0.5 ? p.compAtkMs : 5.0, sr);
            compRel_ = smoothing (p.compRelMs > 0.5 ? p.compRelMs : 120.0, sr);
        }

        void process (float* L, float* R, int n) const noexcept
        {
            const FxParams& p = p_;
            if (! (p.gateOn || p.compOn || p.limitOn)) return;   // 调用方已判，双保险

            for (int i = 0; i < n; ++i)
            {
                // 共享检测器：L/R 中较大者驱动同一包络
                const float inst = juce::jmax (std::fabs (L[i]), std::fabs (R[i]));
                if (inst > env_) env_ += (inst - env_) * envAtk_;
                else             env_ += (inst - env_) * envRel_;
                const float db = 20.0f * std::log10 (juce::jmax (env_, 1.0e-6f));

                float g = 1.0f;

                if (p.gateOn)                       // 噪声门：低于阈值静音
                {
                    const float target = (db < p.gateThreshDb) ? 0.0f : 1.0f;
                    const float coeff = (target > gateGain_) ? gateAtk_ : gateRel_;
                    gateGain_ += (target - gateGain_) * coeff;
                    g *= gateGain_;
                }

                if (p.compOn)                       // 压缩：硬膝，输出同一增益
                {
                    const float target = compGainDb (db);
                    const float coeff = (target < compGain_) ? compAtk_ : compRel_;
                    compGain_ += (target - compGain_) * coeff;
                    g *= compGain_;
                }

                if (p.limitOn)                      // 限制器：把峰值压到上限
                {
                    g *= limitGainDb (db);
                }

                L[i] *= g;
                R[i] *= g;
            }
        }

    private:
        static double smoothing (double ms, double sr) noexcept
        {
            const double tau = ms / 1000.0;
            return 1.0 - std::exp (-1.0 / (tau * sr));
        }

        static float dbToLin (float db) noexcept { return std::pow (10.0f, db / 20.0f); }

        float compGainDb (float db) const noexcept
        {
            const float thr = p_.compThreshDb;
            if (db <= thr) return 1.0f;
            const float out = thr + (db - thr) / p_.compRatio;
            return dbToLin (out - db);
        }

        float limitGainDb (float db) const noexcept
        {
            if (db <= p_.limitCeilDb) return 1.0f;
            return dbToLin (p_.limitCeilDb - db);
        }

        double sr_ = 48000.0;
        FxParams p_ {};
        mutable float env_ = 0.0f;        // 包络需跨块连续 → mutable（const process 内修改）
        mutable float gateGain_ = 1.0f;
        mutable float compGain_ = 1.0f;
        double envAtk_ = 0.5, envRel_ = 0.05, gateAtk_ = 0.5, gateRel_ = 0.1,
               compAtk_ = 0.3, compRel_ = 0.2;
    };

    /** 单通道立体声 DSP 状态。只在音频线程使用；prepare 在控制线程调用。 */
    class ChannelFxSlot
    {
    public:
        void prepare (double sampleRate, int maxBlock)
        {
            sr_ = sampleRate;
            juce::dsp::ProcessSpec spec { sampleRate,
                                          (juce::uint32) juce::jmax (32, maxBlock), 2 };
            hpf.prepare (spec);
            eqLow.prepare (spec);
            eqMid.prepare (spec);
            eqHigh.prepare (spec);

            juce::dsp::ProcessSpec spec1 { sampleRate,
                                          (juce::uint32) juce::jmax (32, maxBlock), 1 };
            delayL.prepare (spec1);
            delayR.prepare (spec1);
            delayL.setMaximumDelayInSamples (static_cast<int> (sampleRate) + 1);
            delayR.setMaximumDelayInSamples (static_cast<int> (sampleRate) + 1);
            delayL.reset();
            delayR.reset();

            linked_.prepare (sampleRate);
            applied_ = FxParams {};
            appliedValid_ = false;
        }

        bool matches (const FxParams& p) const noexcept { return appliedValid_ && applied_.sameAs (p); }

        /** 重算全部系数（音频线程内调用；参数不变时不要调用）。 */
        void apply (const FxParams& p)
        {
            if (p.hpfOn)
                hpf.coefficients = juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass
                                   (sr_, juce::jlimit (20.0f, 500.0f, p.hpfHz));

            eqLow.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf
                                  (sr_, 180.0f, 0.707f, dbFactor (p.eqLowDb));
            eqMid.coefficients  = juce::dsp::IIR::Coefficients<float>::makePeakFilter
                                  (sr_, juce::jlimit (200.0f, 5000.0f, p.eqMidHz),
                                   0.9f, dbFactor (p.eqMidDb));
            eqHigh.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf
                                  (sr_, 5200.0f, 0.707f, dbFactor (p.eqHighDb));

            juce::dsp::Reverb::Parameters rp;
            rp.roomSize = juce::jlimit (0.0f, 1.0f, p.reverbSize);
            rp.damping  = 0.4f;
            rp.width    = 0.7f;
            rp.wetLevel = juce::jlimit (0.0f, 1.0f, p.reverbMix);
            rp.dryLevel = 1.0f - rp.wetLevel * 0.85f;
            rp.freezeMode = 0.0f;
            reverb.setParameters (rp);

            linked_.apply (p);

            applied_ = p;
            appliedValid_ = true;
        }

        void process (const FxParams& p, float* L, float* R, int n) noexcept
        {
            if (! appliedValid_) return;

            float* ptrs[2] = { L, R };
            juce::dsp::AudioBlock<float> block (ptrs, 2, static_cast<size_t> (n));
            juce::dsp::ProcessContextReplacing<float> ctx (block);

            // 线性模块（HPF / EQ）逐通道处理，无需联动
            if (p.hpfOn)    hpf.process (ctx);
            if (std::fabs (p.eqLowDb)  > 0.05f) eqLow.process (ctx);
            if (std::fabs (p.eqMidDb)  > 0.05f) eqMid.process (ctx);
            if (std::fabs (p.eqHighDb) > 0.05f) eqHigh.process (ctx);

            // 激励驱动：逐通道 tanh 饱和（波形整形的逐点处理，独立即可）
            if (p.driveOn)
            {
                const float amt  = juce::jlimit (1.0f, 10.0f, p.driveAmt);
                const float mix  = juce::jlimit (0.0f, 1.0f, p.driveMix);
                const float norm = std::tanh (amt);
                for (int i = 0; i < n; ++i)
                {
                    const float xL = L[i], xR = R[i];
                    L[i] = xL * (1.0f - mix) + std::tanh (xL * amt) / norm * mix;
                    R[i] = xR * (1.0f - mix) + std::tanh (xR * amt) / norm * mix;
                }
            }

            // 反馈延迟：L/R 各一条延迟线（独立回声，不影响声像）
            if (p.delayOn)
            {
                const float fb  = juce::jlimit (0.0f, 0.95f, p.delayFb);
                const float mix = juce::jlimit (0.0f, 1.0f, p.delayMix);
                const float dly = juce::jlimit (10.0f, 1000.0f, p.delayMs)
                                * static_cast<float> (sr_) / 1000.0f;
                delayL.setDelay (dly);
                delayR.setDelay (dly);
                for (int i = 0; i < n; ++i)
                {
                    const float dL = delayL.popSample (0);
                    delayL.pushSample (0, L[i] + dL * fb);
                    L[i] = L[i] * (1.0f - mix) + dL * mix;
                    const float dR = delayR.popSample (0);
                    delayR.pushSample (0, R[i] + dR * fb);
                    R[i] = R[i] * (1.0f - mix) + dR * mix;
                }
            }

            if (p.reverbOn) reverb.process (ctx);

            // 联动动态：门 + 压缩 + 限制器（共享 L/R 检测器，相同增益作用于两路）
            if (p.gateOn || p.compOn || p.limitOn)
                linked_.process (L, R, n);
        }

    private:
        static float dbFactor (float db) noexcept
        {
            return std::pow (10.0f, juce::jlimit (-12.0f, 12.0f, db) / 20.0f);
        }

        double sr_ = 48000.0;
        FxParams applied_ {};
        bool appliedValid_ = false;

        juce::dsp::IIR::Filter<float> hpf, eqLow, eqMid, eqHigh;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayL, delayR;
        juce::dsp::Reverb reverb;
        LinkedDynamics linked_;
    };

    /** 全通道效果参数仓：控制线程写、音频线程读（快照发布）。 */
    class ChannelFxBank
    {
    public:
        static constexpr int kMaxChannels = 32;

        /** 全通道参数快照（不可变）。侧链闪避等跨通道 DSP 在引擎层整体读取。 */
        struct Published
        {
            std::array<FxParams, kMaxChannels> params {};
        };

        /** 音频线程：整体参数快照。 */
        std::shared_ptr<const Published> snapshot() const
        {
            return pub_.load (std::memory_order_acquire);
        }

        void prepare (double sampleRate, int maxBlock)
        {
            for (auto& s : slots_) s.prepare (sampleRate, maxBlock);
        }

        /** 控制线程：更新一个通道的参数并发布快照。 */
        void setParams (int ch, const FxParams& p)
        {
            if (ch < 0 || ch >= kMaxChannels) return;

            auto next = std::make_shared<Published>();
            if (auto cur = pub_.load (std::memory_order_acquire))
                *next = *cur;
            next->params[static_cast<size_t> (ch)] = p;
            pub_.store (std::move (next), std::memory_order_release);
            uiParams_[static_cast<size_t> (ch)] = p;
        }

        /** 控制线程（UI）读取。 */
        const FxParams& params (int ch) const
        {
            static const FxParams none {};
            if (ch < 0 || ch >= kMaxChannels) return none;
            return uiParams_[static_cast<size_t> (ch)];
        }

        /** 音频线程：对单个通道的 L/R 缓冲做插入式处理。 */
        void processChannel (int ch, float* L, float* R, int n) noexcept
        {
            if (ch < 0 || ch >= kMaxChannels) return;
            const auto snap = pub_.load (std::memory_order_acquire);
            if (snap == nullptr) return;
            const auto& p = snap->params[static_cast<size_t> (ch)];
            if (! p.any()) return;

            auto& slot = slots_[static_cast<size_t> (ch)];
            if (! slot.matches (p)) slot.apply (p);
            slot.process (p, L, R, n);
        }

    private:
        std::atomic<std::shared_ptr<const Published>> pub_;
        std::array<ChannelFxSlot, kMaxChannels> slots_;
        std::array<FxParams, kMaxChannels> uiParams_ {};
    };
}
