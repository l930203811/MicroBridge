#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "I18n.h"

//==============================================================================
// VST3 插件宿主（参考 Studio One 的 Inserts 插件架）
//
// · PluginCatalog：扫描 Common VST3 目录（可追加自定义目录），结果缓存到
//   %APPDATA%/MicroBridge/plugins.xml，启动时先读缓存（快），扫描放后台线程。
// · ChannelPluginBank：每通道一条插件链。控制线程增删 / 旁通后把不可变快照
//   用 atomic<shared_ptr> 换出（与路由 ProcessingPlan 同一模式）；
//   被移除的实例延迟 0.7 s 再析构，保证音频线程不再持有指针。
// · 通道内部信号是单声道，喂给插件前复制成 2 声道（L=R），处理完再回混。
//==============================================================================
namespace mb
{
    //--------------------------------------------------------------------------
    /** 宿主播放头。MicroBridge 是常驻实时机架：设备打开后音频回调永不停止，
        因此 isPlaying 恒为 true，时间轴按累计采样数连续推进。
        JUCE 的 VST3 宿主包装器在每次 processBlock 前调用
        getPlayHead()->getPosition() 去填 Vst::ProcessContext（kPlaying /
        projectTimeSamples / tempo ...）。宿主不提供 playHead 时 context.state
        全为 0：插件会认为「宿主没有在播放」，内部播放器不启动、进度条不走
        —— S-Rack 播放器插件在同类机架里正常、在这里不动正是这个原因。 */
    class HostPlayHead : public juce::AudioPlayHead
    {
    public:
        /** 消息线程：只在采样率真正变化时重置时间轴，避免进度条反复跳回 0。 */
        void prepare (double sampleRate)
        {
            const double sr = sampleRate > 8000.0 ? sampleRate : 48000.0;
            if (std::abs (rate_.load (std::memory_order_relaxed) - sr) < 1.0) return;
            rate_.store (sr, std::memory_order_relaxed);
            samples_.store (0, std::memory_order_relaxed);
        }

        /** 音频线程：每个处理块推进一次，所有插件共用同一条时间轴。 */
        void advance (int numSamples) noexcept
        {
            samples_.fetch_add (static_cast<std::int64_t> (numSamples),
                                std::memory_order_relaxed);
        }

        juce::Optional<PositionInfo> getPosition() const override
        {
            const auto s = samples_.load (std::memory_order_relaxed);
            const double rate = rate_.load (std::memory_order_relaxed);
            const double seconds = static_cast<double> (s) / rate;
            const double beats = seconds * 2.0;                 // 120 BPM

            PositionInfo info;
            info.setTimeInSamples (s);
            info.setTimeInSeconds (seconds);
            info.setPpqPosition (beats);
            info.setPpqPositionOfLastBarStart (std::floor (beats / 4.0) * 4.0);
            info.setBpm (120.0);
            info.setTimeSignature (TimeSignature { 4, 4 });
            info.setIsPlaying (true);
            info.setIsRecording (false);
            info.setIsLooping (false);
            return info;
        }

    private:
        std::atomic<double> rate_ { 48000.0 };
        std::atomic<std::int64_t> samples_ { 0 };
    };

    //--------------------------------------------------------------------------
    class PluginCatalog
    {
    public:
        PluginCatalog()
        {
            juce::addDefaultFormatsToManager (formatManager_);   // JUCE_PLUGINHOST_VST3=1 时含 VST3
        }

        ~PluginCatalog()
        {
            if (job_ != nullptr) job_->stopThread (4000);
        }

        juce::AudioPluginFormatManager& formatManager() noexcept { return formatManager_; }

        int count() const
        {
            const juce::ScopedLock sl (lock_);
            return static_cast<int> (types_.size());
        }

        juce::PluginDescription get (int index) const
        {
            const juce::ScopedLock sl (lock_);
            if (index < 0 || index >= static_cast<int> (types_.size()))
                return {};
            return types_[static_cast<size_t> (index)];
        }

        /** 按显示名 / 文件名模糊匹配（会话恢复用），返回第一个命中。 */
        juce::PluginDescription findByName (const juce::String& name) const
        {
            const juce::ScopedLock sl (lock_);
            for (const auto& t : types_)
                if (t.name == name || t.descriptiveName == name
                    || t.fileOrIdentifier == name
                    || juce::File (t.fileOrIdentifier).getFileNameWithoutExtension() == name)
                    return t;
            return {};
        }

        /** 按插件文件路径精确匹配：同名插件 / 同 UID 的不同版本不会串错。 */
        juce::PluginDescription findByIdentifier (const juce::String& id) const
        {
            if (id.isEmpty()) return {};
            const juce::ScopedLock sl (lock_);
            for (const auto& t : types_)
                if (t.fileOrIdentifier.equalsIgnoreCase (id))
                    return t;
            return {};
        }

        juce::StringArray names() const
        {
            const juce::ScopedLock sl (lock_);
            juce::StringArray a;
            for (const auto& t : types_) a.add (t.name);
            return a;
        }

        static juce::File cacheFile()
        {
            // 便携化：与设置 / 会话同放在 exe 同级 MicroBridgeData 文件夹
            return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                    .getParentDirectory().getChildFile ("MicroBridgeData")
                    .getChildFile ("plugins.xml");
        }

        /** 启动时读缓存：不加载 DLL，只恢复描述列表（会话恢复可立即匹配）。 */
        void loadCache()
        {
            const auto xml = juce::parseXML (cacheFile());
            if (xml == nullptr) return;

            const juce::ScopedLock sl (lock_);
            types_.clear();
            for (auto* e : xml->getChildWithTagNameIterator ("PLUGIN"))
            {
                juce::PluginDescription d;
                if (d.loadFromXml (*e)) types_.push_back (d);
            }
            customDirs_.clear();
            for (auto* e : xml->getChildWithTagNameIterator ("DIR"))
                customDirs_.add (e->getStringAttribute ("path"));
        }

        void addSearchDirectory (const juce::File& dir)
        {
            const auto p = dir.getFullPathName();
            const juce::ScopedLock sl (lock_);
            if (! customDirs_.contains (p)) customDirs_.add (p);
        }

        juce::StringArray customDirectories() const
        {
            const juce::ScopedLock sl (lock_);
            return customDirs_;
        }

        void removeSearchDirectory (const juce::String& path)
        {
            const juce::ScopedLock sl (lock_);
            customDirs_.removeString (path);
        }

        /** 常见 VST3 安装目录（默认扫描路径，UI 中只读不可移除）。 */
        static juce::StringArray defaultDirectories()
        {
            juce::VST3PluginFormat fmt;
            const auto base = fmt.getDefaultLocationsToSearch();
            juce::StringArray d;
            for (int i = 0; i < base.getNumPaths(); ++i)
                d.addIfNotAlreadyThere (base[i].getFullPathName());
            d.addIfNotAlreadyThere (juce::File::getSpecialLocation (
                    juce::File::commonApplicationDataDirectory)
                    .getChildFile ("VST3").getFullPathName());
            d.addIfNotAlreadyThere (juce::File::getSpecialLocation (
                    juce::File::userHomeDirectory)
                    .getChildFile ("AppData").getChildFile ("Local")
                    .getChildFile ("Programs").getChildFile ("Common")
                    .getChildFile ("VST3").getFullPathName());
            return d;
        }

        juce::String currentName() const
        {
            const juce::ScopedLock sl (lock_);
            return currentName_;
        }

        bool isScanning() const noexcept { return scanning_.load (std::memory_order_acquire); }
        double progress() const noexcept { return progress_.load (std::memory_order_relaxed); }

        /** scope 非空 = 只扫描该目录（结果与已有列表合并，仅替换该目录下的条目）。 */
        void startScan (const juce::String& scope = juce::String())
        {
            if (scanning_.load (std::memory_order_acquire)) return;
            scanning_.store (true, std::memory_order_release);
            progress_.store (0.0, std::memory_order_relaxed);
            job_ = std::make_unique<ScanJob> (*this, scope);
            job_->startThread (juce::Thread::Priority::low);
        }

        /** 消息线程：创建一个插件实例（可能加载 DLL，耗时几十 ms）。
            采样率 / 块大小必须按宿主实际值传入：VST3 在实例创建阶段就会
            setSamplerate / setupProcessing，写死 48000/512 会让内部按时间
            计数的播放器（S-Rack 等）在 44.1k / 96k 宿主里节拍错乱。 */
        std::unique_ptr<juce::AudioPluginInstance> create (const juce::PluginDescription& desc,
                                                           juce::String& error,
                                                           double sampleRate = 48000.0,
                                                           int blockSize = 512)
        {
            return formatManager_.createPluginInstance (
                desc,
                sampleRate > 8000.0 ? sampleRate : 48000.0,
                juce::jlimit (32, 8192, blockSize > 0 ? blockSize : 512),
                error);
        }

        std::function<void()> onListChanged;   // 扫描完成后（消息线程）回调

    private:
        juce::FileSearchPath searchPaths() const
        {
            const juce::ScopedLock sl (lock_);
            juce::FileSearchPath p = defaultVST3Path();
            for (const auto& d : customDirs_) p.addIfNotAlreadyThere (juce::File (d));
            return p;
        }

        static juce::FileSearchPath defaultVST3Path()
        {
            juce::VST3PluginFormat fmt;
            auto p = fmt.getDefaultLocationsToSearch();
            if (p.getNumPaths() == 0)                 // 兜底
                p.add (juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                        .getChildFile ("VST3"));
            return p;
        }

        void saveCacheLocked()
        {
            auto root = std::make_unique<juce::XmlElement> ("PLUGINS");
            for (const auto& t : types_)
                if (auto e = t.createXml())
                    root->addChildElement (e.release());
            for (const auto& d : customDirs_)
                root->createNewChildElement ("DIR")->setAttribute ("path", d);

            auto f = cacheFile();
            f.getParentDirectory().createDirectory();
            root->writeTo (f);
        }

        struct ScanJob : juce::Thread
        {
            ScanJob (PluginCatalog& owner, const juce::String& scope)
                : Thread ("MicroBridge VST3 Scan"), c (owner), scope (scope) {}

            void run() override
            {
                juce::KnownPluginList found;
                {
                    const juce::ScopedLock sl (c.lock_);
                    juce::VST3PluginFormat fmt;
                    const auto searchPath = scope.isEmpty() ? c.searchPaths()
                                                            : juce::FileSearchPath (scope);
                    const int total = juce::jmax (1,
                        fmt.searchPathsForPlugins (searchPath, true, false).size());
                    juce::PluginDirectoryScanner scanner (found, fmt, searchPath, true,
                                                          juce::File());
                    int done = 0;
                    juce::String name;
                    while (scanner.scanNextFile (true, name) && ! threadShouldExit())
                    {
                        const juce::ScopedLock sl2 (c.lock_);
                        c.currentName_ = name;
                        c.progress_.store (static_cast<double> (++done) / total,
                                           std::memory_order_relaxed);
                    }
                }

                if (! threadShouldExit())
                {
                    {
                        const juce::ScopedLock sl (c.lock_);
                        if (scope.isEmpty())
                        {
                            c.types_.clear();
                        }
                        else
                        {
                            // 只替换 scope 目录下的条目，其它目录的扫描结果保留
                            std::vector<juce::PluginDescription> kept;
                            for (const auto& t : c.types_)
                                if (! juce::File (t.fileOrIdentifier).getParentDirectory()
                                        .getFullPathName().equalsIgnoreCase (scope))
                                    kept.push_back (t);
                            c.types_.swap (kept);
                        }
                        for (const auto& t : found.getTypes())
                            c.types_.push_back (t);
                        c.saveCacheLocked();
                    }
                    c.progress_.store (1.0, std::memory_order_relaxed);
                }

                c.scanning_.store (false, std::memory_order_release);
                juce::MessageManager::callAsync ([&c = c]
                {
                    if (c.onListChanged) c.onListChanged();
                });
            }

            PluginCatalog& c;
            const juce::String scope;
        };

        juce::AudioPluginFormatManager formatManager_;
        std::vector<juce::PluginDescription> types_;   // guarded by lock_
        juce::StringArray customDirs_;                 // guarded by lock_
        juce::String currentName_;                     // guarded by lock_
        mutable juce::CriticalSection lock_;
        std::atomic<bool> scanning_ { false };
        std::atomic<double> progress_ { 0.0 };
        std::unique_ptr<ScanJob> job_;

        JUCE_DECLARE_NON_COPYABLE (PluginCatalog)
    };

    //--------------------------------------------------------------------------
    class ChannelPluginBank
    {
    public:
        static constexpr int kMaxChannels = 32;
        /** 节点视图「未连接（自由）插件」停放槽。它不属于任何真实通道，
            AudioEngine 只为 0..channelCount()-1 调 processChannel，因此这里
            的插件不参与出声；把端口连到通道 = movePlugin(kFreePluginSlot→ch)，
            圆点菜单「断开连接」= movePlugin(ch→kFreePluginSlot)。 */
        static constexpr int kFreePluginSlot = kMaxChannels - 1;

        ChannelPluginBank()
        {
            // 插件处理缓冲必须容纳插件的全部输出总线：Remix 等音源分离类插件
            // 有 6 条输出总线（12 通道）。若只给 2 通道，JUCE VST3 包装器在
            // HostBufferMapper 里对 getWritePointer(2..11) 取到越界野指针，
            // 插件往野指针写 stem 数据 → 主输出静音 + 堆损坏（界面偶发打不开）。
            // 这里先按立体声预分配，添加插件时按实际总线数扩容。
            ensureProcChannels (2);
        }

        ~ChannelPluginBank() = default;

        //==================================================================
        // 控制线程接口
        //==================================================================
        struct PluginEntry                      // 控制侧持有实例
        {
            int builtInId = 0;                  // >0 = 自带插件（映射内置效果链模块）
            std::unique_ptr<juce::AudioPluginInstance> instance;
            bool bypass = false;
            bool offline = false;               // 离线：实例已卸载，私有状态暂存 savedState
            juce::MemoryBlock savedState;       // 离线期间的 getStateInformation 快照
            juce::String name;
            juce::String identifier;            // VST3 文件路径，会话恢复时精确匹配
            juce::String note;                  // 插件备注（显示「备注 (插件名)」）
            juce::String uid;                   // 稳定标识：节点视图自由摆放位置按它持久化
        };

        static juce::String makeUid()           { return juce::Uuid().toString(); }

        /** 添加自带插件（重复添加返回提示）；参数开关由调用方经 setFxParams 同步。 */
        juce::String addBuiltIn (int ch, int id)
        {
            if (! valid (ch)) return tr ("插件无效", "Invalid plugin");
            const juce::ScopedLock sl (lock_);
            auto& v = slots_[static_cast<size_t> (ch)];
            for (const auto& e : v)
                if (e.builtInId == id)
                    return tr ("该自带插件已添加", "Built-in plugin already added");

            PluginEntry e;
            e.builtInId = id;
            e.name = builtInPluginName (id);
            e.uid = makeUid();
            v.push_back (std::move (e));
            publishLocked (ch);
            return {};
        }

        /** 0 = 第三方 VST3，>0 = 自带插件 id。 */
        int pluginBuiltInId (int ch, int idx) const
        {
            if (! valid (ch)) return 0;
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            return idx >= 0 && idx < static_cast<int> (v.size())
                 ? v[static_cast<size_t> (idx)].builtInId : 0;
        }

        int pluginCount (int ch) const
        {
            if (! valid (ch)) return 0;
            const juce::ScopedLock sl (lock_);
            return static_cast<int> (slots_[static_cast<size_t> (ch)].size());
        }

        juce::String pluginName (int ch, int idx) const
        {
            if (! valid (ch)) return {};
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            return idx >= 0 && idx < static_cast<int> (v.size())
                 ? v[static_cast<size_t> (idx)].name : juce::String();
        }

        /** 查找某通道上指定自带插件的槽位序号（找不到返回 -1）。 */
        int builtInSlot (int ch, int builtInId) const
        {
            if (! valid (ch)) return -1;
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            for (int i = 0; i < static_cast<int> (v.size()); ++i)
                if (v[static_cast<size_t> (i)].builtInId == builtInId) return i;
            return -1;
        }

        bool pluginBypass (int ch, int idx) const
        {
            if (! valid (ch)) return false;
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            return idx >= 0 && idx < static_cast<int> (v.size())
                 && v[static_cast<size_t> (idx)].bypass;
        }

        bool pluginOffline (int ch, int idx) const
        {
            if (! valid (ch)) return false;
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            return idx >= 0 && idx < static_cast<int> (v.size())
                 && v[static_cast<size_t> (idx)].offline;
        }

        juce::String pluginNote (int ch, int idx) const
        {
            if (! valid (ch)) return {};
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            return idx >= 0 && idx < static_cast<int> (v.size())
                 ? v[static_cast<size_t> (idx)].note : juce::String();
        }

        void setPluginNote (int ch, int idx, const juce::String& note)
        {
            if (! valid (ch)) return;
            const juce::ScopedLock sl (lock_);
            auto& v = slots_[static_cast<size_t> (ch)];
            if (idx < 0 || idx >= static_cast<int> (v.size())) return;
            v[static_cast<size_t> (idx)].note = note;
        }

        /** 节点视图按 UID 记住每张插件卡片的画布位置（序号会因增删/连线而变）。 */
        juce::String pluginUid (int ch, int idx) const
        {
            if (! valid (ch)) return {};
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            return idx >= 0 && idx < static_cast<int> (v.size())
                 ? v[static_cast<size_t> (idx)].uid : juce::String();
        }

        void setPluginUid (int ch, int idx, const juce::String& uid)
        {
            if (! valid (ch) || uid.isEmpty()) return;
            const juce::ScopedLock sl (lock_);
            auto& v = slots_[static_cast<size_t> (ch)];
            if (idx < 0 || idx >= static_cast<int> (v.size())) return;
            auto& e = v[static_cast<size_t> (idx)];
            if (e.uid.isEmpty()) e.uid = uid;           // 旧会话存档：首次载入时补一个
        }

        /** 离线插件：卸载实例释放资源，私有状态暂存；音频链上该条自动跳过。 */
        bool markOffline (int ch, int idx)
        {
            if (! valid (ch)) return false;
            const juce::ScopedLock sl (lock_);
            auto& v = slots_[static_cast<size_t> (ch)];
            if (idx < 0 || idx >= static_cast<int> (v.size())) return false;
            auto& e = v[static_cast<size_t> (idx)];
            if (e.instance == nullptr || e.offline) return false;
            try { e.instance->getStateInformation (e.savedState); } catch (...) {}
            retire (std::move (e.instance));
            e.offline = true;
            publishLocked (ch);
            return true;
        }

        /** 取消离线：用新创建的实例原地恢复（状态回灌 + prepareToPlay + 播放头）。 */
        juce::String restoreOffline (int ch, int idx,
                                     std::unique_ptr<juce::AudioPluginInstance> p,
                                     double sampleRate, int blockSize)
        {
            if (! valid (ch) || p == nullptr) return tr ("插件无效", "Invalid plugin");
            const juce::ScopedLock sl (lock_);
            auto& v = slots_[static_cast<size_t> (ch)];
            if (idx < 0 || idx >= static_cast<int> (v.size())) return tr ("插件无效", "Invalid plugin");
            auto& e = v[static_cast<size_t> (idx)];
            if (! e.offline) return {};

            // 与 addPlugin 相同的顺序：先挂播放头、回灌状态，再 prepareToPlay
            playHead_.prepare (sampleRate);
            p->setPlayHead (&playHead_);
            if (e.savedState.getSize() > 0)
            {
                try
                {
                    p->setStateInformation (e.savedState.getData(),
                                            static_cast<int> (e.savedState.getSize()));
                }
                catch (...) {}
            }
            p->prepareToPlay (sampleRate > 8000.0 ? sampleRate : 48000.0,
                              juce::jlimit (32, 8192, blockSize > 0 ? blockSize : 512));
            ensureProcChannels (juce::jmax (p->getTotalNumInputChannels(),
                                            p->getTotalNumOutputChannels()));

            e.instance = std::move (p);
            e.offline = false;
            e.savedState.reset();
            publishLocked (ch);
            return {};
        }

        /** 会话恢复：离线条目不创建实例，只占位（name/identifier/状态快照）。 */
        void addOfflineEntry (int ch, const juce::String& name, const juce::String& identifier,
                              const juce::MemoryBlock& state)
        {
            if (! valid (ch)) return;
            const juce::ScopedLock sl (lock_);
            PluginEntry e;
            e.name = name;
            e.identifier = identifier;
            e.offline = true;
            e.savedState = state;
            e.uid = makeUid();
            slots_[static_cast<size_t> (ch)].push_back (std::move (e));
            publishLocked (ch);
        }

        /** 控制线程：取实例指针（打开插件编辑器窗口用，不转移所有权）。 */
        juce::AudioPluginInstance* pluginInstance (int ch, int idx) const
        {
            if (! valid (ch)) return nullptr;
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            return idx >= 0 && idx < static_cast<int> (v.size())
                 ? v[static_cast<size_t> (idx)].instance.get() : nullptr;
        }

        /** 消息线程：导出插件私有状态（VST3 chunk / 播放列表 / 参数）。
            离线条目返回离线前的暂存快照。 */
        juce::MemoryBlock pluginState (int ch, int idx) const
        {
            if (! valid (ch)) return {};
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            if (idx < 0 || idx >= static_cast<int> (v.size())) return {};
            auto& e = v[static_cast<size_t> (idx)];
            if (e.offline) return e.savedState;
            auto* p = e.instance.get();
            if (p == nullptr) return {};
            juce::MemoryBlock block;
            try { p->getStateInformation (block); } catch (...) { return {}; }
            return block;
        }

        juce::String pluginIdentifier (int ch, int idx) const
        {
            if (! valid (ch)) return {};
            const juce::ScopedLock sl (lock_);
            const auto& v = slots_[static_cast<size_t> (ch)];
            return idx >= 0 && idx < static_cast<int> (v.size())
                 ? v[static_cast<size_t> (idx)].identifier : juce::String();
        }

        /** 宿主收不到插件内部改动的回调（S-Rack 导入歌曲就是典型），
         *  因此周期性取全部第三方插件状态的指纹，变化即触发会话自动保存。 */
        juce::String stateDigest() const
        {
            const juce::ScopedLock sl (lock_);
            std::uint64_t h = 14695981039346656037ULL;
            auto mix = [&h] (const void* data, std::size_t n)
            {
                auto* b = static_cast<const std::uint8_t*> (data);
                for (std::size_t i = 0; i < n; ++i)
                {
                    h ^= b[i];
                    h *= 1099511628211ULL;
                }
            };
            int count = 0;
            for (const auto& v : slots_)
                for (const auto& e : v)
                {
                    if (e.builtInId > 0 || e.instance == nullptr) continue;
                    juce::MemoryBlock block;
                    try { e.instance->getStateInformation (block); }
                    catch (...) { continue; }
                    const std::uint64_t sz = block.getSize();
                    mix (&sz, sizeof (sz));
                    if (sz > 0) mix (block.getData(), static_cast<std::size_t> (sz));
                    const std::uint8_t by = e.bypass ? 1 : 0;
                    mix (&by, 1);
                    ++count;
                }
            return juce::String::toHexString (h) + mb::S(":") + juce::String (count);
        }

        /** state 非空时先恢复插件私有状态，再 prepareToPlay。
         *  顺序很重要：插件通常在 setStateInformation 里重建内部对象
         *  （S-Rack 的播放列表 / 曲目路径就是在这里读回的）。 */
        juce::String addPlugin (int ch, std::unique_ptr<juce::AudioPluginInstance> p,
                                double sampleRate, int blockSize,
                                const juce::MemoryBlock* state = nullptr,
                                const juce::String& identifier = {})
        {
            if (! valid (ch) || p == nullptr) return tr ("插件无效", "Invalid plugin");
            const juce::ScopedLock sl (lock_);

            // 宿主播放头必须在 setStateInformation / prepareToPlay 之前挂上：
            // 插件会在这里读取播放状态并初始化内部播放器与进度条。
            playHead_.prepare (sampleRate);
            p->setPlayHead (&playHead_);

            if (state != nullptr && state->getSize() > 0)
            {
                try
                {
                    p->setStateInformation (state->getData(),
                                            static_cast<int> (state->getSize()));
                }
                catch (...) {}
            }

            juce::String err;
            p->prepareToPlay (sampleRate > 8000.0 ? sampleRate : 48000.0,
                              juce::jlimit (32, 8192, blockSize > 0 ? blockSize : 512));

            // 处理缓冲按插件实际总线数扩容（消息线程，音频槽快照尚未发布）。
            // 必须在 publishLocked 之前，保证音频线程拿到新链时缓冲已够大。
            ensureProcChannels (juce::jmax (p->getTotalNumInputChannels(),
                                            p->getTotalNumOutputChannels()));

            PluginEntry e;
            e.name = p->getName();
            e.identifier = identifier;
            e.instance = std::move (p);
            e.uid = makeUid();
            slots_[static_cast<size_t> (ch)].push_back (std::move (e));
            publishLocked (ch);
            return err;
        }

        bool removePlugin (int ch, int idx)
        {
            if (! valid (ch)) return false;
            const juce::ScopedLock sl (lock_);
            auto& v = slots_[static_cast<size_t> (ch)];
            if (idx < 0 || idx >= static_cast<int> (v.size())) return false;

            retire (std::move (v[static_cast<size_t> (idx)].instance));
            v.erase (v.begin() + idx);
            publishLocked (ch);
            return true;
        }

        void setBypass (int ch, int idx, bool bypass)
        {
            if (! valid (ch)) return;
            const juce::ScopedLock sl (lock_);
            auto& v = slots_[static_cast<size_t> (ch)];
            if (idx < 0 || idx >= static_cast<int> (v.size())) return;
            auto& e = v[static_cast<size_t> (idx)];
            e.bypass = bypass;
            publishLocked (ch);
            // 自带插件旁通联动钩子（「音频播放」= 静音走带）
            if (onBuiltInBypass && e.builtInId > 0)
            {
                const int id = e.builtInId;
                const juce::ScopedUnlock su (lock_);
                onBuiltInBypass (ch, id, bypass);
            }
        }

        void clearChannel (int ch)
        {
            if (! valid (ch)) return;
            const juce::ScopedLock sl (lock_);
            for (auto& e : slots_[static_cast<size_t> (ch)])
                retire (std::move (e.instance));
            slots_[static_cast<size_t> (ch)].clear();
            publishLocked (ch);
        }

        /** 通道重排（order[newPos] = oldIdx）：插件链跟随通道移动。
         *  order 只覆盖前 k 个通道（k = 实际通道数），其余槽位保持原位。 */
        void reorderChannels (const std::vector<int>& order)
        {
            // 停放槽（kFreePluginSlot）不参与通道换位 / 删除补位
            const int n = kFreePluginSlot;
            if (order.empty() || static_cast<int> (order.size()) > n) return;

            std::vector<bool> seen (static_cast<size_t> (n), false);
            for (int o : order)
            {
                if (o < 0 || o >= n || seen[static_cast<size_t> (o)]) return;
                seen[static_cast<size_t> (o)] = true;
            }

            std::vector<int> full (static_cast<size_t> (n));
            for (int i = 0; i < n; ++i)
                full[static_cast<size_t> (i)]
                    = i < static_cast<int> (order.size()) ? order[static_cast<size_t> (i)] : i;

            const juce::ScopedLock sl (lock_);
            auto t = std::move (slots_);               // PluginEntry 持有 unique_ptr，只能移动
            for (int i = 0; i < n; ++i)
                slots_[static_cast<size_t> (i)]
                    = std::move (t[static_cast<size_t> (full[static_cast<size_t> (i)])]);
            for (int i = 0; i < n; ++i)
                publishLocked (i);
        }

        /** 移动插件条目：src 通道第 idx 条 → dst 通道链尾，返回目标新索引（-1 失败）。
            只搬 PluginEntry（含实例/备注/旁通/离线状态）；内置插件的 FxParams 联动
            与播放器清理由调用方（MainComponent）负责，避免音频线程副作用。 */
        int movePlugin (int srcCh, int idx, int dstCh)
        {
            if (! valid (srcCh) || ! valid (dstCh) || srcCh == dstCh) return -1;
            const juce::ScopedLock sl (lock_);
            auto& src = slots_[static_cast<size_t> (srcCh)];
            if (idx < 0 || idx >= static_cast<int> (src.size())) return -1;
            auto& dst = slots_[static_cast<size_t> (dstCh)];
            const int newIdx = static_cast<int> (dst.size());
            dst.push_back (std::move (src[static_cast<size_t> (idx)]));
            src.erase (src.begin() + idx);
            publishLocked (srcCh);
            publishLocked (dstCh);
            return newIdx;
        }

        /** 同通道内重排：把第 from 条移到第 to 条之前（to 为移动后目标下标）。
            UI 的「插件 A 输出 → 插件 B 输入」连线即调用本方法表达处理顺序。
            from/to 均按调用前的下标计算；返回是否发生变动。 */
        bool reorderPlugin (int ch, int from, int to)
        {
            if (! valid (ch)) return false;
            const juce::ScopedLock sl (lock_);
            auto& v = slots_[static_cast<size_t> (ch)];
            if (from < 0 || from >= static_cast<int> (v.size())
                || to < 0 || to >= static_cast<int> (v.size()) || from == to) return false;
            auto entry = std::move (v[static_cast<size_t> (from)]);
            v.erase (v.begin() + from);
            int dst = to;
            if (from < to) --dst;                       // 前移一位补位
            dst = juce::jlimit (0, static_cast<int> (v.size()), dst);
            v.insert (v.begin() + dst, std::move (entry));
            publishLocked (ch);
            return true;
        }

        /** 删除通道：其插件链退役，其后通道前移补位。 */
        void removeChannel (int ch)
        {
            // 停放槽（kFreePluginSlot）不参与通道换位 / 删除补位
            const int n = kFreePluginSlot;
            if (ch < 0 || ch >= n) return;

            const juce::ScopedLock sl (lock_);
            for (auto& e : slots_[static_cast<size_t> (ch)])
                retire (std::move (e.instance));
            for (int i = ch; i < n - 1; ++i)
                slots_[static_cast<size_t> (i)] = std::move (slots_[static_cast<size_t> (i + 1)]);
            slots_[static_cast<size_t> (n - 1)].clear();
            for (int i = ch; i < n; ++i)
                publishLocked (i);
        }

        /** 采样率 / 块大小变化后调用（消息线程）。prepareToPlay 会重置插件
            内部缓冲，因此只在参数真的变化时才执行；播放头时间轴不重置。 */
        void prepare (double sampleRate, int blockSize)
        {
            if (sampleRate <= 8000.0) return;
            const int bs = juce::jlimit (32, 8192, blockSize > 0 ? blockSize : 512);
            if (std::abs (sampleRate - preparedSr_) < 1.0 && bs == preparedBlock_) return;
            preparedSr_ = sampleRate;
            preparedBlock_ = bs;
            playHead_.prepare (sampleRate);

            const juce::ScopedLock sl (lock_);
            for (auto& v : slots_)
                for (auto& e : v)
                    if (e.instance != nullptr)
                    {
                        // 会话恢复 / 采样率变化后同样要扩容（ensure 无锁，调用处已持锁）
                        ensureProcChannels (juce::jmax (e.instance->getTotalNumInputChannels(),
                                                        e.instance->getTotalNumOutputChannels()));
                        e.instance->prepareToPlay (sampleRate, bs);
                    }
        }

        /** 音频线程：每个处理块推进一次宿主时间轴（全部插件共用）。 */
        void advance (int numSamples) noexcept { playHead_.advance (numSamples); }

        /** UI 定时器调用：安全清理已退役的插件实例（音频线程已远离旧快照）。 */
        void flushRetired()
        {
            const auto cutoff = juce::Time::getMillisecondCounter() - 700;
            for (auto it = retired_.begin(); it != retired_.end();)
                if (it->first < cutoff) it = retired_.erase (it);
                else ++it;
        }

        //==================================================================
        // 音频线程
        //==================================================================
        void processChannel (int ch, float* L, float* R, int n) noexcept
        {
            if (! valid (ch) || L == nullptr || R == nullptr) return;
            const auto chain = chains_[static_cast<size_t> (ch)].load (std::memory_order_acquire);
            if (chain == nullptr || chain->empty()) return;

            // 处理缓冲按插件实际总线数扩容（见 ensureProcChannels）。VST3 包装器
            // 会把多总线插件的每条输出总线映射到 buffer.getWritePointer(k)，
            // 通道不足就是越界野指针 —— 必须让缓冲覆盖插件全部输出通道。
            const auto pb = procBuf_.load (std::memory_order_acquire);
            if (pb == nullptr || n > pb->owned.getNumSamples()) return;

            // 通道 L/R 写进总线 0；其余总线清零（部分插件读它们做侧链/stem）。
            auto* const d0 = pb->ptrs[(size_t) 0];
            auto* const d1 = pb->ptrs[(size_t) 1];
            juce::FloatVectorOperations::copy (d0, L, n);
            juce::FloatVectorOperations::copy (d1, R, n);
            for (int c = 2; c < pb->numChannels; ++c)
                juce::FloatVectorOperations::clear (pb->ptrs[(size_t) c], n);

            // 每块用当前块长建立视图（零分配）；视图引用 pb 拥有的稳定内存。
            processBuf_.setDataToReferTo (pb->ptrs.data(), pb->numChannels, n);
            midi_.clear();

            for (const auto& s : *chain)
            {
                if (s.plugin == nullptr || s.bypass) continue;
                s.plugin->processBlock (processBuf_, midi_);
            }

            // 主输出（总线 0 的 L/R）写回通道；stem 总线不走通道，避免混叠。
            juce::FloatVectorOperations::copy (L, d0, n);
            juce::FloatVectorOperations::copy (R, d1, n);
        }

    private:
        struct SlotSnap { juce::AudioPluginInstance* plugin = nullptr; bool bypass = false; };
        using Chain = std::vector<SlotSnap>;

        static bool valid (int ch) noexcept { return ch >= 0 && ch < kMaxChannels; }

        /** 插件处理缓冲：owned 拥有稳定内存，ptrs 指向各通道，音频线程只读快照。 */
        struct ProcBuffer
        {
            explicit ProcBuffer (int nc) : numChannels (juce::jmax (2, nc))
            {
                owned.setSize (numChannels, 8192);
                ptrs.resize (static_cast<size_t> (numChannels));
                for (int c = 0; c < numChannels; ++c)
                    ptrs[(size_t) c] = owned.getWritePointer (c);
            }
            juce::AudioBuffer<float> owned;
            std::vector<float*> ptrs;
            int numChannels = 2;
        };

        /** 消息线程调用（addPlugin / prepare 已持锁）：按插件总线数扩容处理缓冲。
         *  只增不减，避免音频线程正引用的快照内存被释放；旧快照靠 shared_ptr 引用计数
         *  在音频线程远离后自然析构。 */
        void ensureProcChannels (int need)
        {
            auto cur = procBuf_.load (std::memory_order_acquire);
            if (cur != nullptr && need <= cur->numChannels) return;
            procBuf_.store (std::make_shared<ProcBuffer> (need), std::memory_order_release);
        }

        void publishLocked (int ch)
        {
            auto snap = std::make_shared<Chain>();
            for (const auto& e : slots_[static_cast<size_t> (ch)])
                snap->push_back ({ e.instance.get(), e.bypass });
            chains_[static_cast<size_t> (ch)].store (std::move (snap), std::memory_order_release);
        }

        void retire (std::unique_ptr<juce::AudioPluginInstance> p)
        {
            if (p != nullptr)
                retired_.emplace_back (juce::Time::getMillisecondCounter(), std::move (p));
        }

        // playHead_ 必须最先声明（成员逆序析构 → 最后释放），保证所有插件
        // 实例都还活着时它不会先被销毁，插件内保存的是非拥有裸指针。
        HostPlayHead playHead_;

    public:
        /** 自带插件旁通联动钩子（消息线程，setBypass 触发；lock_ 已临时释放）。 */
        std::function<void (int ch, int builtInId, bool bypass)> onBuiltInBypass;

    private:
        double preparedSr_ = 0.0;
        int preparedBlock_ = 0;

        juce::CriticalSection lock_;
        std::array<std::vector<PluginEntry>, kMaxChannels> slots_;        // 控制侧
        std::array<std::atomic<std::shared_ptr<const Chain>>, kMaxChannels> chains_; // 音频侧
        std::vector<std::pair<std::uint32_t, std::unique_ptr<juce::AudioPluginInstance>>> retired_;

        std::atomic<std::shared_ptr<const ProcBuffer>> procBuf_;  // 处理缓冲快照（音频线程 acquire 读）
        juce::AudioBuffer<float> processBuf_;   // 每块 setDataToReferTo 引用 procBuf_ 的 owned
        juce::MidiBuffer midi_;

        JUCE_DECLARE_NON_COPYABLE (ChannelPluginBank)
    };
}
