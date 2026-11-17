#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "ChannelFx.h"
#include "ChannelLinks.h"
#include "RouteGraph.h"
#include "Theme.h"

//==============================================================================
// 会话 / 预设持久化（参考主流机架的 session 自动保存 + 预设导入导出）
//
// · 便携化：全部数据存放在 exe 同级的 MicroBridgeData 文件夹内，随程序走。
// · 会话文件 session.json：通道数、推子、静音、独奏、全部连边与每通道效果
//   参数。启动时恢复；运行中每次改动后约 2 秒自动保存。
// · 预设文件（.mbpreset）存 MicroBridgeData/Presets，内容同构，
//   经对话框导入 / 导出，可分享。
// · 连边保存的是端口编号；驱动重扫后端口可能重排，越界边会被
//   RouteGraph::setPortCounts 直接丢弃（README 限制 2 的既有行为）。
//==============================================================================
namespace mb
{
    class SessionStore
    {
    public:
        /** 便携化数据根目录：exe 所在位置的 MicroBridgeData 文件夹。 */
        static juce::File dataRoot()
        {
            return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                    .getParentDirectory().getChildFile ("MicroBridgeData");
        }

        static juce::File sessionFile()
        {
            return dataRoot().getChildFile ("session.json");
        }

        static juce::File presetDirectory()
        {
            return dataRoot().getChildFile ("Presets");
        }

        //======================================================================
        static juce::var toJson (const mb::RouteGraph& g, const mb::AudioEngine& engine,
                                 const juce::var& nodeLayout = juce::var())
        {
            auto* root = new juce::DynamicObject();
            root->setProperty ("app", "MicroBridge");
            root->setProperty ("version", 4);   // v4：新增 players（音频播放器状态）
            root->setProperty ("saved", juce::Time::getCurrentTime().toISO8601 (true));
            root->setProperty ("channels", g.channelCount());

            juce::Array<juce::var> faders, pans, muted, solo, enabled, armed, names, colours, icons, slots, aligned, stereo;
            juce::Array<juce::var> linkAct, linkLetter, linkIdx, linkKey, linkMods;
            const auto& customNames = mb::customChannelNames();
            const auto& customCols  = mb::ui::customChannelColours();
            const auto& customIcons = mb::customChannelIcons();
            for (int c = 0; c < g.channelCount(); ++c)
            {
                faders.add (g.fader (c));
                pans.add (g.pan (c));
                muted.add (g.isMuted (c));
                solo.add (g.isSolo (c));
                enabled.add (g.storedEnabled (c));   // 独奏中保存快照值，不写扫过态
                armed.add (mb::ui::isChannelArmed (c));
                names.add (c < customNames.size() ? customNames[c] : juce::String());
                colours.add (c < static_cast<int> (customCols.size())
                             ? static_cast<int> (customCols[static_cast<size_t> (c)].getARGB()) : 0);
                icons.add (c < customIcons.size() ? customIcons[c] : juce::String());
                slots.add (static_cast<int> (mb::ui::slotState (c)));
                aligned.add (static_cast<int> (mb::ui::isChannelRightAligned (c)));
                stereo.add (static_cast<int> (mb::ui::isChannelStereo (c)));
                // 每通道 × 每动作一条配置，扁平存储：下标 = c * kActionCount + a
                for (int a = 0; a < mb::links::kActionCount; ++a)
                {
                    const auto lk = mb::links::linkOf (c, a);
                    linkAct.add (a);
                    linkLetter.add (lk.letter);
                    linkIdx.add (lk.index);
                    linkKey.add (lk.key);
                    linkMods.add (lk.mods);
                }
            }
            root->setProperty ("faders", faders);
            root->setProperty ("pans", pans);
            root->setProperty ("muted", muted);
            root->setProperty ("solo", solo);
            root->setProperty ("enabled", enabled);
            root->setProperty ("armed", armed);
            root->setProperty ("names", names);
            root->setProperty ("colours", colours);
            root->setProperty ("icons", icons);
            root->setProperty ("slots", slots);
            // 「对齐前位置」记忆（取消右对齐时恢复用）
            juce::Array<juce::var> alignOrigin;
            for (int c = 0; c < g.channelCount(); ++c)
                alignOrigin.add (mb::ui::channelRightAlignedOrigin (c));

            root->setProperty ("aligned", aligned);
            root->setProperty ("alignOrigin", alignOrigin);
            root->setProperty ("stereo", stereo);
            root->setProperty ("linkAct", linkAct);
            root->setProperty ("linkLetter", linkLetter);
            root->setProperty ("linkIdx", linkIdx);
            root->setProperty ("linkKey", linkKey);
            root->setProperty ("linkMods", linkMods);

            juce::Array<juce::var> edges;
            for (const auto& e : g.edges())
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("s", e.src);
                o->setProperty ("d", e.dst);
                o->setProperty ("k", static_cast<int> (e.kind));
                o->setProperty ("g", e.gain);
                o->setProperty ("sd", e.side);
                edges.add (juce::var (o));
            }
            root->setProperty ("edges", edges);

            juce::Array<juce::var> fx;
            for (int c = 0; c < g.channelCount() && c < mb::ChannelFxBank::kMaxChannels; ++c)
                fx.add (fxToJson (engine.fxParams (c)));
            root->setProperty ("fx", fx);

            // 每通道插件链（自带插件存 id，第三方 VST3 按名字恢复）
            // 自由插件（节点视图未连接的卡片）存在停放槽，单独一份序列。
            auto packChain = [&engine] (int ch) -> juce::Array<juce::var>
            {
                juce::Array<juce::var> chain;
                const int n = engine.pluginBank().pluginCount (ch);
                for (int i = 0; i < n; ++i)
                {
                    auto* o = new juce::DynamicObject();
                    const int builtIn = engine.pluginBank().pluginBuiltInId (ch, i);
                    o->setProperty ("n", engine.pluginBank().pluginName (ch, i));
                    o->setProperty ("b", engine.pluginBank().pluginBypass (ch, i));
                    o->setProperty ("k", builtIn);
                    o->setProperty ("o", engine.pluginBank().pluginOffline (ch, i));
                    o->setProperty ("pn", engine.pluginBank().pluginNote (ch, i));
                    o->setProperty ("u", engine.pluginBank().pluginUid (ch, i));
                    if (builtIn == 0)          // 第三方 VST3：连私有状态一起保存
                    {
                        o->setProperty ("f", engine.pluginBank().pluginIdentifier (ch, i));
                        const auto state = engine.pluginBank().pluginState (ch, i);
                        if (state.getSize() > 0)
                            o->setProperty ("st", state.toBase64Encoding());
                    }
                    chain.add (juce::var (o));
                }
                return chain;
            };
            juce::Array<juce::var> plugins;
            for (int c = 0; c < g.channelCount() && c < mb::ChannelPluginBank::kMaxChannels; ++c)
                plugins.add (packChain (c));
            root->setProperty ("plugins", plugins);
            root->setProperty ("freePlugins",
                               packChain (mb::ChannelPluginBank::kFreePluginSlot));

            // 每通道「音频播放」播放器状态（v4）
            juce::Array<juce::var> players;
            for (int c = 0; c < g.channelCount() && c < mb::MediaPlayerBank::kMaxChannels; ++c)
            {
                auto* p = engine.mediaPlayers().playerPtr (c);
                auto* o = new juce::DynamicObject();
                if (p != nullptr)
                {
                    juce::Array<juce::var> paths;
                    for (const auto& f : p->files) paths.add (f.getFullPathName());
                    o->setProperty ("files", paths);
                    o->setProperty ("idx", p->currentIdx.load());
                    o->setProperty ("pos", p->positionSec());
                    o->setProperty ("vol", p->volume.load());
                    o->setProperty ("mode", p->mode.load());
                    o->setProperty ("off", p->offsetMs.load());
                    o->setProperty ("playing", p->wantPlay.load());
                }
                players.add (juce::var (o));
            }
            root->setProperty ("players", players);

            // 节点视图自由布局（可选；由调用方传入）
            if (! nodeLayout.isVoid())
                root->setProperty ("nodeLayout", nodeLayout);

            return juce::var (root);
        }

        /** 从设备目录推导端口 L/R 配对（与 MainComponent::collectPortPairs 同规则）：
            同设备、相邻且 channelIndex 为 0/1 的两个端口视为一对。返回 (L,R) 索引对，
            供 RouteGraph::setPortPairs 使用 —— compile() 据此把立体声通道的边正确映射到
            L/R 缓冲，避免两个物理端口进立体声通道时被误判成「居中」而丢失声像。 */
        template <typename Catalog>
        static std::vector<std::pair<int, int>> derivePortPairs (const Catalog& cat, bool input)
        {
            const auto& ports = input ? cat.inputPorts() : cat.outputPorts();
            std::vector<std::pair<int, int>> out;
            for (size_t i = 0; i + 1 < ports.size(); ++i)
            {
                if (ports[i].input == input && ports[i + 1].input == input
                    && ports[i].deviceIndex == ports[i + 1].deviceIndex
                    && ports[i].channelIndex % 2 == 0
                    && ports[i + 1].channelIndex == ports[i].channelIndex + 1)
                {
                    out.push_back ({ static_cast<int> (i), static_cast<int> (i + 1) });
                    ++i;
                }
            }
            return out;
        }

        /** 应用到图与效果仓。返回是否至少恢复了通道布局。 */
        static bool fromJson (const juce::var& v, mb::RouteGraph& g,
                              mb::AudioEngine& engine, int inputPorts, int outputPorts)
        {
            if (! v.isObject()) return false;
            auto* root = v.getDynamicObject();
            if (root == nullptr || root->getProperty ("app").toString() != "MicroBridge")
                return false;

            g.clearDroppedEdges();   // 新装载开始：清除上次装载/设备切换的掉边记录
            const int channels = juce::jlimit (1, 16,
                static_cast<int> (root->getProperty ("channels")));
            g.resize (channels, inputPorts, outputPorts);

            const auto& faders  = root->getProperty ("faders");
            const auto& pans    = root->getProperty ("pans");
            const auto& muted   = root->getProperty ("muted");
            const auto& solo    = root->getProperty ("solo");
            const auto& enabled = root->getProperty ("enabled");
            const auto& armed   = root->getProperty ("armed");
            // 新建空白预设会落盘 channels=0 + 全空数组；通道数在此被夹成 1，
            // 若仍直接 faders[c] 索引空数组，JUCE var::operator[] 不做边界检查，
            // 会越界读取并在启动阶段触发 0xC0000005。所有数组一律先判类型 + 长度，
            // 缺项回退默认值（推子 1.0 / 未静音 / 未独奏），坏会话也能安全恢复。
            for (int c = 0; c < channels; ++c)
            {
                const bool haveF = faders.isArray() && c < faders.size();
                const bool haveM = muted.isArray()  && c < muted.size();
                const bool haveS = solo.isArray()   && c < solo.size();
                g.setFader (c, haveF ? static_cast<float> (static_cast<double> (faders[c]))
                                     : 1.0f);
                // 声像（旧预设无 pans 字段时保持居中 0）
                if (pans.isArray() && c < pans.size())
                    g.setPan (c, juce::jlimit (-1.0f, 1.0f,
                              static_cast<float> (static_cast<double> (pans[c]))));
                g.setMuted (c, haveM && (static_cast<bool> (muted[c])
                                          || static_cast<int> (muted[c]) != 0));
                g.setSolo  (c, haveS && (static_cast<bool> (solo[c])
                                          || static_cast<int> (solo[c]) != 0));
                if (enabled.isArray() && c < enabled.size())
                    g.setEnabled (c, static_cast<int> (enabled[c]) != 0);
                mb::ui::setChannelArmed (c, armed.isArray() && c < armed.size()
                                         && static_cast<int> (armed[c]) != 0);
            }

            // 通道名 / 颜色（旧文件无此字段时保持默认；空名与 0 色值回退默认）
            const auto& nameArr = root->getProperty ("names");
            if (nameArr.isArray())
            {
                auto& dst = mb::customChannelNames();
                dst.clear();
                for (const auto& nv : *nameArr.getArray())
                    dst.add (nv.toString());
            }
            const auto& colArr = root->getProperty ("colours");
            if (colArr.isArray())
            {
                auto& dst = mb::ui::customChannelColours();
                dst.clear();
                for (const auto& cv : *colArr.getArray())
                    dst.emplace_back (static_cast<uint32> (static_cast<int> (cv)));
            }
            const auto& iconArr = root->getProperty ("icons");
            if (iconArr.isArray())
            {
                auto& dst = mb::customChannelIcons();
                dst.clear();
                for (const auto& iv : *iconArr.getArray())
                    dst.add (iv.toString());
            }

            // 槽位状态：0 正常 / 1 空槽 / 2 隐藏（旧预设无此字段时全部按正常）
            auto& slotDst = mb::ui::channelSlots();
            slotDst.assign (static_cast<size_t> (channels), mb::ui::kSlotNormal);
            const auto& slotArr = root->getProperty ("slots");
            if (slotArr.isArray())
                for (int c = 0; c < channels && c < slotArr.size(); ++c)
                    slotDst[static_cast<size_t> (c)] = static_cast<std::uint8_t> (
                        juce::jlimit (0, 2, static_cast<int> (slotArr[c])));

            // 右对齐分组（旧文件无此字段时全部按普通通道）
            auto& alignDst = mb::ui::rightAlignedState();
            alignDst.assign (static_cast<size_t> (channels), 0);
            const auto& alignArr = root->getProperty ("aligned");
            if (alignArr.isArray())
                for (int c = 0; c < channels && c < alignArr.size(); ++c)
                    alignDst[static_cast<size_t> (c)] =
                        static_cast<std::uint8_t> (static_cast<int> (alignArr[c]) != 0);

            // 「对齐前位置」记忆（旧文件无此字段时全部 -1 = 回退到分组左缘）
            auto& originDst = mb::ui::rightAlignedOrigin();
            originDst.assign (static_cast<size_t> (channels), -1);
            const auto& originArr = root->getProperty ("alignOrigin");
            if (originArr.isArray())
                for (int c = 0; c < channels && c < originArr.size(); ++c)
                    originDst[static_cast<size_t> (c)] = static_cast<int> (originArr[c]);

            // 通道制式（旧文件无此字段时全部按单声道）
            auto& stereoDst = mb::ui::channelStereoState();
            stereoDst.assign (static_cast<size_t> (channels), 0);
            const auto& stereoArr = root->getProperty ("stereo");
            if (stereoArr.isArray())
                for (int c = 0; c < channels && c < stereoArr.size(); ++c)
                    stereoDst[static_cast<size_t> (c)] =
                        static_cast<std::uint8_t> (static_cast<int> (stereoArr[c]) != 0); 

            // 通道链接（旧文件无此字段时全部解绑；linkKey/linkMods 缺省 = 未设快捷键）
            mb::links::resize (channels);
            const auto& linkActArr    = root->getProperty ("linkAct");
            const auto& linkLetterArr = root->getProperty ("linkLetter");
            const auto& linkIdxArr    = root->getProperty ("linkIdx");
            const auto& linkKeyArr    = root->getProperty ("linkKey");
            const auto& linkModsArr   = root->getProperty ("linkMods");
            if (linkActArr.isArray() && linkLetterArr.isArray() && linkIdxArr.isArray())
            {
                const int actN = mb::links::kActionCount;
                if (linkActArr.size() >= channels * actN)
                {
                    // 新格式：每通道 × 每动作各一条（下标 = c * kActionCount + a）
                    for (int c = 0; c < channels; ++c)
                        for (int a = 0; a < actN; ++a)
                        {
                            const int i = c * actN + a;
                            if (i >= linkActArr.size() || i >= linkLetterArr.size()
                                || i >= linkIdxArr.size()) break;
                            mb::links::LinkSpec s;
                            s.letter = juce::jlimit (0, mb::links::kLetterCount - 1,
                                                     static_cast<int> (linkLetterArr[i]));
                            s.index  = juce::jlimit (0, mb::links::kIndexCount,
                                                     static_cast<int> (linkIdxArr[i]));
                            if (linkKeyArr.isArray()  && i < linkKeyArr.size())  s.key  = static_cast<int> (linkKeyArr[i]);
                            if (linkModsArr.isArray() && i < linkModsArr.size()) s.mods = static_cast<int> (linkModsArr[i]);
                            mb::links::setLink (c, a, s);
                        }
                }
                else
                {
                    // 旧格式：每通道一条 → 迁移到其动作槽位，其余动作留空
                    for (int c = 0; c < channels && c < linkActArr.size()
                                      && c < linkLetterArr.size() && c < linkIdxArr.size(); ++c)
                    {
                        mb::links::LinkSpec s;
                        s.action = juce::jlimit (0, mb::links::kActionCount - 1, static_cast<int> (linkActArr[c]));
                        s.letter = juce::jlimit (0, mb::links::kLetterCount - 1, static_cast<int> (linkLetterArr[c]));
                        s.index  = juce::jlimit (0, mb::links::kIndexCount,  static_cast<int> (linkIdxArr[c]));
                        if (linkKeyArr.isArray()  && c < linkKeyArr.size())  s.key  = static_cast<int> (linkKeyArr[c]);
                        if (linkModsArr.isArray() && c < linkModsArr.size()) s.mods = static_cast<int> (linkModsArr[c]);
                        mb::links::setLink (c, s.action, s);
                    }
                }
            }

            const auto& edges = root->getProperty ("edges");
            if (edges.isArray())
            {
                std::string why;
                for (const auto& ev : *edges.getArray())
                {
                    auto* o = ev.getDynamicObject();
                    if (o == nullptr) continue;
                    mb::RouteEdge e;
                    e.src  = static_cast<int> (o->getProperty ("s"));
                    e.dst  = static_cast<int> (o->getProperty ("d"));
                    e.kind = static_cast<mb::EdgeKind> (static_cast<int> (o->getProperty ("k")));
                    e.gain = static_cast<float> (static_cast<double> (o->getProperty ("g")));
                    e.side = o->hasProperty ("sd")
                               ? static_cast<int> (o->getProperty ("sd")) : -1; // 旧会话 = 双声道
                    // 非法边（成环/自环/负增益）静默丢弃；端口越界 = 设备不匹配，
                    // 记入掉边表供 UI 提示 + NodeView 悬空角标（不再静默）。
                    if (! g.tryAddEdge (e, why))
                    {
                        const bool badPort =
                            (e.kind == mb::EdgeKind::InputToChannel
                                 && e.src >= g.inputPortCount())
                         || (e.kind == mb::EdgeKind::ChannelToOutput
                                 && e.dst >= g.outputPortCount());
                        if (badPort)
                        {
                            if (e.kind == mb::EdgeKind::InputToChannel)
                                g.recordDroppedEdge ({ e.dst, e.src, 0 });
                            else
                                g.recordDroppedEdge ({ e.src, e.dst, 1 });
                        }
                    }
                }
            }

            const auto& fxArr = root->getProperty ("fx");
            if (fxArr.isArray())
            {
                int c = 0;
                for (const auto& fv : *fxArr.getArray())
                {
                    if (c >= channels || c >= mb::ChannelFxBank::kMaxChannels) break;
                    engine.setFxParams (c, fxFromJson (fv));
                    ++c;
                }
            }

            // 插件链：自带插件按 id 恢复（fx 开关随条目），VST3 按名字匹配创建实例。
            // ch = kFreePluginSlot 时只恢复条目，不碰 FxParams（自由插件不出声）。
            auto restoreChain = [&engine] (int ch, const juce::var& arr)
            {
                auto& bank = engine.pluginBank();
                if (! arr.isArray()) return;
                for (const auto& pv : *arr.getArray())
                {
                    auto* o = pv.getDynamicObject();
                    if (o == nullptr) continue;
                    const auto name = o->getProperty ("n").toString();
                    const bool bypass = static_cast<int> (o->getProperty ("b")) != 0;
                    const int builtInId = static_cast<int> (o->getProperty ("k"));
                    const bool offline = static_cast<int> (o->getProperty ("o")) != 0;
                    const auto note = o->getProperty ("pn").toString();
                    const auto uid  = o->getProperty ("u").toString();

                    if (builtInId > 0)
                    {
                        if (ch != mb::ChannelPluginBank::kFreePluginSlot)
                        {
                            auto params = engine.fxParams (ch);
                            mb::applyBuiltInToParams (params, builtInId, ! bypass);
                            engine.setFxParams (ch, params);
                        }
                        bank.addBuiltIn (ch, builtInId);
                        const int last = bank.pluginCount (ch) - 1;
                        bank.setBypass (ch, last, bypass);
                        bank.setPluginNote (ch, last, note);
                        if (uid.isNotEmpty()) bank.setPluginUid (ch, last, uid);
                        continue;
                    }

                    if (name.isEmpty()) continue;
                    const auto b64   = o->getProperty ("st").toString();
                    const auto ident = o->getProperty ("f").toString();

                    // 离线条目：不创建实例，只占位（状态快照随 st 照常保存）
                    if (offline)
                    {
                        juce::MemoryBlock st;
                        if (b64.isNotEmpty()) st.fromBase64Encoding (b64);
                        bank.addOfflineEntry (ch, name,
                                              ident.isNotEmpty() ? ident : name, st);
                        const int last = bank.pluginCount (ch) - 1;
                        bank.setPluginNote (ch, last, note);
                        if (uid.isNotEmpty()) bank.setPluginUid (ch, last, uid);
                        continue;
                    }

                    auto desc = engine.pluginCatalog().findByIdentifier (ident);
                    if (desc.name.isEmpty()) desc = engine.pluginCatalog().findByName (name);
                    if (desc.name.isEmpty()) continue;          // 未安装：跳过
                    const double sr = engine.sampleRate() > 8000.0
                                    ? engine.sampleRate() : 48000.0;
                    const int bs = engine.bufferSizeSamples() > 0
                                 ? engine.bufferSizeSamples() : 512;
                    juce::String err;
                    auto inst = engine.pluginCatalog().create (desc, err, sr, bs);
                    if (inst == nullptr) continue;

                    juce::MemoryBlock state;
                    if (b64.isNotEmpty()) state.fromBase64Encoding (b64);
                    const auto restoredId = ident.isNotEmpty() ? ident : desc.fileOrIdentifier;
                    if (bank.addPlugin (ch, std::move (inst), sr, bs,
                                        state.getSize() > 0 ? &state : nullptr,
                                        restoredId).isEmpty())
                    {
                        const int last = bank.pluginCount (ch) - 1;
                        bank.setBypass (ch, last, bypass);
                        bank.setPluginNote (ch, last, note);
                        if (uid.isNotEmpty()) bank.setPluginUid (ch, last, uid);
                    }
                }
            };
            const auto& plugArr = root->getProperty ("plugins");
            if (plugArr.isArray())
            {
                int c = 0;
                for (const auto& cv : *plugArr.getArray())
                {
                    if (c >= channels || c >= mb::ChannelPluginBank::kMaxChannels) break;
                    restoreChain (c, cv);
                    ++c;
                }
            }
            // 自由插件每次载入都从存档重建（旧存档无 freePlugins → 清空）
            engine.pluginBank().clearChannel (mb::ChannelPluginBank::kFreePluginSlot);
            restoreChain (mb::ChannelPluginBank::kFreePluginSlot,
                          root->getProperty ("freePlugins"));
            // 「音频播放」播放器状态恢复（v4；旧存档无此字段时保持默认）
            const auto& mpArr = root->getProperty ("players");
            if (mpArr.isArray())
            {
                int c = 0;
                for (const auto& mv : *mpArr.getArray())
                {
                    if (c >= channels || c >= mb::MediaPlayerBank::kMaxChannels) break;
                    auto* o = mv.getDynamicObject();
                    if (o != nullptr)
                    {
                        // 只在该通道确实挂了「音频播放」自带插件时恢复
                        bool hasPlayer = false;
                        auto& bank = engine.pluginBank();
                        for (int i = 0; i < bank.pluginCount (c); ++i)
                            if (bank.pluginBuiltInId (c, i) == 9) { hasPlayer = true; break; }
                        if (! hasPlayer) { ++c; continue; }

                        auto& mp = engine.mediaPlayers();
                        auto& p = mp.ensurePlayer (c);
                        const auto& paths = o->getProperty ("files");
                        if (paths.isArray())
                        {
                            std::vector<juce::File> files;
                            for (const auto& pv : *paths.getArray())
                            {
                                const auto f = juce::File (pv.toString());
                                if (f.existsAsFile()) files.push_back (f);
                            }
                            p.setPlaylist (std::move (files));
                        }
                        p.volume.store (juce::jlimit (0.0f, 2.0f,
                            static_cast<float> (static_cast<double> (o->getProperty ("vol")))),
                            std::memory_order_relaxed);
                        p.mode.store (juce::jlimit (0, 3,
                            static_cast<int> (o->getProperty ("mode"))), std::memory_order_relaxed);
                        p.offsetMs.store (juce::jlimit (-10000, 10000,
                            static_cast<int> (o->getProperty ("off"))), std::memory_order_relaxed);
                        const int idx = static_cast<int> (o->getProperty ("idx"));
                        const double pos = static_cast<double> (o->getProperty ("pos"));
                        // 启动不自动播放：恢复到保存的曲目与位置，但保持暂停，
                        // 等用户点「播放」才出声（避免一开应用就爆出声音）。
                        if (idx >= 0 && idx < static_cast<int> (p.files.size()))
                            p.loadTrack (idx, false, pos > 0.0 ? pos : -1.0);
                    }
                    ++c;
                }
            }

            // 兼容旧版存档：旧版删除通道只标记空槽沉底（通道数固定）。
            // 现已改为真删除，这里把恢复出来的空槽通道逐一真删除，
            // 图 / 插件链 / 内置效果参数与随通道状态整体前移，通道数收缩。
            for (int c = channels - 1; c >= 0; --c)
            {
                auto& slotV = mb::ui::channelSlots();
                if (c >= static_cast<int> (slotV.size())
                    || slotV[static_cast<size_t> (c)] != mb::ui::kSlotEmpty) continue;

                g.removeChannel (c);
                engine.pluginBank().removeChannel (c);
                engine.mediaPlayers().removeChannel (c);
                for (int i = c; i < g.channelCount(); ++i)
                    engine.setFxParams (i, engine.fxParams (i + 1));

                auto& names = mb::customChannelNames();
                if (c < names.size()) names.removeRange (c, 1);
                auto& icons = mb::customChannelIcons();
                if (c < icons.size()) icons.removeRange (c, 1);
                auto& cols = mb::ui::customChannelColours();
                if (c < static_cast<int> (cols.size())) cols.erase (cols.begin() + c);
                auto eraseAt = [c] (std::vector<std::uint8_t>& v)
                {
                    if (c < static_cast<int> (v.size())) v.erase (v.begin() + c);
                };
                eraseAt (mb::ui::channelArmState());
                eraseAt (mb::ui::rightAlignedState());
                eraseAt (mb::ui::channelStereoState());
                eraseAt (mb::ui::channelSlots());
                {
                    auto& origins = mb::ui::rightAlignedOrigin();
                    if (c < static_cast<int> (origins.size())) origins.erase (origins.begin() + c);
                }
                auto& links = mb::links::table();
                if (c < static_cast<int> (links.size())) links.erase (links.begin() + c);
            }

            // 路由内核与 UI / 设备同步：立体声标记 + 端口 L/R 配对。
            // 放在末尾，覆盖上面的通道真删除前移；compile() 据此展开缓冲级 L/R 边。
            g.setStereoFlags (stereoDst);
            g.setPortPairs (derivePortPairs (engine.catalog(), true),
                            derivePortPairs (engine.catalog(), false));
            return true;
        }

        static bool save (const juce::File& file, const juce::var& v)
        {
            file.getParentDirectory().createDirectory();
            return file.replaceWithText (juce::JSON::toString (v, true));
        }

        static juce::var load (const juce::File& file)
        {
            const auto text = file.loadFileAsString();
            if (text.isEmpty()) return {};
            return juce::JSON::parse (text);
        }

        //======================================================================
        static juce::var fxToJson (const mb::FxParams& p)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("hpfOn", p.hpfOn);       o->setProperty ("hpfHz", p.hpfHz);
            o->setProperty ("eqLow", p.eqLowDb);     o->setProperty ("eqMid", p.eqMidDb);
            o->setProperty ("eqMidHz", p.eqMidHz);   o->setProperty ("eqHigh", p.eqHighDb);
            o->setProperty ("gateOn", p.gateOn);     o->setProperty ("gateDb", p.gateThreshDb);
            o->setProperty ("compOn", p.compOn);     o->setProperty ("compDb", p.compThreshDb);
            o->setProperty ("compRatio", p.compRatio);
            o->setProperty ("compAtk", p.compAtkMs); o->setProperty ("compRel", p.compRelMs);
            o->setProperty ("drvOn", p.driveOn);     o->setProperty ("drvAmt", p.driveAmt);
            o->setProperty ("drvMix", p.driveMix);
            o->setProperty ("dlyOn", p.delayOn);     o->setProperty ("dlyMs", p.delayMs);
            o->setProperty ("dlyFb", p.delayFb);     o->setProperty ("dlyMix", p.delayMix);
            o->setProperty ("revOn", p.reverbOn);    o->setProperty ("revMix", p.reverbMix);
            o->setProperty ("revSize", p.reverbSize);
            o->setProperty ("limOn", p.limitOn);     o->setProperty ("limDb", p.limitCeilDb);
            o->setProperty ("duckOn", p.duckOn);     o->setProperty ("duckSrc", p.duckSrcCh);
            o->setProperty ("duckDb", p.duckThreshDb);
            o->setProperty ("duckDepth", p.duckDepthDb);
            o->setProperty ("duckAtk", p.duckAtkMs); o->setProperty ("duckRel", p.duckRelMs);
            return juce::var (o);
        }

        static mb::FxParams fxFromJson (const juce::var& v)
        {
            mb::FxParams p;
            auto* o = v.getDynamicObject();
            if (o == nullptr) return p;
            auto getB = [&] (const juce::Identifier& id, bool def)
            {
                const auto x = o->getProperty (id);
                return x.isVoid() ? def : (static_cast<int> (x) != 0);
            };
            auto getF = [&] (const juce::Identifier& id, float def)
            {
                const auto x = o->getProperty (id);
                return x.isVoid() ? def : static_cast<float> (static_cast<double> (x));
            };
            p.hpfOn = getB ("hpfOn", false);   p.hpfHz = getF ("hpfHz", 90.0f);
            p.eqLowDb = getF ("eqLow", 0.0f);  p.eqMidDb = getF ("eqMid", 0.0f);
            p.eqMidHz = getF ("eqMidHz", 1000.0f);
            p.eqHighDb = getF ("eqHigh", 0.0f);
            p.gateOn = getB ("gateOn", false); p.gateThreshDb = getF ("gateDb", -52.0f);
            p.compOn = getB ("compOn", false); p.compThreshDb = getF ("compDb", -24.0f);
            p.compRatio = getF ("compRatio", 3.0f);
            p.compAtkMs = getF ("compAtk", 12.0f);
            p.compRelMs = getF ("compRel", 180.0f);
            p.driveOn = getB ("drvOn", false); p.driveAmt = getF ("drvAmt", 3.0f);
            p.driveMix = getF ("drvMix", 0.35f);
            p.delayOn = getB ("dlyOn", false); p.delayMs = getF ("dlyMs", 240.0f);
            p.delayFb = getF ("dlyFb", 0.35f); p.delayMix = getF ("dlyMix", 0.25f);
            p.reverbOn = getB ("revOn", false); p.reverbMix = getF ("revMix", 0.22f);
            p.reverbSize = getF ("revSize", 0.5f);
            p.limitOn = getB ("limOn", false); p.limitCeilDb = getF ("limDb", -1.0f);
            p.duckOn = getB ("duckOn", false);
            p.duckSrcCh = o->getProperty ("duckSrc").isVoid()
                        ? -1 : juce::jlimit (-1, 31, static_cast<int> (o->getProperty ("duckSrc")));
            p.duckThreshDb = getF ("duckDb", -28.0f);
            p.duckDepthDb = getF ("duckDepth", -12.0f);
            p.duckAtkMs = getF ("duckAtk", 15.0f);
            p.duckRelMs = getF ("duckRel", 350.0f);
            return p;
        }
    };
}
