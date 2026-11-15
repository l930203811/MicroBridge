#pragma once

// 系统声音回环捕获（WASAPI Loopback）—— 虚拟输入设备
//
// · 独立捕获线程：COM(MTA) → 默认渲染端点 → IAudioClient(SHARED | LOOPBACK)
//   → IAudioCaptureClient，轮询取包；mix format 前两路转 float、去交织后
//   推入宿主提供的 StreamRing。
// · 与 AuxDevice 复用同一套 ASRC 拉取路径：AuxDevice::pullInput 按
//   deviceRate_/masterRate_ 从 inRing_ 读取，主时钟节奏不受渲染端点
//   采样率影响；本类只负责往 ring 里灌数据。
// · 无人渲染时系统不产生数据包，线程保持 5ms 低频轮询；端点失效
//   （切换默认设备 / 独占模式抢占）自动按 500ms 周期重建。仅 Windows。
//
// 注意：本文件必须在 StreamRing 定义之后、AuxDevice 定义之前被
// AudioEngine.h 包含（sink 类型 StreamRing 不在此声明）。本文件位于
// namespace mb 内部包含（AudioEngine.h），因此不再自行开命名空间。

#if JUCE_WINDOWS

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstring>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>

class LoopbackDevice : private juce::Thread
{
public:
    LoopbackDevice() : juce::Thread ("MicroBridge Loopback Capture")
    {
        startThread (juce::Thread::Priority::normal);
        // 同步等待首次初始化（AuxDevice 构造期要立刻知道能不能用）
        for (int i = 0; i < 300; ++i)
        {
            if (ok_.load (std::memory_order_acquire)) { error_ = {}; return; }
            if (error_.isNotEmpty()) return;
            juce::Thread::sleep (10);
        }
        if (error_.isEmpty()) error_ = tr ("回环初始化超时", "Loopback init timeout");
    }

    ~LoopbackDevice() override
    {
        shutdownEndpoint();
        stopThread (2000);
    }

    bool ok() const noexcept { return ok_.load (std::memory_order_acquire); }
    const juce::String& error() const noexcept { return error_; }
    double sampleRate() const noexcept { return rate_.load (std::memory_order_acquire); }

    /** 控制线程一次性设置（AuxDevice 构造期）。 */
    void setSink (StreamRing* r) noexcept { sink_ = r; }

private:
    void run() override
    {
        // 每线程初始化 COM(MTA)。故意不配 CoUninitialize：线程退出时
        // COM 自行清理，避免析构顺序问题（与 MFAudioFormat.h 同款）。
        ::CoInitializeEx (nullptr, COINIT_MULTITHREADED);

        while (! threadShouldExit())
        {
            if (! initEndpoint())
            {
                for (int i = 0; i < 50 && ! threadShouldExit(); ++i)
                    juce::Thread::sleep (10);          // 失败后 500ms 重试
                continue;
            }

            ok_.store (true, std::memory_order_release);
            error_ = {};
            while (! threadShouldExit() && alive_)
            {
                pump();
                juce::Thread::sleep (5);               // 轮询（loopback 无事件通知）
            }
            ok_.store (false, std::memory_order_release);
            shutdownEndpoint();
        }
    }

    bool initEndpoint()
    {
        shutdownEndpoint();

        IMMDeviceEnumerator* en = nullptr;
        HRESULT hr = ::CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr,
                                         CLSCTX_ALL, __uuidof (IMMDeviceEnumerator),
                                         (void**) &en);
        if (FAILED (hr))
        {
            error_ = tr ("COM/MMDevice 初始化失败", "COM/MMDevice init failed");
            return false;
        }

        IMMDevice* dev = nullptr;
        hr = en->GetDefaultAudioEndpoint (eRender, eConsole, &dev);
        en->Release();
        if (FAILED (hr))
        {
            error_ = tr ("找不到默认播放设备", "No default render endpoint");
            return false;
        }

        IAudioClient* ac = nullptr;
        hr = dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &ac);
        dev->Release();
        if (FAILED (hr))
        {
            error_ = tr ("播放端点激活失败", "Endpoint activation failed");
            return false;
        }

        WAVEFORMATEX* wfx = nullptr;
        hr = ac->GetMixFormat (&wfx);
        if (FAILED (hr) || wfx == nullptr)
        {
            ac->Release();
            error_ = tr ("无法读取混音格式", "Mix format unavailable");
            return false;
        }

        bool isFloat = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22)
            isFloat = subFormatIsFloat (*reinterpret_cast<WAVEFORMATEXTENSIBLE*> (wfx));

        // 回环捕获：渲染端点上开共享模式采集；缓冲时长 0 = 系统默认周期
        hr = ac->Initialize (AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK,
                             0, 0, wfx, nullptr);
        const double rate = wfx->nSamplesPerSec;
        const int chans = juce::jlimit (1, 8, (int) wfx->nChannels);
        const int bits = wfx->wBitsPerSample;
        ::CoTaskMemFree (wfx);
        if (FAILED (hr))
        {
            ac->Release();
            error_ = tr ("回环流初始化失败（共享模式被占用？）",
                         "Loopback init failed (shared mode busy?)");
            return false;
        }

        IAudioCaptureClient* cap = nullptr;
        hr = ac->GetService (__uuidof (IAudioCaptureClient), (void**) &cap);
        if (FAILED (hr))
        {
            ac->Release();
            error_ = tr ("无法获取采集客户端", "Capture client unavailable");
            return false;
        }

        if (FAILED (ac->Start()))
        {
            cap->Release();
            ac->Release();
            error_ = tr ("回环采集启动失败", "Loopback start failed");
            return false;
        }

        client_ = ac;
        capture_ = cap;
        srcChans_ = chans;
        floatFormat_ = isFloat;
        pcmBits_ = bits;
        rate_.store (rate, std::memory_order_release);
        alive_ = true;
        return true;
    }

    /** 取完当前全部数据包；端点失效时置 alive_ = false 让外层重建。 */
    void pump()
    {
        UINT32 packet = 0;
        if (capture_ == nullptr || FAILED (capture_->GetNextPacketSize (&packet)))
        {
            alive_ = false;
            return;
        }

        while (packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED (capture_->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
            {
                alive_ = false;
                return;
            }

            if (sink_ != nullptr && frames > 0)
            {
                scratch_.assign (static_cast<size_t> (frames) * 2, 0.0f);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data != nullptr)
                    deinterleave (data, frames);
                const float* ch[2] = { scratch_.data(),
                                       scratch_.data() + frames };
                sink_->push (ch, 2, (int) frames);
            }

            capture_->ReleaseBuffer (frames);
            if (FAILED (capture_->GetNextPacketSize (&packet)))
            {
                alive_ = false;
                return;
            }
        }
    }

    /** mix format → float 立体声（取前两路；通道不足的填 0）。 */
    void deinterleave (const BYTE* data, UINT32 frames)
    {
        const int nch = srcChans_;
        if (floatFormat_)
        {
            const auto* s = reinterpret_cast<const float*> (data);
            for (UINT32 i = 0; i < frames; ++i)
                for (int c = 0; c < 2; ++c)
                    scratch_[static_cast<size_t> (i) + static_cast<size_t> (c) * frames]
                        = c < nch ? s[static_cast<size_t> (i) * nch + c] : 0.0f;
        }
        else if (pcmBits_ == 32)
        {
            const auto* s = reinterpret_cast<const int32_t*> (data);
            for (UINT32 i = 0; i < frames; ++i)
                for (int c = 0; c < 2; ++c)
                    scratch_[static_cast<size_t> (i) + static_cast<size_t> (c) * frames]
                        = c < nch ? s[static_cast<size_t> (i) * nch + c] / 2147483648.0f : 0.0f;
        }
        else   // 16-bit PCM 兜底
        {
            const auto* s = reinterpret_cast<const int16_t*> (data);
            for (UINT32 i = 0; i < frames; ++i)
                for (int c = 0; c < 2; ++c)
                    scratch_[static_cast<size_t> (i) + static_cast<size_t> (c) * frames]
                        = c < nch ? s[static_cast<size_t> (i) * nch + c] / 32768.0f : 0.0f;
        }
    }

    static bool subFormatIsFloat (const WAVEFORMATEXTENSIBLE& ex) noexcept
    {
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00aa00389b71}
        static const GUID ieee = { 0x00000003, 0x0000, 0x0010,
                                   { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
        return ex.SubFormat.Data1 == ieee.Data1
            && ex.SubFormat.Data2 == ieee.Data2
            && ex.SubFormat.Data3 == ieee.Data3
            && std::memcmp (ex.SubFormat.Data4, ieee.Data4, 8) == 0;
    }

    void shutdownEndpoint()
    {
        if (client_ != nullptr) client_->Stop();
        if (capture_ != nullptr) { capture_->Release(); capture_ = nullptr; }
        if (client_ != nullptr) { client_->Release(); client_ = nullptr; }
        alive_ = false;
    }

    StreamRing* sink_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    std::atomic<bool> ok_ { false };
    std::atomic<bool> alive_ { false };
    std::atomic<double> rate_ { 48000.0 };
    int srcChans_ = 2;
    int pcmBits_ = 16;
    bool floatFormat_ = true;
    juce::String error_;
    std::vector<float> scratch_;
};

#else   // 非 Windows：占位（项目按 Windows 发布，正常不会走到）
class LoopbackDevice
{
public:
    bool ok() const noexcept { return false; }
    const juce::String& error() const noexcept { return error_; }
    double sampleRate() const noexcept { return 48000.0; }
    void setSink (void*) noexcept {}
private:
    juce::String error_ { "Loopback requires Windows" };
};
#endif
