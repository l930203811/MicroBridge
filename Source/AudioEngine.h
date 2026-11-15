#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "ChannelFx.h"
#include "ChannelRecorder.h"
#include "DeviceCatalog.h"
#include "I18n.h"
#include "MediaPlayer.h"
#include "PluginRack.h"
#include "RouteGraph.h"

// MF 系头文件可能引入 windef.h 的 min/max 宏，破坏后续 std::min/std::max
#ifdef min
 #undef min
#endif
#ifdef max
 #undef max
#endif

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

//==============================================================================
// 多设备音频引擎
//
// · 主设备由 AudioDeviceManager 托管，是唯一的处理主时钟；设备选定后自动
//   addAudioCallback，界面不提供任何「启动 / 停止」控件。
// · 其它驱动按需懒打开为辅助设备（AudioIODeviceType::createDevice），
//   每个辅助设备用 SPSC 无锁环形缓冲 + 线性插值做轻量 ASRC，
//   抵消不同声卡晶振的 ppm 级漂移。
// · 音频回调只按 ProcessingPlan 做乘加：零分配、零锁、零图遍历。
//==============================================================================
namespace mb
{
    //--------------------------------------------------------------------------
    /** 单生产者 / 单消费者环形缓冲。绝对下标单调递增，支持分数速率读取。 */
    class StreamRing
    {
    public:
        void prepare (int numChannels, int capacityPow2 = 16)   // 65536 帧 ≈ 1.3 s @48k
        {
            channels_ = juce::jlimit (1, 32, numChannels);   // ASIO 多通道设备可达 16/32 路
            capacity_ = 1 << juce::jlimit (10, 20, capacityPow2);
            mask_ = capacity_ - 1;
            data_.assign (static_cast<size_t> (channels_ * capacity_), 0.0f);
            reset();
        }

        void reset()
        {
            write_.store (0, std::memory_order_relaxed);
            floor_.store (0, std::memory_order_relaxed);
            skipTo_.store (0, std::memory_order_relaxed);
            frac_ = 0.0;
            std::fill (data_.begin(), data_.end(), 0.0f);
            underruns_ = 0;
            overflows_ = 0;
        }

        /** 生产者线程：一次写入全部通道的 n 帧。 */
        void push (const float* const* src, int numChannels, int n)
        {
            const auto w = write_.load (std::memory_order_relaxed);
            const auto f = floor_.load (std::memory_order_acquire);
            if (w - f + static_cast<std::uint64_t> (n) > static_cast<std::uint64_t> (capacity_))
            {
                skipTo_.store (w + static_cast<std::uint64_t> (n)
                               - static_cast<std::uint64_t> (capacity_),
                               std::memory_order_release);
                ++overflows_;
            }

            const int chans = juce::jmin (channels_, numChannels);
            for (int c = 0; c < chans; ++c)
            {
                if (src[c] == nullptr) continue;
                float* dst = data_.data() + static_cast<size_t> (c) * static_cast<size_t> (capacity_);
                for (int i = 0; i < n; ++i)
                    dst[(w + static_cast<std::uint64_t> (i)) & static_cast<std::uint64_t> (mask_)] = src[c][i];
            }
            write_.store (w + static_cast<std::uint64_t> (n), std::memory_order_release);
        }

        /** 消费者线程：按 step = 生产者速率 / 消费者速率 读取 n 帧
            （每个输出帧在源缓冲里前进多少帧；两者速率相同时为 1）。 */
        void pull (float* const* dst, int numChannels, int n, double step, double targetFill)
        {
            const auto w = write_.load (std::memory_order_acquire);
            auto base = floor_.load (std::memory_order_relaxed);
            const auto skip = skipTo_.load (std::memory_order_acquire);
            if (base < skip)
            {
                base = skip;
                frac_ = 0.0;
            }

            const double start = static_cast<double> (base) + frac_;
            const double fill  = static_cast<double> (w) - start;
            const double err   = (fill - targetFill) / juce::jmax (1.0, targetFill);
            const double rate  = step * (1.0 + juce::jlimit (-0.05, 0.05, err * 0.004));
            const double end   = start + static_cast<double> (n) * rate;

            const int chans = juce::jmin (channels_, numChannels);
            for (int c = 0; c < chans; ++c)
            {
                if (dst[c] == nullptr) continue;
                const float* buf = data_.data() + static_cast<size_t> (c) * static_cast<size_t> (capacity_);
                double pos = start;
                for (int i = 0; i < n; ++i)
                {
                    const auto p = static_cast<std::uint64_t> (pos);
                    if (p + 1 >= w)
                    {
                        dst[c][i] = 0.0f;                 // 欠载：保持静音，不越过生产者
                        ++underruns_;
                    }
                    else
                    {
                        const float a = buf[p & static_cast<std::uint64_t> (mask_)];
                        const float b = buf[(p + 1) & static_cast<std::uint64_t> (mask_)];
                        dst[c][i] = a + (b - a) * static_cast<float> (pos - static_cast<double> (p));
                    }
                    pos += rate;
                }
            }

            // 欠载时把读指针夹在生产者位置，避免形成永久错位。
            const double target = juce::jmin (end, static_cast<double> (w));
            const auto nb = static_cast<std::uint64_t> (juce::jmax (0.0, target));
            frac_ = juce::jmax (0.0, target) - static_cast<double> (nb);
            floor_.store (nb, std::memory_order_release);
        }

        int underruns() const noexcept { return underruns_; }
        int overflows() const noexcept { return overflows_; }

    private:
        std::vector<float> data_;
        std::atomic<std::uint64_t> write_ { 0 }, floor_ { 0 }, skipTo_ { 0 };
        double frac_ = 0.0;
        int channels_ = 2, capacity_ = 65536, mask_ = 65535;
        int underruns_ = 0, overflows_ = 0;
    };

    //--------------------------------------------------------------------------
    /** 一个辅助设备（非主设备）。输出 = 播放宿，输入 = 采集源。 */
    class AuxDevice : public juce::AudioIODeviceCallback
    {
    public:
        AuxDevice (juce::AudioDeviceManager& dm, const DeviceEntry& entry,
                   bool needInput, bool needOutput, double masterRate, int masterBuffer)
            : needInput_ (needInput), needOutput_ (needOutput),
              masterRate_ (masterRate > 0.0 ? masterRate : 48000.0),
              masterBuffer_ (masterBuffer > 0 ? masterBuffer : 256)
        {
            auto* type = typeFor (dm, entry.typeIndex);
            if (type == nullptr)
            {
                error_ = tr ("驱动类型不可用", "Device type unavailable");
                return;
            }

            // ① 只打开真正需要的那一侧。旧版即使只需输入，也会把同类驱动的
            //    首个输出端点一起传给 createDevice；多个辅助设备于是重复打开同一个
            //    播放端点，在独占 / 低延迟模式下会直接 open 失败，输入随之静音。
            const auto outName = needOutput_
                ? (entry.createOutputName.isNotEmpty() ? entry.createOutputName : entry.outputName)
                : juce::String();
            const auto inName = needInput_
                ? (entry.createInputName.isNotEmpty() ? entry.createInputName : entry.inputName)
                : juce::String();
            std::unique_ptr<juce::AudioIODevice> raw (type->createDevice (outName, inName));
            if (raw == nullptr)
            {
                error_ = tr ("无法创建设备实例", "Could not create device");
                return;
            }

            // ② 通道数按目录探测到的真实值，而不是写死 2 路。
            nIn_  = needInput_  ? juce::jlimit (1, 32, entry.inputChannels)  : 0;
            nOut_ = needOutput_ ? juce::jlimit (1, 32, entry.outputChannels) : 0;

            juce::BigInteger inBits, outBits;
            if (needInput_)
            {
                const int reported = raw->getInputChannelNames().size();
                const int count = reported > 0 ? juce::jmin (nIn_, reported) : nIn_;
                for (int i = 0; i < count; ++i) inBits.setBit (i);
            }
            if (needOutput_)
            {
                const int reported = raw->getOutputChannelNames().size();
                const int count = reported > 0 ? juce::jmin (nOut_, reported) : nOut_;
                for (int i = 0; i < count; ++i) outBits.setBit (i);
            }

            const double rate = nearestRate (raw->getAvailableSampleRates(), masterRate_);
            const int buffer  = nearestBuffer (raw->getAvailableBufferSizes(), masterBuffer_);
            error_ = raw->open (inBits, outBits, rate, buffer);
            if (error_.isNotEmpty()) return;

            device_ = std::move (raw);
            deviceRate_ = device_->getCurrentSampleRate();
            if (deviceRate_ <= 0.0) deviceRate_ = masterRate_;

            // 指针表在控制线程一次性分配，音频回调只读，保持实时安全。
            inPtrs_.resize  (static_cast<size_t> (juce::jmax (1, nIn_)),  nullptr);
            outPtrs_.resize (static_cast<size_t> (juce::jmax (1, nOut_)), nullptr);
            // 16384 帧 ≈ 0.34 s @48k，远大于 targetFill(≥1024)；
            // 多通道时按此容量可把每对 ring 控制在 4 MB 以内。
            inRing_.prepare  (juce::jmax (1, nIn_),  14);
            outRing_.prepare (juce::jmax (1, nOut_), 14);
            dummy_.resize (8192, 0.0f);

            targetFill_ = static_cast<double> (juce::jmax (1024, masterBuffer_ * 4));
            device_->start (this);
            opened_ = true;
        }

        ~AuxDevice() override { close(); }

        void close()
        {
            if (device_ != nullptr && opened_)
            {
                device_->stop();
                device_->close();
            }
            opened_ = false;
        }

        bool opened() const noexcept { return opened_; }
        const juce::String& error() const noexcept { return error_; }
        double deviceRate() const noexcept { return deviceRate_; }
        bool needInput()  const noexcept { return needInput_; }
        bool needOutput() const noexcept { return needOutput_; }
        int inputChannels()  const noexcept { return nIn_; }
        int outputChannels() const noexcept { return nOut_; }

        void pushOutput (const float* const* src, int n)
        {
            if (opened_ && needOutput_)
                outRing_.push (src, static_cast<int> (outPtrs_.size()), n);
        }

        void pullInput (float* const* dst, int n)
        {
            if (opened_ && needInput_)
                // ring 里的源帧速率 = deviceRate_，消费端输出速率 = masterRate_。
                // 每输出一帧应前进 deviceRate_/masterRate_ 个源帧；写反会在
                // 两端采样率不一致时把音频拉快 / 拉慢（听感为变调）。
                inRing_.pull (dst, static_cast<int> (inPtrs_.size()), n,
                              deviceRate_ / masterRate_, targetFill_);
        }

        int underruns() const noexcept { return inRing_.underruns() + outRing_.underruns(); }
        int overflows() const noexcept { return inRing_.overflows() + outRing_.overflows(); }

        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                               int numInputChannels,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            const int n = juce::jmin (numSamples, 8192);

            for (int c = 0; c < numOutputChannels; ++c)
                if (outputChannelData[c] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[c], n);

            if (needInput_)
            {
                // 旧版在这里无条件读 inputChannelData[1]；设备只有 0/1 路输入时
                // 会越界取到无效指针（随后虽被覆盖，读本身已属未定义行为）。
                for (size_t c = 0; c < inPtrs_.size(); ++c)
                {
                    const int ci = static_cast<int> (c);
                    inPtrs_[c] = (ci < numInputChannels && inputChannelData[ci] != nullptr)
                                 ? inputChannelData[ci] : dummy_.data();
                }
                inRing_.push (inPtrs_.data(), static_cast<int> (inPtrs_.size()), n);
            }

            if (needOutput_)
            {
                for (size_t c = 0; c < outPtrs_.size(); ++c)
                {
                    const int ci = static_cast<int> (c);
                    outPtrs_[c] = (ci < numOutputChannels && outputChannelData[ci] != nullptr)
                                  ? outputChannelData[ci] : dummy_.data();
                }
                // outRing 的生产者是主回调（masterRate_），消费者是本辅助设备
                // 回调（deviceRate_），step 方向与 inRing 相反。
                outRing_.pull (outPtrs_.data(), static_cast<int> (outPtrs_.size()), n,
                               masterRate_ / deviceRate_, targetFill_);
            }
        }

        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}

    private:
        static juce::AudioIODeviceType* typeFor (juce::AudioDeviceManager& dm, int index)
        {
            const auto& types = dm.getAvailableDeviceTypes();
            return index >= 0 && index < types.size() ? types[index] : nullptr;
        }

        static double nearestRate (const juce::Array<double>& rates, double wanted)
        {
            if (rates.isEmpty()) return wanted;
            double best = rates[0];
            for (auto r : rates)
                if (std::abs (r - wanted) < std::abs (best - wanted)) best = r;
            return best;
        }

        static int nearestBuffer (const juce::Array<int>& sizes, int wanted)
        {
            if (sizes.isEmpty()) return wanted;
            int best = sizes[0];
            for (auto s : sizes)
                if (std::abs (s - wanted) < std::abs (best - wanted)) best = s;
            return best;
        }

        bool needInput_, needOutput_, opened_ = false;
        double masterRate_, deviceRate_, targetFill_;
        int masterBuffer_;
        int nIn_ = 0, nOut_ = 0;
        juce::String error_;
        std::unique_ptr<juce::AudioIODevice> device_;
        StreamRing inRing_, outRing_;
        std::vector<float> dummy_;
        std::vector<const float*> inPtrs_;
        std::vector<float*> outPtrs_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuxDevice)
    };

    //--------------------------------------------------------------------------
    /** 端口 -> 实际采样缓冲区 的不可变映射，控制线程编译、音频线程只读。 */
    struct PortRoute
    {
        bool master = false;
        int masterChannel = 0;
        int auxSlot = -1;
        int auxChannel = 0;
    };

    struct AuxSlot
    {
        AuxDevice* device = nullptr;
        bool needInput = false, needOutput = false;
        int inputChannels = 2, outputChannels = 2;
        std::vector<std::pair<int, int>> inputs;    // (端口 id, 环通道)
        std::vector<std::pair<int, int>> outputs;
    };

    struct PortRouting
    {
        std::vector<PortRoute> in, out;
        std::vector<AuxSlot> aux;
        double masterRate = 48000.0;
    };

    //--------------------------------------------------------------------------
    class AudioEngine : public juce::AudioIODeviceCallback
    {
    public:
        explicit AudioEngine (mb::RouteGraph& graph) : graph_ (graph)
        {
            for (auto& v : peaks_)
                v.store (0.0f, std::memory_order_relaxed);
            for (auto& v : portPeaks_)
                v.store (0.0f, std::memory_order_relaxed);

            catalog_.scan (deviceManager_);
            deviceManager_.initialise (2, 2, nullptr, true);
            publishing (buildRouting());
            prepareBuffers();
            pluginCatalog_.loadCache();

            // 自带插件旁通联动：「音频播放」圆点 = 静音（时间轴继续走）
            pluginBank_.onBuiltInBypass = [this] (int ch, int id, bool bypass)
            {
                if (id == 9) mediaPlayers_.setBypassMuted (ch, bypass);
            };
        }

        ~AudioEngine() override
        {
            stopVirtualClock();              // 必须先停：线程体内会调用本对象的音频回调
            deviceManager_.removeAudioCallback (this);
            for (auto& a : auxDevices_)
                if (a->device != nullptr) a->device->close();
            auxDevices_.clear();
#if JUCE_WINDOWS
            if (auto* h = static_cast<HANDLE> (audioThreadHandle_.exchange (nullptr)))
                CloseHandle (h);
#endif
        }

        juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }

        /** 请求音频设备切换采样率（设备不支持时由系统自动回退到可用值）。 */
        void setDeviceSampleRate (double sr)
        {
            // 无主设备时没有真实设备可调速率：重建虚拟时钟 + 全部辅助流，
            // 否则 AuxDevice 里的 masterRate_/targetFill_ 仍是旧值，ASRC 会变调。
            if (virtualRate_.load (std::memory_order_acquire) > 0.0)
            {
                if (juce::approximatelyEqual (sampleRate(), sr)) return;
                auto c = preferences_.load();
                if (! c.valid) return;
                c.sampleRate = sr;
                applySetup (c, false);
                return;
            }
            auto setup = deviceManager_.getAudioDeviceSetup();
            if (juce::approximatelyEqual (setup.sampleRate, sr)) return;
            setup.sampleRate = sr;
            const auto err = deviceManager_.setAudioDeviceSetup (setup, true);
            if (err.isNotEmpty()) lastError_ = err;
        }
        DeviceCatalog& catalog() noexcept { return catalog_; }
        const DeviceCatalog& catalog() const noexcept { return catalog_; }

        //==================================================================
        // 通道内置效果链（控制线程接口）
        const mb::FxParams& fxParams (int ch) const noexcept { return fxBank_.params (ch); }
        void setFxParams (int ch, const mb::FxParams& p)
        {
            fxBank_.setParams (ch, p);
            if (onFxParamsChanged) onFxParamsChanged (ch);   // 通道链接联动钩子
        }

        /** UI 侧可选监听：效果参数被改动（MainComponent 安装，用于链接同步）。 */
        std::function<void (int ch)> onFxParamsChanged;

        // 通道录音（start/stop 在消息线程调用）
        mb::ChannelRecorder& recorder() noexcept { return recorder_; }

        // VST3 插件库 / 每通道插件链（消息线程增删，音频线程只读快照）
        mb::PluginCatalog& pluginCatalog() noexcept { return pluginCatalog_; }
        mb::ChannelPluginBank& pluginBank() noexcept { return pluginBank_; }
        const mb::PluginCatalog& pluginCatalog() const noexcept { return pluginCatalog_; }
        const mb::ChannelPluginBank& pluginBank() const noexcept { return pluginBank_; }

        // 「音频播放」自带插件引擎（立体声直通输出端口）
        mb::MediaPlayerBank& mediaPlayers() noexcept { return mediaPlayers_; }
        const mb::MediaPlayerBank& mediaPlayers() const noexcept { return mediaPlayers_; }

        //------------------------------------------------------------------
        // 「音频播放」立体声直通的输出端口辅助
        //------------------------------------------------------------------
        /** 输出端口是否为主设备端口（主设备 = 当前系统输出，扬声器所在）。 */
        bool outputPortIsMaster (int p) const
        {
            if (p < 0 || p >= catalog_.outputPortCount()) return false;
            const auto& port = catalog_.outputPorts()[(size_t) p];
            return port.deviceIndex >= 0
                && port.deviceIndex < static_cast<int> (catalog_.devices().size())
                && catalog_.devices()[(size_t) port.deviceIndex].isMaster;
        }

        /** 输出端口能否真正到达硬件：主设备端口，或已打开的辅助播放设备端口。 */
        bool outputPortReachable (int p) const
        {
            if (p < 0 || p >= catalog_.outputPortCount()) return false;
            const auto& port = catalog_.outputPorts()[(size_t) p];
            if (port.deviceIndex < 0
                || port.deviceIndex >= static_cast<int> (catalog_.devices().size()))
                return false;
            if (catalog_.devices()[(size_t) port.deviceIndex].isMaster) return true;
            for (const auto& h : auxDevices_)
                if (h->key.first == port.deviceIndex)
                    return h->device != nullptr && h->device->opened();
            return false;
        }

        /** 端口显示名（"设备名 · L" 形式），用于直通端口选择菜单。 */
        juce::String outputPortName (int p) const
        {
            if (p < 0 || p >= catalog_.outputPortCount()) return {};
            return catalog_.outputPorts()[(size_t) p].name;
        }

        /** 主输出设备的前两个端口（立体声直通的默认 L / R）。 */
        std::pair<int, int> masterOutputPortPair() const
        {
            int l = -1, r = -1;
            for (int p = 0; p < catalog_.outputPortCount() && r < 0; ++p)
            {
                if (! outputPortIsMaster (p)) continue;
                if (l < 0) l = p; else r = p;
            }
            if (l < 0) l = 0;
            if (r < 0) r = (l + 1 < catalog_.outputPortCount()) ? l + 1 : l;
            return { l, r };
        }

        /** 参考主流机架的调度思路：周期性把音频线程移出繁忙内核（避开 0 号核，
            并轮换排除一个核）。由 UI 定时器约每 11 s 调一次。 */
        void rebalanceAudioThread()
        {
#if JUCE_WINDOWS
            auto* h = static_cast<HANDLE> (audioThreadHandle_.load (std::memory_order_acquire));
            if (h == nullptr) return;

            SYSTEM_INFO si {};
            GetSystemInfo (&si);
            const int cores = static_cast<int> (si.dwNumberOfProcessors);
            if (cores < 2) return;

            DWORD_PTR full = 0;
            for (int i = 1; i < cores && i < 32; ++i)     // 避开 0 号核（内核/DPC 最重）
                full |= static_cast<DWORD_PTR> (1) << i;

            const int skip = 1 + (affinityRotation_++ % static_cast<unsigned> (cores - 1));
            DWORD_PTR mask = full;
            if (skip > 1 && skip < 32)
                mask &= ~(static_cast<DWORD_PTR> (1) << skip);
            if (mask != 0)
                SetThreadAffinityMask (h, mask);
#endif
        }

        bool isRunning() const noexcept { return running_; }
        bool isReady()   const noexcept { return running_ && lastError_.isEmpty(); }
        juce::String error() const { return lastError_; }
        juce::String fallbackNote() const { return fallbackNote_; }

        double sampleRate() const
        {
            // 无主设备：返回虚拟时钟速率，插件 prepare / ASRC / 诊断面板都需要有效值。
            const double vr = virtualRate_.load (std::memory_order_acquire);
            if (vr > 0.0) return vr;
            if (auto* d = deviceManager_.getCurrentAudioDevice()) return d->getCurrentSampleRate();
            return 0.0;
        }

        int bufferSizeSamples() const
        {
            const int vb = virtualBlock_.load (std::memory_order_acquire);
            if (vb > 0) return vb;
            if (auto* d = deviceManager_.getCurrentAudioDevice()) return d->getCurrentBufferSizeSamples();
            return 0;
        }

        juce::String deviceInfo() const
        {
            if (virtualRate_.load (std::memory_order_acquire) > 0.0)
                return tr ("不使用主设备 · 内部时钟", "No master device · internal clock")
                     + S("  ·  ") + juce::String (sampleRate() / 1000.0, 1) + S(" kHz  ·  ")
                     + juce::String (bufferSizeSamples()) + S(" f");
            if (auto* d = deviceManager_.getCurrentAudioDevice())
                return d->getName() + S("  ·  ")
                     + juce::String (d->getCurrentSampleRate() / 1000.0, 1) + S(" kHz  ·  ")
                     + juce::String (d->getCurrentBufferSizeSamples()) + S(" f");
            return tr ("未选择设备", "No device selected");
        }

        juce::String driverTypeName() const { return deviceManager_.getCurrentAudioDeviceType(); }

        mb::DeviceChoice savedChoice() const { return preferences_.load(); }

        int auxDeviceCount() const
        {
            int n = 0;
            for (const auto& a : auxDevices_)
                if (a->device != nullptr && a->device->opened()) ++n;
            return n;
        }

        juce::StringArray availableSampleRates() const
        {
            juce::StringArray a;
            a.add (tr ("自动", "Auto"));
            for (double r : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
                a.add (juce::String (r, 0) + S(" Hz"));
            return a;
        }

        juce::StringArray availableBufferSizes() const
        {
            juce::StringArray a;
            a.add (tr ("自动", "Auto"));
            for (int b : { 64, 96, 128, 192, 256, 384, 512, 768, 1024, 2048 })
                a.add (juce::String (b) + S(" f"));
            return a;
        }

        /** 选定驱动/输入/输出并立即自动运行；无「启动 / 停止」状态。 */
        bool applySetup (const DeviceChoice& choice, bool save = true)
        {
            stopVirtualClock();              // 切换配置期间绝不能让虚拟线程读旧缓冲
            deviceManager_.removeAudioCallback (this);
            running_ = false;
            for (auto& a : auxDevices_)
                if (a->device != nullptr) a->device->close();
            auxDevices_.clear();
            catalog_.scan (deviceManager_);

            if (choice.typeName.isNotEmpty())
                deviceManager_.setCurrentAudioDeviceType (choice.typeName, true);

            auto setup = deviceManager_.getAudioDeviceSetup();
            setup.inputDeviceName  = choice.inputName;
            setup.outputDeviceName = choice.outputName;
            setup.sampleRate = choice.sampleRate;
            setup.bufferSize = choice.bufferSize;
            setup.useDefaultInputChannels  = true;
            setup.useDefaultOutputChannels = true;

            // 「（不使用设备）」= 真正不占用任何主端点。JUCE 对双空名会
            // deleteCurrentDevice() 并返回成功，主回调不再由设备驱动。这里不
            // 再回退到默认设备（会抢占用户想留给其它设备的端点），改用内部
            // 虚拟时钟推进音频块：所有输入/输出都由勾选的其它设备（辅助流）
            // 承担，通道输出目标选到它们时照常出声。
            const bool virtualClockMode = setup.inputDeviceName.isEmpty()
                                       && setup.outputDeviceName.isEmpty();

            lastError_ = deviceManager_.setAudioDeviceSetup (setup, true);
            if (lastError_.isNotEmpty())
                return false;

            if (virtualClockMode)
            {
                virtualRate_.store  (choice.sampleRate > 8000.0 ? choice.sampleRate : 48000.0,
                                     std::memory_order_release);
                virtualBlock_.store (choice.bufferSize > 0
                                      ? juce::jlimit (32, 2048, choice.bufferSize) : 256,
                                     std::memory_order_release);
            }
            else
            {
                virtualRate_.store  (0.0, std::memory_order_release);
                virtualBlock_.store (0,   std::memory_order_release);
            }

            // JUCE 的 initialise(2,2) + useDefaultChannels 只启用前 2 路；
            // ASIO 声卡常有 8 / 16 / 32 路，不显式打开全部通道，目录里列出的
            // 第 3 路之后就全是静音端口。设备已 open，可直接读真实通道名。
            juce::StringArray masterInNames, masterOutNames;
            if (auto* cur = deviceManager_.getCurrentAudioDevice())
            {
                masterInNames  = cur->getInputChannelNames();
                masterOutNames = cur->getOutputChannelNames();
                const int nIn  = juce::jmax (2, masterInNames.size());
                const int nOut = juce::jmax (2, masterOutNames.size());
                if (nIn > 2 || nOut > 2)
                {
                    const auto working = deviceManager_.getAudioDeviceSetup();  // 已生效的 2 通道版
                    auto full = working;
                    full.useDefaultInputChannels  = false;
                    full.useDefaultOutputChannels = false;
                    full.inputChannels.clear();
                    full.outputChannels.clear();
                    if (full.inputDeviceName.isNotEmpty())  full.inputChannels.setRange  (0, nIn,  true);
                    if (full.outputDeviceName.isNotEmpty()) full.outputChannels.setRange (0, nOut, true);
                    // 某些驱动不接受全通道。JUCE 在 open 失败时会留下「设备已停止」，
                    // 必须显式回滚到刚才那份能工作的配置，否则整个引擎会静音。
                    if (deviceManager_.setAudioDeviceSetup (full, true).isNotEmpty())
                        deviceManager_.setAudioDeviceSetup (working, true);
                }
            }

            catalog_.scan (deviceManager_);
            const auto applied = deviceManager_.getAudioDeviceSetup();
            const auto masterType = deviceManager_.getCurrentAudioDeviceType();
            catalog_.markMaster (masterType, applied.inputDeviceName, applied.outputDeviceName);
            catalog_.setMasterChannels (masterType, applied.inputDeviceName, applied.outputDeviceName,
                                        masterInNames, masterOutNames);
            graph_.setPortCounts (catalog_.inputPortCount(), catalog_.outputPortCount());

            prepareBuffers();
            publishing (buildRouting());
            if (virtualClockMode)
                startVirtualClock();                       // 无主设备：内部时钟驱动主回调
            else
                deviceManager_.addAudioCallback (this);    // 自动开始，界面无启停
            running_ = true;

            // 保存「实际生效」的配置而不是请求值：某些驱动（尤其 ASIO）可能打不开，
            // JUCE 会静默回退到默认设备；若把请求值写盘，下次启动会反复回退。
            mb::DeviceChoice appliedChoice;
            const auto currentType = deviceManager_.getCurrentAudioDeviceType();
            appliedChoice.typeName   = (virtualClockMode && currentType.isEmpty())
                                     ? choice.typeName : currentType;
            appliedChoice.inputName  = applied.inputDeviceName;    // 虚拟时钟模式下为空
            appliedChoice.outputName = applied.outputDeviceName;
            appliedChoice.sampleRate = virtualClockMode ? virtualRate_.load  (std::memory_order_acquire)
                                                       : applied.sampleRate;
            appliedChoice.bufferSize = virtualClockMode ? virtualBlock_.load (std::memory_order_acquire)
                                                       : applied.bufferSize;
            appliedChoice.valid      = virtualClockMode || appliedChoice.typeName.isNotEmpty();
            fallbackNote_ = virtualClockMode
                ? tr ("不使用主设备：内部时钟运行中，请在通道里选择已勾选的设备作为输入/输出",
                      "No master device: internal clock running - pick checked devices as channel I/O")
                : ((choice.typeName.isNotEmpty() && appliedChoice.typeName != choice.typeName)
                   ? tr ("请求的驱动不可用，已回退到 ", "Requested driver unavailable, fell back to ")
                     + appliedChoice.typeName
                   : juce::String());
            if (save && appliedChoice.valid)
                preferences_.save (appliedChoice);

            syncDevices();
            return true;
        }

        /** 按当前图的连边，懒打开/关闭非主设备；控制线程调用。
            虚拟时钟模式下整个重建过程暂停处理，避免音频线程读到半更新的缓冲。 */
        void syncDevices()
        {
            if (! running_) return;
            suspendVirtualProcessing();

            struct Need { bool input = false, output = false; };
            std::map<int, Need> wanted;
            const auto& devs = catalog_.devices();

            for (const auto& e : graph_.edges())
            {
                int di = -1;
                if (e.kind == EdgeKind::InputToChannel)  di = catalog_.portDeviceIndex (true,  e.src);
                if (e.kind == EdgeKind::ChannelToOutput) di = catalog_.portDeviceIndex (false, e.dst);
                if (di < 0 || di >= static_cast<int> (devs.size()) || devs[static_cast<size_t> (di)].isMaster)
                    continue;
                if (e.kind == EdgeKind::InputToChannel)  wanted[di].input  = true;
                if (e.kind == EdgeKind::ChannelToOutput) wanted[di].output = true;
            }

            for (size_t i = 0; i < auxDevices_.size(); ++i)
                if (wanted.find (auxDevices_[i]->key.first) == wanted.end())
                    if (auxDevices_[i]->device != nullptr) auxDevices_[i]->device->close();

            for (auto& [di, need] : wanted)
            {
                auto it = std::find_if (auxDevices_.begin(), auxDevices_.end(),
                                        [di] (const std::unique_ptr<AuxHolder>& a) { return a->key.first == di; });
                if (it != auxDevices_.end())
                {
                    auto* dev = (*it)->device.get();
                    // 关键修复：设备先被当作输出目标打开（只 needOutput），之后又
                    // 从它取输入建立 P→C 时，旧代码因 opened()==true 直接 continue，
                    // 设备永远停在「只输出」状态 —— 输入端口全是静音。
                    // 现在能力不足就合并 need 并重新打开为双工。
                    const bool reopen = ! dev->opened()
                        || (need.input  && ! dev->needInput())
                        || (need.output && ! dev->needOutput());
                    if (reopen)
                    {
                        const bool wantIn  = need.input  || dev->needInput();
                        const bool wantOut = need.output || dev->needOutput();
                        dev->close();
                        (*it)->device = std::make_unique<AuxDevice> (
                            deviceManager_, devs[static_cast<size_t> (di)],
                            wantIn, wantOut, sampleRate(), bufferSizeSamples());
                        auto* fresh = (*it)->device.get();
                        if (! fresh->opened())
                        {
                            auto msg = fresh->error();
                            if (msg.isEmpty()) msg = tr ("设备打开失败", "Could not open device");
                            deviceErrors_[di] = msg;
                        }
                        else
                        {
                            deviceErrors_.erase (di);
                        }
                    }
                    continue;
                }

                auto holder = std::make_unique<AuxHolder>();
                holder->key = { di, 0 };
                holder->device = std::make_unique<AuxDevice> (deviceManager_, devs[static_cast<size_t> (di)],
                                                              need.input, need.output,
                                                              sampleRate(), bufferSizeSamples());
                if (! holder->device->opened())
                {
                    auto msg = holder->device->error();
                    if (msg.isEmpty()) msg = tr ("设备打开失败", "Could not open device");
                    deviceErrors_[di] = msg;
                }
                else
                {
                    deviceErrors_.erase (di);
                }
                auxDevices_.push_back (std::move (holder));
            }

            prepareBuffers();
            publishing (buildRouting());
            resumeVirtualProcessing();
        }

        juce::String deviceError (int deviceIndex) const
        {
            const auto it = deviceErrors_.find (deviceIndex);
            return it == deviceErrors_.end() ? juce::String() : it->second;
        }

        int auxUnderruns() const
        {
            int total = 0;
            for (const auto& a : auxDevices_) total += a->device->underruns();
            return total;
        }

        /** 每通道输出峰值（推子后，relaxed 原子），UI 定时读取。
            已乘通道推子增益；静音/独奏剔除/喇叭关闭的通道恒为 0。 */
        float channelPeak (int ch) const noexcept
        {
            if (ch < 0 || ch >= kMaxChannels) return 0.0f;
            return peaks_[static_cast<size_t> (ch)].load (std::memory_order_relaxed);
        }

        /** 通道推子后电平（立体声 L / R，relaxed 原子），UI 定时读取。
            单声道通道：R 恒为 0（缓冲 2c+1 不被任何边引用，麦克风混音前即静音）。 */
        float channelPeakL (int ch) const noexcept
        {
            if (ch < 0 || ch >= kMaxChannels) return 0.0f;
            return peaksL_[static_cast<size_t> (ch)].load (std::memory_order_relaxed);
        }
        float channelPeakR (int ch) const noexcept
        {
            if (ch < 0 || ch >= kMaxChannels) return 0.0f;
            return peaksR_[static_cast<size_t> (ch)].load (std::memory_order_relaxed);
        }

        /** 每个物理输入端口的峰值（relaxed 原子）：立体声通道的 L/R 电平来源。 */
        float inputPortPeak (int port) const noexcept
        {
            if (port < 0 || port >= kMaxPorts) return 0.0f;
            return portPeaks_[static_cast<size_t> (port)].load (std::memory_order_relaxed);
        }

        float inputPeak() const noexcept { return inputPeak_.load (std::memory_order_relaxed); }

        /** 控制线程：图维度变化后重建 scratch（音频回调内绝不分配）。
            虚拟时钟线程每秒会跑近 200 次主回调，重建期间必须暂停它。 */
        void prepareBuffers()
        {
            suspendVirtualProcessing();
            const auto p = graph_.plan();
            const int chans   = juce::jlimit (1, kMaxChannels, juce::jmax (1, p->channelCount));
            const int inPorts = juce::jlimit (1, kMaxPorts,    juce::jmax (1, p->inputPortCount));
            const int outPorts= juce::jlimit (1, kMaxPorts,    juce::jmax (1, p->outputPortCount));

            channels_.assign (static_cast<size_t> (chans) * 2, std::vector<float> (kMaxBlock, 0.0f));
            inScratch_.assign (static_cast<size_t> (inPorts), std::vector<float> (kMaxBlock, 0.0f));
            outScratch_.assign(static_cast<size_t> (outPorts), std::vector<float> (kMaxBlock, 0.0f));
            recMix_.assign (static_cast<size_t> (kMaxBlock), 0.0f);
            auxIn_.assign (juce::jmax<size_t> (1, auxDevices_.size()), {});
            for (size_t s = 0; s < auxIn_.size(); ++s)
            {
                int ch = 2;
                if (s < auxDevices_.size() && auxDevices_[s]->device != nullptr)
                    ch = juce::jmax (1, auxDevices_[s]->device->inputChannels());
                auxIn_[s].assign (static_cast<size_t> (ch),
                                  std::vector<float> (kMaxBlock, 0.0f));
            }
            zero_.assign (kMaxBlock, 0.0f);
            silent_.assign (kMaxBlock, 0.0f);   // 辅助输入多余通道的只写占位，绝不复用 zero_
            allocChannels_ = chans;
            allocInPorts_  = inPorts;
            allocOutPorts_ = outPorts;

            const double sr = sampleRate();
            if (sr > 8000.0)
            {
                // 内置效果链只在采样率变化时重建（重建会清空滤波器历史）。
                if (std::abs (sr - fxPreparedSr_) > 1.0)
                {
                    fxBank_.prepare (sr, kMaxBlock);
                    fxPreparedSr_ = sr;
                    recorder_.setSampleRate (sr);
                    recScratch_.assign (static_cast<size_t> (kMaxChannels),
                                        std::vector<float> (kMaxBlock, 0.0f));
                }
                // 插件按「真实」采样率与块大小 prepare（内部按参数去重）。
                // 旧版固定传 kMaxBlock=8192，而 processBlock 实际只有 256~512
                // 帧，播放器类插件按错误块长计时会出现进度条不走 / 不出声。
                pluginBank_.prepare (sr, bufferSizeSamples());

                // 「音频播放」解码输出速率对齐设备（媒体播放器按真实块长计时）
                mediaPlayers_.prepare (sr, bufferSizeSamples());
            }
            resumeVirtualProcessing();
        }

        //======================================================================
        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                               int numInputChannels,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            const int n = juce::jmin (numSamples, kMaxBlock);
            const auto plan = graph_.plan();
            const auto routing = routing_.load (std::memory_order_acquire);
            if (plan == nullptr || routing == nullptr) return;

            // 宿主播放头：每个音频块推进一次，插件在 processBlock 里通过
            // getPlayHead()->getPosition() 读到连续递增的时间轴与 isPlaying=true。
            pluginBank_.advance (n);

            for (int c = 0; c < numOutputChannels; ++c)
                if (outputChannelData[c] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[c], n);

            const int nCh    = juce::jmin (plan->channelCount, allocChannels_);
            const int nInP   = juce::jmin (plan->inputPortCount, allocInPorts_);
            const int nOutP  = juce::jmin (plan->outputPortCount, allocOutPorts_);

            // 每通道 2 个缓冲：L = 2c，R = 2c+1（单声道通道的 R 缓冲恒静音）
            for (int b = 0; b < 2 * nCh; ++b)
                juce::FloatVectorOperations::clear (channels_[static_cast<size_t> (b)].data(), n);
            for (int p = 0; p < nInP; ++p)
                juce::FloatVectorOperations::clear (inScratch_[static_cast<size_t> (p)].data(), n);
            for (int p = 0; p < nOutP; ++p)
                juce::FloatVectorOperations::clear (outScratch_[static_cast<size_t> (p)].data(), n);

            // 1) 辅助采集设备：一次拉全部通道（ASIO 多通道不再被截成 2 路）
            for (size_t s = 0; s < routing->aux.size(); ++s)
            {
                const auto& slot = routing->aux[s];
                if (! slot.needInput || slot.device == nullptr || s >= auxIn_.size()) continue;
                float* dst[32];
                const int chans = juce::jmin (32, static_cast<int> (auxIn_[s].size()));
                for (int c = 0; c < 32; ++c)
                    dst[c] = c < chans ? auxIn_[s][static_cast<size_t> (c)].data()
                                       : silent_.data();
                slot.device->pullInput (dst, n);
            }

            // 2) 填充每个输入端口的采样缓冲
            for (int p = 0; p < nInP; ++p)
            {
                if (p >= static_cast<int> (routing->in.size())) break;
                const auto& r = routing->in[static_cast<size_t> (p)];
                float* dst = inScratch_[static_cast<size_t> (p)].data();

                if (r.master)
                {
                    // 虚拟时钟模式两侧通道数为 0 且指针表为 nullptr；
                    // 此时 catalog 里没有主设备，r.master 也不会被置位，这里仍做空指针防护。
                    if (inputChannelData != nullptr && r.masterChannel < numInputChannels
                        && inputChannelData[r.masterChannel] != nullptr)
                        juce::FloatVectorOperations::copy (dst, inputChannelData[r.masterChannel], n);
                }
                else if (r.auxSlot >= 0 && r.auxSlot < static_cast<int> (auxIn_.size()))
                {
                    const auto& buf = auxIn_[static_cast<size_t> (r.auxSlot)];
                    if (! buf.empty())
                    {
                        const int c = juce::jlimit (0, static_cast<int> (buf.size()) - 1, r.auxChannel);
                        juce::FloatVectorOperations::copy (dst, buf[static_cast<size_t> (c)].data(), n);
                    }
                }

                // 输入端口电平（供通道条立体声 L/R 表使用）
                float pv = 0.0f;
                for (int i = 0; i < n; ++i) pv = juce::jmax (pv, std::fabs (dst[i]));
                portPeaks_[static_cast<size_t> (p)].store (pv, std::memory_order_relaxed);
            }

            // 3) P -> C：输入源求和进通道缓冲（e.dst 为缓冲索引 2c/2c+1）
            for (const auto& e : plan->inputFeeds)
                if (e.src < nInP && e.dst < 2 * nCh)
                    juce::FloatVectorOperations::addWithMultiply
                        (channels_[static_cast<size_t> (e.dst)].data(),
                         inScratch_[static_cast<size_t> (e.src)].data(), e.gain, n);

            // 3.4) 「音频播放」：输出进本通道缓冲（与麦克风同路径：
            //      效果链 → 推子 → C→C / C→P 路由，推子直接控制音量）
            mediaPlayers_.render (n, channels_, nCh);

            // 3.5) 通道插入式效果链（立体声 L/R 独立 + 动态联动）：放在出边之前。
            for (int c = 0; c < nCh; ++c)
                fxBank_.processChannel (c,
                                        channels_[static_cast<size_t> (2 * c)].data(),
                                        channels_[static_cast<size_t> (2 * c + 1)].data(), n);

            // 3.6) 通道 VST3 插件链（Inserts，立体声）：同样在出边之前处理。
            for (int c = 0; c < nCh; ++c)
                pluginBank_.processChannel (c,
                                            channels_[static_cast<size_t> (2 * c)].data(),
                                            channels_[static_cast<size_t> (2 * c + 1)].data(), n);

            // 4) C -> C：已按拓扑序排好，src 的贡献先于 dst 的出边（缓冲索引）
            for (const auto& e : plan->channelSends)
                if (e.src < 2 * nCh && e.dst < 2 * nCh)
                    juce::FloatVectorOperations::addWithMultiply
                        (channels_[static_cast<size_t> (e.dst)].data(),
                         channels_[static_cast<size_t> (e.src)].data(), e.gain, n);

            // 5) C -> P：累加到输出端口（e.src 为缓冲索引）
            for (const auto& e : plan->outputSends)
                if (e.src < 2 * nCh && e.dst < nOutP)
                    juce::FloatVectorOperations::addWithMultiply
                        (outScratch_[static_cast<size_t> (e.dst)].data(),
                         channels_[static_cast<size_t> (e.src)].data(), e.gain, n);

            // 5.5) （已移除）播放器直通输出端口 —— 现已改为走通道（见步骤 3.4）

            // 6) 主设备输出
            for (int p = 0; p < nOutP; ++p)
            {
                if (p >= static_cast<int> (routing->out.size())) break;
                const auto& r = routing->out[static_cast<size_t> (p)];
                if (! r.master || outputChannelData == nullptr
                    || r.masterChannel >= numOutputChannels
                    || outputChannelData[r.masterChannel] == nullptr) continue;
                juce::FloatVectorOperations::add
                    (outputChannelData[r.masterChannel], outScratch_[static_cast<size_t> (p)].data(), n);
            }

            // 7) 辅助播放设备：一次推全部通道
            for (const auto& slot : routing->aux)
            {
                if (! slot.needOutput || slot.device == nullptr) continue;
                const float* src[32];
                for (int c = 0; c < 32; ++c) src[c] = zero_.data();
                for (const auto& [port, ch] : slot.outputs)
                    if (port < nOutP && ch >= 0 && ch < 32 && ch < slot.outputChannels)
                        src[ch] = outScratch_[static_cast<size_t> (port)].data();
                slot.device->pushOutput (src, n);
            }

            // 8) 电平表 + 通道录音（效果后、推子后；未激活的通道不录）
            const bool recActive = recorder_.activeCount() > 0;
            const bool sumMode = recActive && recorder_.mode() == mb::ChannelRecorder::Mode::Sum;
            float peak = 0.0f;
            if (sumMode)
                juce::FloatVectorOperations::clear (recMix_.data(), n);
            for (int c = 0; c < nCh; ++c)
            {
                float* bL = channels_[static_cast<size_t> (2 * c)].data();
                float* bR = channels_[static_cast<size_t> (2 * c + 1)].data();
                float pkL = 0.0f, pkR = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    pkL = juce::jmax (pkL, std::fabs (bL[i]));
                    pkR = juce::jmax (pkR, std::fabs (bR[i]));
                }

                // 推子后电平（L/R）：表显跟随通道推子增益；静音/独奏剔除/喇叭关闭 = 0。
                // 缓冲本身不动（推子增益在出边 plan->outputSends 里逐边生效）。
                // 「音频播放」现在走通道（步骤 3.4），其贡献自然计入本表。
                const bool activeCh = plan->active[static_cast<size_t> (c)] != 0;
                const float f = activeCh
                    ? (c < static_cast<int> (plan->faders.size())
                           ? plan->faders[static_cast<size_t> (c)] : 1.0f)
                    : 0.0f;
                const float vL = pkL * f, vR = pkR * f;

                peaksL_[static_cast<size_t> (c)].store (vL, std::memory_order_relaxed);
                peaksR_[static_cast<size_t> (c)].store (vR, std::memory_order_relaxed);
                peaks_[static_cast<size_t> (c)].store (juce::jmax (vL, vR), std::memory_order_relaxed);
                peak = juce::jmax (peak, vL, vR);

                if (recActive && c < kMaxChannels && activeCh)
                {
                    const float g = f;                         // = 推子增益（已含 active 判定）
                    if (sumMode)
                    {
                        for (int i = 0; i < n; ++i) recMix_[i] += bL[i] * g;
                    }
                    else
                    {
                        auto& scratch = recScratch_[static_cast<size_t> (c)];
                        for (int i = 0; i < n; ++i) scratch[i] = bL[i] * g;
                        recorder_.push (c, scratch.data(), n);
                    }
                }
            }
            if (sumMode) recorder_.pushSum (recMix_.data(), n);
            inputPeak_.store (peak, std::memory_order_relaxed);
        }

        void audioDeviceAboutToStart (juce::AudioIODevice*) override
        {
#if JUCE_WINDOWS
            // 记下音频线程句柄（伪句柄需复制后才能跨线程使用），
            // 之后由 UI 定时器周期性重设亲和性掩码。
            void* h = nullptr;
            if (DuplicateHandle (GetCurrentProcess(), GetCurrentThread(),
                                 GetCurrentProcess(), &h, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS))
                audioThreadHandle_.store (h, std::memory_order_release);
#endif
        }

        void audioDeviceStopped() override
        {
#if JUCE_WINDOWS
            if (auto* h = static_cast<HANDLE> (audioThreadHandle_.exchange (nullptr)))
                CloseHandle (h);
#endif
        }

    private:
        //----------------------------------------------------------------------
        /** 「（不使用设备）」模式下的主时钟。不占用任何主设备端点，按固定周期
            调用主回调；两侧通道数传 0，主回调里所有 master 端口分支都不走，
            采样只经 PortRouting 的 aux 槽进出。辅助设备各自跑自己的设备回调，
            StreamRing 的 ASRC 会把它们对齐到这个虚拟时钟（Windows 定时器抖动
            由 ±5% 速率反馈吸收，ring 的 targetFill 留了约 21 ms 缓冲）。 */
        class VirtualClock : public juce::Thread
        {
        public:
            VirtualClock (AudioEngine& owner, int block)
                : juce::Thread ("MicroBridge internal clock"),
                  owner_ (owner),
                  block_ (juce::jlimit (32, 2048, block > 0 ? block : 256))
            {}

            void run() override
            {
                const juce::AudioIODeviceCallbackContext ctx {};
                const double periodMs = 1000.0 * static_cast<double> (block_)
                                      / juce::jmax (8000.0, owner_.sampleRate());
                double nextTick = static_cast<double> (juce::Time::getMillisecondCounterHiRes());

                while (! threadShouldExit())
                {
                    // 控制线程重建缓冲 / 换路由时暂停处理；双检查 + inCallback_
                    // 计数保证「暂停」生效时当前块已经退出，音频线程不加锁。
                    if (owner_.clockSuspend_.load (std::memory_order_acquire) == 0)
                    {
                        owner_.inCallback_.fetch_add (1, std::memory_order_acq_rel);
                        if (owner_.clockSuspend_.load (std::memory_order_acquire) == 0)
                            owner_.audioDeviceIOCallbackWithContext
                                (nullptr, 0, nullptr, 0, block_, ctx);
                        owner_.inCallback_.fetch_sub (1, std::memory_order_acq_rel);
                    }

                    nextTick += periodMs;
                    const double now = static_cast<double> (juce::Time::getMillisecondCounterHiRes());
                    // 系统忙 / 断点导致严重落后时重新对齐，避免追赶风暴把
                    // 环形缓冲抽空（连续欠载会听成断续）。
                    if (nextTick < now - periodMs * 4.0) nextTick = now + periodMs;
                    const double wait = nextTick
                                      - static_cast<double> (juce::Time::getMillisecondCounterHiRes());
                    // Thread::sleep 只有 1 ms 精度：256 f @48k = 5.33 ms，直接取整
                    // 会被睡成 6 ms（慢 ≈12%），超出 StreamRing ASRC 的 ±5% 调节
                    // 上限 → 持续欠载。提前 1.5 ms 醒来，余量用 QPC 高精度自旋
                    // 补齐，保证平均周期不漂移。
                    if (wait > 2.5)
                        sleep (juce::roundToInt (juce::jmin (wait - 1.5, 50.0)));
                    while (! threadShouldExit()
                           && static_cast<double> (juce::Time::getMillisecondCounterHiRes()) < nextTick)
                        juce::Thread::yield();
                }
            }

        private:
            AudioEngine& owner_;
            int block_;
        };

        /** 控制线程改缓冲 / 路由前调用；可嵌套（syncDevices 内还会调 prepareBuffers）。 */
        void suspendVirtualProcessing()
        {
            if (virtualClock_ == nullptr) return;
            clockSuspend_.fetch_add (1, std::memory_order_acq_rel);
            // 等正在执行的那一块退出，最多 50 ms（之后旧 routing 仍由 shared_ptr 保活）。
            for (int i = 0; i < 50 && inCallback_.load (std::memory_order_acquire) > 0; ++i)
                juce::Thread::sleep (1);
        }

        void resumeVirtualProcessing()
        {
            if (clockSuspend_.load (std::memory_order_acquire) > 0)
                clockSuspend_.fetch_sub (1, std::memory_order_acq_rel);
        }

        void startVirtualClock()
        {
            stopVirtualClock();
            clockSuspend_.store (0, std::memory_order_release);
            inCallback_.store   (0, std::memory_order_release);
            virtualClock_ = std::make_unique<VirtualClock>
                (*this, virtualBlock_.load (std::memory_order_acquire));
            virtualClock_->startThread (juce::Thread::Priority::high);
        }

        void stopVirtualClock()
        {
            if (virtualClock_ != nullptr)
            {
                virtualClock_->signalThreadShouldExit();
                virtualClock_->stopThread (3000);
                virtualClock_.reset();
            }
            clockSuspend_.store (0, std::memory_order_release);
            inCallback_.store   (0, std::memory_order_release);
        }

        struct AuxHolder
        {
            std::pair<int, int> key;
            std::unique_ptr<AuxDevice> device;
        };

        static constexpr int kMaxChannels = 32;
        static constexpr int kMaxPorts    = 256;
        static constexpr int kMaxBlock    = 8192;

        void publishing (std::shared_ptr<PortRouting> r)
        {
            routing_.store (std::shared_ptr<const PortRouting> (std::move (r)),
                            std::memory_order_release);
        }

        std::shared_ptr<PortRouting> buildRouting()
        {
            auto r = std::make_shared<PortRouting>();
            r->masterRate = sampleRate() > 0.0 ? sampleRate() : 48000.0;

            const auto& devices = catalog_.devices();
            r->in.resize (static_cast<size_t> (catalog_.inputPortCount()));
            r->out.resize(static_cast<size_t> (catalog_.outputPortCount()));

            struct Need { bool input = false, output = false; };
            std::map<int, Need> needed;
            for (const auto& e : graph_.edges())
            {
                if (e.kind == EdgeKind::InputToChannel)
                    needed[catalog_.portDeviceIndex (true, e.src)].input = true;
                if (e.kind == EdgeKind::ChannelToOutput)
                    needed[catalog_.portDeviceIndex (false, e.dst)].output = true;
            }

            // slot 下标必须与 auxDevices_ 下标一致：prepareBuffers() 按同样的
            // 下标为 auxIn_ 分配通道缓冲。旧版按 di 升序分配 slot，而 auxDevices_
            // 按建连顺序插入，两者不一致时会把输入采样错位到别的槽。
            std::map<int, int> slotOf;
            for (size_t i = 0; i < auxDevices_.size(); ++i)
            {
                const int di = auxDevices_[i]->key.first;
                auto* dev = auxDevices_[i]->device.get();
                AuxSlot slot;
                const bool usable = dev != nullptr && dev->opened()
                                 && di >= 0 && di < static_cast<int> (devices.size())
                                 && ! devices[static_cast<size_t> (di)].isMaster
                                 && needed.count (di) != 0;
                if (usable)
                {
                    slot.device = dev;
                    slot.needInput  = needed[di].input  || dev->needInput();
                    slot.needOutput = needed[di].output || dev->needOutput();
                    slot.inputChannels  = juce::jmax (1, dev->inputChannels());
                    slot.outputChannels = juce::jmax (1, dev->outputChannels());
                    slotOf[di] = static_cast<int> (i);
                }
                r->aux.push_back (slot);   // 占位槽保持下标对齐
            }
            if (r->aux.empty()) r->aux.emplace_back();

            for (size_t p = 0; p < catalog_.inputPorts().size(); ++p)
            {
                const auto& port = catalog_.inputPorts()[p];
                PortRoute route;
                if (port.deviceIndex >= 0 && port.deviceIndex < static_cast<int> (devices.size())
                    && devices[static_cast<size_t> (port.deviceIndex)].isMaster)
                {
                    route.master = true;
                    route.masterChannel = port.channelIndex;
                }
                else if (slotOf.count (port.deviceIndex))
                {
                    const int s = slotOf[port.deviceIndex];
                    route.auxSlot = s;
                    route.auxChannel = port.channelIndex;
                    r->aux[static_cast<size_t> (s)].inputs.push_back ({ static_cast<int> (p), port.channelIndex });
                }
                r->in[p] = route;
            }

            for (size_t p = 0; p < catalog_.outputPorts().size(); ++p)
            {
                const auto& port = catalog_.outputPorts()[p];
                PortRoute route;
                if (port.deviceIndex >= 0 && port.deviceIndex < static_cast<int> (devices.size())
                    && devices[static_cast<size_t> (port.deviceIndex)].isMaster)
                {
                    route.master = true;
                    route.masterChannel = port.channelIndex;
                }
                else if (slotOf.count (port.deviceIndex))
                {
                    const int s = slotOf[port.deviceIndex];
                    route.auxSlot = s;
                    route.auxChannel = port.channelIndex;
                    r->aux[static_cast<size_t> (s)].outputs.push_back ({ static_cast<int> (p), port.channelIndex });
                }
                r->out[p] = route;
            }
            return r;
        }

        mb::RouteGraph& graph_;
        juce::AudioDeviceManager deviceManager_;
        DeviceCatalog catalog_;
        DevicePreferences preferences_;
        juce::String lastError_, fallbackNote_;
        bool running_ = false;

        // 无主设备（「不使用设备」）模式：内部虚拟时钟 + 全部设备走辅助流。
        // rate/block 用原子，sampleRate()/bufferSizeSamples() 可能被音频线程读到。
        std::unique_ptr<VirtualClock> virtualClock_;
        std::atomic<double> virtualRate_   { 0.0 };
        std::atomic<int>    virtualBlock_  { 0 };
        std::atomic<int>    clockSuspend_  { 0 };   // >0 = 暂停处理（可嵌套）
        std::atomic<int>    inCallback_    { 0 };   // 正在执行主回调的块数

        std::vector<std::unique_ptr<AuxHolder>> auxDevices_;
        std::map<int, juce::String> deviceErrors_;
        std::atomic<std::shared_ptr<const PortRouting>> routing_;

        std::vector<std::vector<float>> channels_, inScratch_, outScratch_;
        std::vector<std::vector<std::vector<float>>> auxIn_;
        std::vector<float> zero_, silent_;
        int allocChannels_ = 0, allocInPorts_ = 0, allocOutPorts_ = 0;

        mutable std::array<std::atomic<float>, kMaxChannels> peaks_;     // 推子后电平（表显，max(L,R)）
        mutable std::array<std::atomic<float>, kMaxChannels> peaksL_;    // 推子后 L
        mutable std::array<std::atomic<float>, kMaxChannels> peaksR_;    // 推子后 R
        mutable std::array<std::atomic<float>, kMaxPorts> portPeaks_;
        std::atomic<float> inputPeak_ { 0.0f };

        // 通道效果链 / 录音 / VST3 插件
        mb::ChannelFxBank fxBank_;
        mb::ChannelRecorder recorder_;
        mb::PluginCatalog pluginCatalog_;
        mb::ChannelPluginBank pluginBank_;
        mb::MediaPlayerBank mediaPlayers_;       // 「音频播放」自带插件
        std::vector<std::vector<float>> recScratch_;
        std::vector<float> recMix_;                  // 合成模式：布防通道相加
        double fxPreparedSr_ = 0.0;

        // 音频线程亲和性（Windows）
        std::atomic<void*> audioThreadHandle_ { nullptr };
        unsigned affinityRotation_ = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
    };
}
