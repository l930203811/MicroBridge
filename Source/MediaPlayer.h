#pragma once

// 每通道「音频播放」自带插件引擎 —— 移植自 S-Rack 直读播放架构
//
// · 架构（与 S-Rack 同款，久经实测）：消息线程只负责把文件路径排队；音频线程
//   创建 / 接管 / 释放 / 直读解码 —— 无后台解码线程、无环形缓冲、无重采样组件。
//   MF 解码远快于实时，音频线程直读足够。关键：reader 与使用它的读取必须同处
//   音频线程的 MTA 公寓，否则跨公寓封送会让读取偶发阻塞 → 卡碟式爆音。
//   （上一版移植引入的「解码线程 + 环形缓冲 + ResamplingAudioSource」
//   实测爆音 + 卡死，全部废弃。）
// · 走通道：解码出的立体声 L/R 分别写入本通道的 L/R 缓冲（2c = L，2c+1 = R），
//   与麦克风输入同一路径 —— 经过通道效果链、推子、静音与 C→C/C→P 路由，
//   通道推子直接控制音量，也可把本通道送到其它通道作输入源。
//   播放器自己的音量滑条作用在通道之前（预增益）；旁通（插件圆点）=
//   静音但时间轴继续走，取消旁通从当前位置无缝接上。
// · 防爆音三重保险：解码样本钳制到 ±1、输出贡献软限幅（tanh 膝区）、
//   音量起停按样本斜坡。
// · EOF 判定：read 失败 / 已知长度读满 / 连续全静音块（未知时长兜底），
//   置 exhausted 由 Bank 定时器按播放模式自动切曲。
// · 全部控制 API 仅限消息线程调用；render 仅限音频线程。

#include <JuceHeader.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "MFAudioFormat.h"

namespace mb
{
    class MediaPlayerBank : private juce::Timer
    {
    public:
        static constexpr int kMaxChannels = 32;
        static constexpr int kMaxBlock    = 8192;    // 解码暂存窗容量（≥ 设备最大块长）

        struct Player;                          // 前置声明：ensurePlayer 等接口在 Player 定义之前引用它

        enum { kSequential = 0, kListLoop = 1, kSingleLoop = 2, kRandom = 3 };

        MediaPlayerBank()
        {
            // 解码暂存窗一次性按最大块长分配（音频线程直读用，绝不中途重分配）
            scratch_ = std::make_unique<juce::AudioBuffer<float>> (2, kMaxBlock);
            scratchFrames_ = kMaxBlock;
            startTimerHz (10);                      // 自动切曲 + 退役回收轮询
        }

        ~MediaPlayerBank() override
        {
            stopTimer();
            std::vector<std::unique_ptr<Player>> dead;
            {
                const juce::ScopedLock sl (lock_);
                dead = std::move (players_);
                for (auto& p : players_) p = nullptr;   // unique_ptr 不可拷贝赋值，只能逐个 move 置空
                for (auto& r : retired_) dead.push_back (std::move (r.p));
                retired_.clear();
            }
            dead.clear();                           // Player 析构内部释放 reader
        }

        //==================================================================
        // 控制线程接口（消息线程）
        //==================================================================
        Player& ensurePlayer (int ch)
        {
            const juce::ScopedLock sl (lock_);
            if (ch < 0 || ch >= kMaxChannels) return dummy_;
            if (players_[static_cast<size_t> (ch)] == nullptr)
            {
                players_[static_cast<size_t> (ch)] = std::make_unique<Player> (this, ch);
                publishSnapshot();
            }
            return *players_[static_cast<size_t> (ch)];
        }

        bool hasPlayer (int ch) const
        {
            const juce::ScopedLock sl (lock_);
            return ch >= 0 && ch < kMaxChannels
                && players_[static_cast<size_t> (ch)] != nullptr;
        }

        Player* playerPtr (int ch) const
        {
            const juce::ScopedLock sl (lock_);
            if (ch < 0 || ch >= kMaxChannels) return nullptr;
            return players_[static_cast<size_t> (ch)].get();
        }

        /** 撤销通道的播放器（移除插件时）：清列表 + 停播（保留对象供音频线程收尾）。 */
        void clearPlayer (int ch)
        {
            const juce::ScopedLock sl (lock_);
            if (ch < 0 || ch >= kMaxChannels) return;
            if (auto& p = players_[static_cast<size_t> (ch)])
                p->resetAll();
        }

        /** 新建空白预设：停播、清列表并销毁全部播放器，同时把音频线程快照
            全部置空。AudioEngine 每块无条件调用 render() 遍历 snap_，只做
            resetAll() 会留下对象继续往通道缓冲写音频、电平表继续跳动。 */
        void clearAll()
        {
            const juce::ScopedLock sl (lock_);
            for (auto& p : players_)
            {
                if (p == nullptr) continue;
                p->resetAll();
                retire (std::move (p));       // 延迟 1s 销毁，音频线程不会读到悬空对象
            }
            publishSnapshot();                // snap_ 全部为 nullptr → 立即静默
        }

        /** 删除通道：销毁该通道播放器并把后面的整体前移。 */
        void removeChannel (int ch)
        {
            const juce::ScopedLock sl (lock_);
            if (ch < 0 || ch >= kMaxChannels) return;
            retire (std::move (players_[static_cast<size_t> (ch)]));
            players_.erase (players_.begin() + ch);
            players_.push_back (nullptr);
            publishSnapshot();
        }

        /** 通道重排（order[newPos] = oldIdx）：播放器跟随通道移动。 */
        void reorderChannels (const std::vector<int>& order)
        {
            const juce::ScopedLock sl (lock_);
            if (order.empty() || static_cast<int> (order.size()) > kMaxChannels) return;
            std::vector<std::unique_ptr<Player>> next (kMaxChannels);
            for (int i = 0; i < static_cast<int> (order.size()); ++i)
            {
                const int old = order[static_cast<size_t> (i)];
                if (old < 0 || old >= kMaxChannels) return;
                next[static_cast<size_t> (i)] = std::move (players_[static_cast<size_t> (old)]);
            }
            for (int i = 0; i < kMaxChannels; ++i)
                if (players_[static_cast<size_t> (i)] != nullptr)     // 被顶掉的通道
                    retire (std::move (players_[static_cast<size_t> (i)]));
            players_ = std::move (next);
            publishSnapshot();
        }

        /** 复制通道（dst = src）：拷贝播放列表与全部设置，不复制播放进度。 */
        void copyConfig (int src, int dst)
        {
            Player* s = nullptr;
            {
                const juce::ScopedLock sl (lock_);
                if (src < 0 || src >= kMaxChannels || dst < 0 || dst >= kMaxChannels) return;
                s = players_[static_cast<size_t> (src)].get();
            }
            if (s == nullptr) return;
            ensurePlayer (dst).copyFrom (*s);
        }

        /** 旁通联动（消息线程，PluginBank 钩子转发）：静音但时间轴继续走。 */
        void setBypassMuted (int ch, bool muted)
        {
            const juce::ScopedLock sl (lock_);
            if (ch < 0 || ch >= kMaxChannels) return;
            if (auto* p = players_[static_cast<size_t> (ch)].get())
                p->muted.store (muted, std::memory_order_relaxed);
        }

        void prepare (double sampleRate, int blockSize)
        {
            if (sampleRate <= 8000.0) return;
            deviceRate_.store (sampleRate, std::memory_order_release);
            deviceBlock_.store (juce::jlimit (32, kMaxBlock, blockSize > 0 ? blockSize : 512),
                                std::memory_order_release);
        }

        double deviceRate() const noexcept { return deviceRate_.load (std::memory_order_acquire); }

        //==================================================================
        // 音频线程
        //==================================================================
        /** 把每个播放器的输出叠加进其所属通道的缓冲（步骤 3 之后、效果链之前，
            与麦克风输入同路径：经过效果链 → 推子 → 路由）。 */
        void render (int numSamples, std::vector<std::vector<float>>& chans, int nChan) noexcept
        {
            for (auto& p : snap_)
                if (auto* q = p.load (std::memory_order_relaxed))
                    q->render (numSamples, chans, nChan);
        }

    private:
        struct Retired { juce::uint32 at; std::unique_ptr<Player> p; };
        std::vector<Retired> retired_;

        void retire (std::unique_ptr<Player> p)
        {
            if (p != nullptr)
                retired_.push_back ({ juce::Time::getMillisecondCounter(), std::move (p) });
        }

        /** snap_ 只在 lock_ 内重写；音频线程经原子读。 */
        void publishSnapshot()
        {
            for (int i = 0; i < kMaxChannels; ++i)
                snap_[static_cast<size_t> (i)].store (
                    players_[static_cast<size_t> (i)].get(), std::memory_order_relaxed);
        }

        void timerCallback() override
        {
            {
                const juce::ScopedLock sl (lock_);
                publishSnapshot();
                // 退役回收：音频线程确认早已离开旧快照（1 s 宽限）
                const auto cutoff = juce::Time::getMillisecondCounter() - 1000;
                for (auto it = retired_.begin(); it != retired_.end();)
                {
                    if (static_cast<std::int32_t> (it->at - cutoff) < 0)
                        it = retired_.erase (it);
                    else
                        ++it;
                }
            }
            std::vector<Player*> live;
            {
                const juce::ScopedLock sl (lock_);
                for (auto& p : players_)
                    if (p != nullptr) live.push_back (p.get());
            }
            for (auto* p : live)
                p->serviceAutoAdvance();
        }

    public:
        //==================================================================
        struct Player
        {
            Player (MediaPlayerBank* bank, int ch)
                : owner (bank), channel (ch)
            {
                tapL.assign (kTapFrames, 0.0f);
                tapR.assign (kTapFrames, 0.0f);
            }

            ~Player()
            {
                // 关停时音频回调已停（引擎先停设备 / 虚拟时钟），安全释放。
                // 音频线程独占 activeReader_，直接释放即可；未接管的加载请求随
                // pendingFileStr_ 一起丢弃（不会创建 reader）。
                delete activeReader_;
                activeReader_ = nullptr;
            }

            static constexpr int kTapFrames = 8192;      // 频谱采样环

            MediaPlayerBank* owner = nullptr;
            int channel = -1;

            // ---- 控制线程持有（消息线程读写）----
            std::vector<juce::File> files;
            int failStreak = 0;                          // 连续加载失败计数（自动跳曲上限）

            // ---- 原子共享：控制写 / 音频与 UI 读 ----
            std::atomic<int>   currentIdx { -1 };
            std::atomic<bool>  wantPlay { false };
            std::atomic<float> volume { 0.8f };          // S-Rack 同款默认 0.8
            std::atomic<bool>  muted { false };
            std::atomic<int>   mode { kListLoop };
            std::atomic<int>   offsetMs { 0 };

            // ---- reader 交接：消息线程只排队文件路径，音频线程创建 / 接管 / 释放 ----
            // （关键：MF SourceReader 必须与使用它的线程同处一个 COM 公寓（MTA）。
            //  若消息线程（STA）创建、音频线程（MTA）读取，跨公寓封送会让读取偶发
            //  阻塞 → 数据断供 → 卡碟式爆音。S-Rack 同款：全程音频线程。）
            std::atomic<bool>   pendingLoad { false };   // 音频线程尚未创建 reader（请求加载）
            juce::CriticalSection loadCs_;               // 保护 pendingFileStr_ 跨线程交接
            juce::String        pendingFileStr_;         // 仅消息线程写 / 音频线程读
            std::atomic<bool>   pendingUnload { false };
            std::atomic<double> pendingStartSec { -1.0 };
            std::atomic<bool>   playAfterLoad { false };
            std::atomic<bool>   loading { false };       // 加载排队中（音频线程尚未创建 reader）
            std::atomic<double> seekReqSec { -1.0 };     // seek 请求（音频线程应用）

            // ---- 音频线程写 / UI 读 ----
            std::atomic<bool>   readerReady { false };
            std::atomic<bool>   exhausted { false };     // 播放中耗尽（待自动切曲）
            std::atomic<double> trackLengthSec { 0.0 };
            std::atomic<double> currentSeconds { 0.0 };
            std::atomic<juce::uint64> tapWritten { 0 };  // 频谱环累计写帧数
            std::vector<float> tapL, tapR;               // 频谱采样环（UI 只读）

            //================================================================
            // 控制线程 API
            //================================================================
            void copyFrom (const Player& src)
            {
                files = src.files;
                volume.store (src.volume.load());
                mode.store (src.mode.load());
                offsetMs.store (src.offsetMs.load());
                // 不复制播放状态：副本从静止开始
                currentIdx.store (-1);
                wantPlay.store (false);
            }

            /** 停播并卸载当前曲，但保留播放列表。 */
            void stopAndUnload()
            {
                wantPlay.store (false);
                currentIdx.store (-1);
                readerReady.store (false);
                exhausted.store (false);
                playAfterLoad.store (false);
                loading.store (false);
                trackLengthSec.store (0.0);
                seekReqSec.store (-1.0);
                pendingLoad.store (false);           // 取消未消费的加载请求
                {
                    const juce::ScopedLock sl (loadCs_);
                    pendingFileStr_.clear();
                }
                pendingUnload.store (true);          // 音频线程下个块释放已接管的 reader
            }

            void resetAll()
            {
                stopAndUnload();
                files.clear();
            }

            void setPlaylist (std::vector<juce::File> list) { files = std::move (list); }

            /** 换曲并按 startPlay 决定是否立即播放；startSec < 0 = 从头。
                消息线程只把文件路径排队，reader 由音频线程（MTA）创建并直读——
                与 S-Rack 同款，避免跨 COM 公寓封送导致卡碟爆音。 */
            void loadTrack (int idx, bool startPlay, double startSec = -1.0)
            {
                if (idx < 0 || idx >= static_cast<int> (files.size())) return;
                currentIdx.store (idx);
                wantPlay.store (false);
                exhausted.store (false);
                playAfterLoad.store (startPlay);
                pendingStartSec.store (startSec);
                pendingUnload.store (false);

                // 丢弃尚未被音频线程接管的旧请求（避免把上一首 reader 又建一次）
                pendingLoad.store (false);
                {
                    const juce::ScopedLock sl (loadCs_);
                    pendingFileStr_.clear();
                }

                const juce::File file = files[static_cast<size_t> (idx)];
                {
                    const juce::ScopedLock sl (loadCs_);
                    pendingFileStr_ = file.getFullPathName();
                }
                loading.store (true);
                pendingLoad.store (true);            // 音频线程下个块创建 reader
            }

            void play()
            {
                if (currentIdx.load() < 0)
                {
                    if (! files.empty()) { loadTrack (0, true); return; }
                    return;
                }
                if (loading.load()) { playAfterLoad.store (true); return; }
                if (! readerReady.load())            // reader 未就绪（如会话恢复后未加载）
                    { loadTrack (currentIdx.load(), true); return; }
                wantPlay.store (true);
            }

            void pause()  { wantPlay.store (false); }
            void toggle() { wantPlay.load() ? pause() : play(); }

            void seekTo (double sec)
            {
                if (currentIdx.load() < 0 || loading.load()) return;
                seekReqSec.store (juce::jmax (0.0, sec));
                exhausted.store (false);
            }

            double positionSec() const { return currentSeconds.load (std::memory_order_relaxed); }

            void advanceTrack (bool forward)
            {
                const int n = static_cast<int> (files.size());
                if (n == 0) { wantPlay.store (false); return; }
                const int m = mode.load();
                const int cur = currentIdx.load();

                if (m == kSingleLoop && cur >= 0) { loadTrack (cur, true, 0.0); return; }

                int next = cur;
                if (m == kRandom && n > 1)
                {
                    do { next = juce::Random::getSystemRandom().nextInt (n); }
                    while (next == cur);
                }
                else
                {
                    next = cur + (forward ? 1 : -1);
                    if (next >= n)
                    {
                        if (m == kSequential) { wantPlay.store (false); exhausted.store (false); return; }
                        next = 0;                        // 列表循环
                    }
                    if (next < 0) next = n - 1;
                }
                loadTrack (next, true);
            }

            void serviceAutoAdvance()
            {
                if (wantPlay.load() && exhausted.load() && ! loading.load())
                {
                    exhausted.store (false);
                    advanceTrack (true);
                }
            }

            //================================================================
            // 音频线程：直读解码 + 软限幅 + 叠加进本通道缓冲（L/R 混单声道）
            //================================================================
            static float saturate (float x) noexcept
            {
                // 0.95 以上 tanh 膝区软限幅：正常信号透明，防与麦克风叠加时削顶爆音
                if (x >  0.95f) return  0.95f + 0.05f * std::tanh ((x - 0.95f) * 20.0f);
                if (x < -0.95f) return -0.95f + 0.05f * std::tanh ((x + 0.95f) * 20.0f);
                return x;
            }

            void render (int n, std::vector<std::vector<float>>& chans, int nChan) noexcept
            {
                // 1) reader 创建 / 卸载（请求来自消息线程；reader 在此——音频线程
                //    MTA 公寓——创建，与 readSamples 同线程，杜绝跨公寓封送卡碟）
                if (pendingUnload.exchange (false))
                {
                    delete activeReader_;            // 音频线程独占，安全释放
                    activeReader_ = nullptr;
                    readerReady.store (false, std::memory_order_relaxed);
                    exhausted.store (false, std::memory_order_relaxed);
                    currentSeconds.store (0.0, std::memory_order_relaxed);
                }
                else if (pendingLoad.exchange (false))
                {
                    juce::File file;
                    {
                        const juce::ScopedLock sl (loadCs_);
                        file = juce::File (pendingFileStr_);
                    }
                    juce::AudioFormatReader* r = nullptr;
                    const double devRate = owner != nullptr ? owner->deviceRate() : 48000.0;
                    if (file.existsAsFile())
                    {
                        const UINT32 rate = devRate > 8000.0
                            ? static_cast<UINT32> (devRate) : 48000;
                        r = MFAudioFormatReader::create (file, rate);
                        if (r == nullptr)           // MF 解不了：回落 JUCE 内置（WAV/FLAC/OGG…）
                        {
                            juce::AudioFormatManager mgr;
                            mgr.registerBasicFormats();
                            r = mgr.createReaderFor (file);
                        }
                    }
                    if (r != nullptr)
                    {
                        delete activeReader_;
                        activeReader_ = r;
                        const double sr = juce::jmax (8000.0, r->sampleRate);
                        readerPosFrames_ = juce::jmax (0.0,
                            pendingStartSec.exchange (-1.0)) * sr;
                        lengthFrames_ = static_cast<juce::uint64> (
                            juce::jmax<juce::int64> (0, r->lengthInSamples));
                        currentSeconds.store (readerPosFrames_ / sr, std::memory_order_relaxed);
                        trackLengthSec.store (lengthFrames_ > 0
                                                  ? lengthFrames_ / sr : 0.0);
                        readerReady.store (true, std::memory_order_relaxed);
                        exhausted.store (false, std::memory_order_relaxed);
                        silentBlocks_ = 0;
                        loading.store (false);
                        if (playAfterLoad.exchange (false))
                            wantPlay.store (true);   // 请求即播：reader 就绪后立刻走带
                    }
                    else
                    {
                        loading.store (false);
                        readerReady.store (false, std::memory_order_relaxed);
                        trackLengthSec.store (0.0, std::memory_order_relaxed);
                        if (playAfterLoad.exchange (false) && ! files.empty()
                            && ++failStreak < static_cast<int> (files.size()))
                            advanceTrack (true);     // 加载失败自动跳下一首（防死循环）
                        wantPlay.store (false);
                    }
                }

                // 2) seek 请求（暂停时也要应用，进度条才能拖）
                const double req = seekReqSec.exchange (-1.0);
                if (req >= 0.0 && activeReader_ != nullptr)
                {
                    const double sr = juce::jmax (8000.0, activeReader_->sampleRate);
                    readerPosFrames_ = juce::jmax (0.0, req) * sr;
                    silentBlocks_ = 0;
                    exhausted.store (false, std::memory_order_relaxed);
                    currentSeconds.store (readerPosFrames_ / sr, std::memory_order_relaxed);
                }

                const bool playing = wantPlay.load (std::memory_order_relaxed)
                                  && readerReady.load (std::memory_order_relaxed)
                                  && activeReader_ != nullptr;
                // 3) 音量斜坡：起停 / 旁通 / 音量变化平滑过渡。旁通只把目标压到 0：
                //    时间轴继续走、解码继续，取消旁通从当前位置无缝接上。
                const float target = playing
                    ? juce::jlimit (0.0f, 2.0f, muted.load (std::memory_order_relaxed)
                                                 ? 0.0f : volume.load (std::memory_order_relaxed))
                    : 0.0f;
                float g = gain_;
                if (! playing && g <= 0.0f) return;  // 完全静止：不解码
                const float step = target > g ? 0.003f : -0.003f;

                // 目标：本通道的 L/R 缓冲（2c = L，2c+1 = R；进效果链 → 推子 → 路由）。
                // 立体声文件解码 L/R 分别写入；单声道文件由下方 numChannels==1
                // 分支复制 L→R，保证立体声通道也能居中播放。
                float* dstL = (channel >= 0 && channel < nChan
                               && 2 * channel + 1 < static_cast<int> (chans.size()))
                           ? chans[static_cast<size_t> (2 * channel)].data() : nullptr;
                float* dstR = (channel >= 0 && channel < nChan
                               && 2 * channel + 1 < static_cast<int> (chans.size()))
                           ? chans[static_cast<size_t> (2 * channel + 1)].data() : nullptr;

                // 解码暂存窗不够大（设备块长突然变大）：本块跳过，等消息线程重分配
                auto* scratch = owner != nullptr ? owner->scratch_.get() : nullptr;
                if (scratch == nullptr || scratch->getNumSamples() < n)
                    return;

                bool gotAudio = false;
                if (playing)
                {
                    const double sr = juce::jmax (8000.0, activeReader_->sampleRate);
                    float* L = scratch->getWritePointer (0);
                    float* R = scratch->getWritePointer (1);
                    float* dest[2] = { L, R };
                    const auto start = static_cast<juce::int64> (readerPosFrames_);
                    gotAudio = activeReader_->read (dest, 2, start, n);
                    // 解码垃圾钳制（损坏数据不得直出）
                    if (gotAudio)
                        for (int i = 0; i < n; ++i)
                        {
                            L[i] = juce::jlimit (-1.0f, 1.0f, L[i]);
                            R[i] = juce::jlimit (-1.0f, 1.0f, R[i]);
                        }
                    // 单声道源（解码只填 L）：复制 L→R，立体声通道才能居中出声
                    if (gotAudio && activeReader_->numChannels == 1)
                        for (int i = 0; i < n; ++i)
                            R[i] = L[i];
                }

                float* L = scratch->getWritePointer (0);
                float* R = scratch->getWritePointer (1);
                const juce::uint64 tw0 = tapWritten.load (std::memory_order_relaxed);
                int tapPos = static_cast<int> (tw0 % kTapFrames);
                float peak = 0.0f;

                for (int i = 0; i < n; ++i)
                {
                    // 增益斜坡朝 target 收敛并钳在 target（必须钳！否则稳态时
                    // target==g 使 step 反向，增益按块率锯齿振荡 → 全程爆音）
                    g = step > 0.0f ? juce::jmin (target, g + step)
                                    : juce::jmax (target, g + step);
                    const float l = gotAudio ? L[i] : 0.0f;
                    const float r = gotAudio ? R[i] : 0.0f;
                    // 立体声：L/R 分别写入本通道的 L/R 缓冲（不再混单声道）
                    if (dstL != nullptr) dstL[i] += saturate (l * g);
                    if (dstR != nullptr) dstR[i] += saturate (r * g);
                    if (gotAudio)
                    {
                        tapL[static_cast<size_t> (tapPos)] = l;
                        tapR[static_cast<size_t> (tapPos)] = r;
                        tapPos = (tapPos + 1) % kTapFrames;
                        peak = juce::jmax (peak, std::abs (l), std::abs (r));
                    }
                }

                if (gotAudio)
                    tapWritten.store (tw0 + static_cast<juce::uint64> (n),
                                      std::memory_order_relaxed);
                gain_ = g;

                if (! playing) return;

                // 4) EOF 判定 → exhausted → Bank 定时器自动切曲
                const double sr = juce::jmax (8000.0, activeReader_->sampleRate);
                readerPosFrames_ += n;
                currentSeconds.store (juce::jmin (readerPosFrames_ / sr,
                                                  trackLengthSec.load() > 0.0
                                                      ? trackLengthSec.load() + 0.5
                                                      : readerPosFrames_ / sr),
                                      std::memory_order_relaxed);
                if (! gotAudio)
                    exhausted.store (true, std::memory_order_relaxed);           // read 失败 = EOF
                else if (lengthFrames_ > 0 && readerPosFrames_ >= lengthFrames_)
                    exhausted.store (true, std::memory_order_relaxed);           // 已知长度读满
                else if (peak <= 0.0f && ++silentBlocks_ >= 40)                  // 未知时长：连续全静音
                    exhausted.store (true, std::memory_order_relaxed);
                else if (peak > 0.0f)
                    silentBlocks_ = 0;
            }

        private:
            // 音频线程独占（S-Rack 同款：reader 由音频线程接管 / 释放）
            juce::AudioFormatReader* activeReader_ = nullptr;
            double readerPosFrames_ = 0.0;
            juce::uint64 lengthFrames_ = 0;
            int silentBlocks_ = 0;
            float gain_ = 0.0f;                          // 音频线程私有斜坡

            friend struct MediaPlayerBank;               // Bank 定时器访问 serviceAutoAdvance
        };

    private:
        mutable juce::CriticalSection lock_;
        std::vector<std::unique_ptr<Player>> players_ { kMaxChannels };
        std::array<std::atomic<Player*>, kMaxChannels> snap_ {};   // 音频线程快照
        std::atomic<double> deviceRate_ { 48000.0 };
        std::atomic<int> deviceBlock_ { 512 };
        std::unique_ptr<juce::AudioBuffer<float>> scratch_;        // 解码暂存窗（音频线程只写）
        int scratchFrames_ = 0;
        Player dummy_ { nullptr, -1 };

        JUCE_DECLARE_NON_COPYABLE (MediaPlayerBank)
    };
}
