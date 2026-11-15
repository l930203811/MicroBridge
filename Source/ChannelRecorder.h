#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

//==============================================================================
// 通道录音（参考主流机架的通道录制：录音分组 / 目录 / 格式）
//
// · 每通道一个 WAV 写出器，音频回调把「效果后、推子后」的通道信号
//   经 ThreadedWriter 的无锁 FIFO 送后台线程写盘，音频线程零阻塞。
// · 单声道 24-bit WAV，文件名 = 通道名_时间戳.wav，落在录音目录
//   （默认 ~/Music/MicroBridge，可在诊断面板里更换）。
// · stop 在消息线程等待超过两个最大块周期后再析构写出器，
//   保证音频线程不再持有指针。
//==============================================================================
namespace mb
{
    class ChannelRecorder
    {
    public:
        static constexpr int kMaxChannels = 32;

        /** 录音模式：Sum = 多轨合成单文件（默认），MultiTrack = 每通道独立文件。 */
        enum class Mode { Sum, MultiTrack };

        ChannelRecorder()
        {
            sliceThread_.startThread (juce::Thread::Priority::normal);
        }

        ~ChannelRecorder()
        {
            stopAll();
            sliceThread_.stopThread (2000);
        }

        juce::File directory() const
        {
            if (dirOverride_.isNotEmpty())
                return juce::File (dirOverride_);
            return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                    .getChildFile ("MicroBridge");
        }

        void setDirectory (const juce::File& d) { dirOverride_ = d.getFullPathName(); }

        void setSampleRate (double sr) { if (sr > 8000.0) sr_ = sr; }

        // ---- 录音设置（由设置页应用）----------------------------------------
        void setMode (Mode m) noexcept
        {
            mode_.store (m == Mode::MultiTrack ? 1 : 0, std::memory_order_relaxed);
        }
        Mode mode() const noexcept
        {
            return mode_.load (std::memory_order_relaxed) != 0 ? Mode::MultiTrack : Mode::Sum;
        }

        /** 格式："WAV" / "FLAC" / "AIFF"，其它值按 WAV 处理。 */
        void setFormat (const juce::String& f) { format_ = f.toUpperCase(); }
        /** 位深：16 或 24（FLAC / WAV / AIFF 均支持）。 */
        void setBitDepth (int bits) { if (bits == 16 || bits == 24) bitDepth_ = bits; }

        bool isRecording (int ch) const
        {
            return ch >= 0 && ch < kMaxChannels && slots_[static_cast<size_t> (ch)].active.load();
        }

        /** 暂停 / 恢复：暂停期间音频线程丢弃推送样本，文件与布防状态保持，
            恢复后接续写同一批文件（时间轴上留白 = 暂停时长）。 */
        void setPaused (bool p) noexcept { paused_.store (p, std::memory_order_relaxed); }
        bool isPaused() const noexcept { return paused_.load (std::memory_order_relaxed); }

        int activeCount() const
        {
            int n = 0;
            for (const auto& s : slots_) if (s.active.load()) ++n;
            return n;
        }

        /** 消息线程：布防一个通道。Sum 模式只建一次共享写出器；MultiTrack 每通道一个文件。 */
        bool start (int ch, const juce::String& channelName, double sampleRate)
        {
            if (ch < 0 || ch >= kMaxChannels || slots_[static_cast<size_t> (ch)].active.load())
                return false;
            if (sampleRate > 8000.0) sr_ = sampleRate;

            const auto dir = directory();
            if (! dir.createDirectory().wasOk()) return false;

            auto& slot = slots_[static_cast<size_t> (ch)];

            if (mode() == Mode::Sum)
            {
                // 多轨合成单文件：第一个布防通道创建共享写出器
                if (sumWriter_.load (std::memory_order_acquire) == nullptr)
                {
                    auto file = dir.getChildFile (mb::S("Mix_") + timeStamp() + ext());
                    int n = 2;
                    while (file.existsAsFile())
                        file = dir.getChildFile (mb::S("Mix_") + timeStamp()
                                                 + "-" + juce::String (n++) + ext());
                    auto writer = createWriter (file, sr_, bitDepth_);
                    if (writer == nullptr) return false;
                    sumFile_ = file.getFullPathName();
                    sumWriter_.store (
                        new juce::AudioFormatWriter::ThreadedWriter (
                            writer.release(), sliceThread_, 65536),
                        std::memory_order_release);
                }
                slot.fileName = sumFile_;
                slot.active.store (true, std::memory_order_release);
                sumRefs_.fetch_add (1, std::memory_order_acq_rel);
                return true;
            }

            // 分轨：每通道独立文件（文件名 = 通道名_时间戳.扩展名）
            const auto base = sanitise (channelName.isEmpty()
                                        ? "CH" + juce::String (ch + 1) : channelName);
            auto file = dir.getChildFile (base + "_" + timeStamp() + ext());
            int n = 2;
            while (file.existsAsFile())
                file = dir.getChildFile (base + "_" + timeStamp() + "-" + juce::String (n++) + ext());

            auto writer = createWriter (file, sr_, bitDepth_);
            if (writer == nullptr) return false;

            auto* threaded = new juce::AudioFormatWriter::ThreadedWriter (
                writer.release(), sliceThread_, 65536);

            slot.fileName = file.getFullPathName();
            slot.writer.store (threaded, std::memory_order_release);
            slot.active.store (true, std::memory_order_release);
            return true;
        }

        /** 消息线程：停止并落盘。返回写出的文件（失败返回空路径）。 */
        juce::File stop (int ch)
        {
            if (ch < 0 || ch >= kMaxChannels) return {};
            auto& slot = slots_[static_cast<size_t> (ch)];
            if (! slot.active.load (std::memory_order_acquire)) return {};

            slot.active.store (false, std::memory_order_release);
            // 音频回调最长一块 = kMaxBlock(8192) @44.1k ≈ 186 ms；等够再析构。
            juce::Thread::sleep (250);

            auto* w = slot.writer.exchange (nullptr, std::memory_order_acquire);
            delete w;                                   // ThreadedWriter 析构时冲刷 FIFO

            // 合成模式：最后一个布防通道停止时销毁共享写出器
            if (mode() == Mode::Sum
                && sumRefs_.load (std::memory_order_acquire) > 0
                && sumRefs_.fetch_sub (1, std::memory_order_acq_rel) == 1)
            {
                auto* sw = sumWriter_.exchange (nullptr, std::memory_order_acquire);
                delete sw;
                sumFile_.clear();
            }

            return juce::File (slot.fileName);
        }

        void stopAll()
        {
            for (int i = 0; i < kMaxChannels; ++i)
                if (slots_[static_cast<size_t> (i)].active.load())
                    stop (i);
        }

        /** 音频线程：推一个通道的样本块（已含推子增益），FIFO 满则丢弃。 */
        void push (int ch, const float* data, int n) noexcept
        {
            if (paused_.load (std::memory_order_relaxed)) return;
            if (ch < 0 || ch >= kMaxChannels || data == nullptr) return;
            const auto& slot = slots_[static_cast<size_t> (ch)];
            if (! slot.active.load (std::memory_order_relaxed)) return;

            auto* w = slot.writer.load (std::memory_order_acquire);
            if (w == nullptr) return;
            const float* ptrs[1] = { data };
            w->write (ptrs, static_cast<int> (n));
        }

        /** 音频线程：推合成模式的混合信号（所有布防通道已相加），FIFO 满则丢弃。 */
        void pushSum (const float* data, int n) noexcept
        {
            if (paused_.load (std::memory_order_relaxed)) return;
            auto* w = sumWriter_.load (std::memory_order_acquire);
            if (w == nullptr || data == nullptr) return;
            const float* ptrs[1] = { data };
            w->write (ptrs, static_cast<int> (n));
        }

    private:
        struct Slot
        {
            std::atomic<bool> active { false };
            std::atomic<juce::AudioFormatWriter::ThreadedWriter*> writer { nullptr };
            juce::String fileName;
        };

        juce::String ext() const
        {
            if (format_ == "AIFF") return ".aiff";
            if (format_ == "FLAC") return ".flac";
            return ".wav";
        }

        std::unique_ptr<juce::AudioFormatWriter> createWriter (const juce::File& file,
                                                               double sr, int bits)
        {
            std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
            if (stream == nullptr) return nullptr;

            const auto e = file.getFileExtension().toLowerCase();
            if (e == ".flac")
                return std::unique_ptr<juce::AudioFormatWriter> (
                    juce::FlacAudioFormat().createWriterFor (stream.release(), sr, 1, bits, {}, 0));
            if (e == ".aiff" || e == ".aif")
                return std::unique_ptr<juce::AudioFormatWriter> (
                    juce::AiffAudioFormat().createWriterFor (stream.release(), sr, 1, bits, {}, 0));
            return std::unique_ptr<juce::AudioFormatWriter> (
                juce::WavAudioFormat().createWriterFor (stream.release(), sr, 1, bits, {}, 0));
        }

        static juce::String sanitise (juce::String s)
        {
            s = s.replaceCharacters ("/\\:*?\"<>|", "__________").trim();
            if (s.isEmpty()) s = "CH";
            return s;
        }

        static juce::String timeStamp()
        {
            return juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
        }

        juce::TimeSliceThread sliceThread_ { "MicroBridge Recorder" };
        std::array<Slot, kMaxChannels> slots_;
        juce::String dirOverride_;
        double sr_ = 48000.0;

        std::atomic<int> mode_ { 0 };                // 0 = Sum（默认），1 = MultiTrack
        std::atomic<juce::AudioFormatWriter::ThreadedWriter*> sumWriter_ { nullptr };
        std::atomic<int> sumRefs_ { 0 };
        juce::String sumFile_;
        juce::String format_ { "WAV" };
        int bitDepth_ = 24;
        std::atomic<bool> paused_ { false };         // 暂停：丢弃推送样本，会话保持

        JUCE_DECLARE_NON_COPYABLE (ChannelRecorder)
    };
}
