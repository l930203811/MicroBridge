#pragma once

//==============================================================================
// MicroBridge 路由内核 —— 纯 C++20，不依赖 JUCE，可在音频线程外独立测试。
//
// 端口化三边模型（没有总线 Bus 节点）：
//   P -> C  物理输入端口 -> 通道   （每条通道的输入源可任意指定，可多路求和）
//   C -> C  通道 -> 通道          （A 的输出可作为 B 的输入源）
//   C -> P  通道 -> 物理输出端口  （可同时送本机任意声卡驱动）
//
// 「通道」既不是输入设备也不是输出设备，它是一段可增益的信号总线以外的
// 独立信号流：输入 = 所有 P->C 与指向它的 C->C 之和；输出 = 它发出的全部边。
// 推子 = 该通道所有出边（C->C / C->P）的公共增益；静音 = 断开全部出边；
// 独奏 = 非保活通道真实停用（进入独奏快照启停，取消独奏整体恢复）。
//
// 实时约束：UI/控制线程建图、检测反馈环并编译成不可变 ProcessingPlan，
// 用 atomic<shared_ptr<const Plan>> 换出；音频回调只按序乘加，
// 零分配、零锁、零图遍历。
//==============================================================================

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mb {

using ChannelId = int;
using PortId = int;

enum class EdgeKind : std::uint8_t
{
    InputToChannel,     // src = 输入端口, dst = 通道
    ChannelToChannel,   // src = 通道,    dst = 通道
    ChannelToOutput     // src = 通道,    dst = 输出端口
};

struct RouteEdge
{
    int src = -1;
    int dst = -1;
    EdgeKind kind = EdgeKind::ChannelToOutput;
    float gain = 1.0f;                                 // 线性增益，>= 0
    int side = -1;    // C→C 专用：-1 = 双声道（L+R）/ 0 = 仅 L / 1 = 仅 R（其他边忽略）
};

struct PlanEdge
{
    int src = 0;
    int dst = 0;
    float gain = 0.0f;
};

/** 不可变处理快照。构造完成后不得修改，实时线程只读。 */
struct ProcessingPlan
{
    std::vector<int> channelOrder;       // C->C 子图拓扑序，保证 src 先于 dst
    // 边的「通道」维度一律为缓冲索引：2c = 通道 c 的 L，2c+1 = R。
    // 单声道通道只用 2c（2c+1 恒为零、不被任何边引用）。
    std::vector<PlanEdge> inputFeeds;    // 输入端口 -> 通道缓冲（gain 不含推子）
    std::vector<PlanEdge> channelSends;  // 通道缓冲 -> 通道缓冲（gain 已含源推子）
    std::vector<PlanEdge> outputSends;   // 通道缓冲 -> 输出端口（gain 已含源推子）
    std::vector<float> faders;
    std::vector<std::uint8_t> active;    // 该通道出边是否有效
    int channelCount = 0;
    int inputPortCount = 0;
    int outputPortCount = 0;

    int edgeCount() const noexcept
    {
        return static_cast<int> (inputFeeds.size() + channelSends.size() + outputSends.size());
    }
    bool empty() const noexcept { return edgeCount() == 0; }
};

class RouteGraph
{
public:
    RouteGraph() { publish (compile()); }
    RouteGraph (int channels, int inputPorts, int outputPorts)
    {
        resize (channels, inputPorts, outputPorts);
    }

    RouteGraph (const RouteGraph&) = delete;
    RouteGraph& operator= (const RouteGraph&) = delete;

    /** 独奏豁免钩子：返回 true 的通道在独奏时保持激活，并作为 keepAlive
        种子向下游传播。用于「向右对齐」的监听/输出总线——独奏任何通道都
        不应剪掉总线本身（否则输出到耳机的总线会整条静默）。仅在控制线程
        compile() 时调用，可安全读取 UI 侧的通道标记；静音/停用仍优先剪枝。 */
    std::function<bool (int)> soloExempt;

    void resize (int channels, int inputPorts, int outputPorts)
    {
        channels_ = std::max (0, channels);
        inputs_   = std::max (0, inputPorts);
        outputs_  = std::max (0, outputPorts);
        faders_.assign (static_cast<size_t> (channels_), 1.0f);
        pans_.assign (static_cast<size_t> (channels_), 0.0f);
        muted_.assign (static_cast<size_t> (channels_), 0);
        solo_.assign  (static_cast<size_t> (channels_), 0);
        enabled_.assign (static_cast<size_t> (channels_), 1);
        stereo_.assign (static_cast<size_t> (channels_), 0);
        savedEnabled_.clear();               // 结构性重建：独奏快照一并作废
        edges_.clear();
        publish (compile());
    }

    /** 设备列表变化时更新端口数，并丢弃越界的端口边。
        被丢弃的边记入 lastDropped_，供 UI 提示「设备不匹配的路由未恢复」，
        保存会话/预设成功后由 UI 调 clearDroppedEdges() 复位。 */
    void setPortCounts (int inputPorts, int outputPorts)
    {
        inputs_  = std::max (0, inputPorts);
        outputs_ = std::max (0, outputPorts);
        edges_.erase (std::remove_if (edges_.begin(), edges_.end(), [&] (const RouteEdge& e)
        {
            if (e.kind == EdgeKind::InputToChannel)
            {
                if (e.src >= inputs_)
                {
                    lastDropped_.push_back ({ e.dst, e.src, 0 });   // 通道 ← 端口
                    return true;
                }
                return false;
            }
            if (e.kind == EdgeKind::ChannelToOutput)
            {
                if (e.dst >= outputs_)
                {
                    lastDropped_.push_back ({ e.src, e.dst, 1 });   // 通道 → 端口
                    return true;
                }
                return false;
            }
            return false;
        }), edges_.end());
        publish (compile());
    }

    // ---- 设备不匹配掉边记录（0 = 端口→通道，1 = 通道→端口） ----
    struct DroppedEdge { int channel; int port; int dir; };

    /** 本次加载/设备切换以来被丢弃的路由边（去重由 UI 负责）。 */
    const std::vector<DroppedEdge>& droppedEdges() const noexcept { return lastDropped_; }
    void clearDroppedEdges() noexcept { lastDropped_.clear(); }
    /** 预设装载路径（SessionStore）记录被拒绝的越界端口边。 */
    void recordDroppedEdge (const DroppedEdge& d) { lastDropped_.push_back (d); }

    /** 掉边涉及的通道集合（去重、升序），用于 NodeView 悬空角标。 */
    std::vector<int> droppedChannels() const
    {
        std::vector<int> v;
        for (const auto& d : lastDropped_)
            if (d.channel >= 0 && d.channel < channels_
                && std::find (v.begin(), v.end(), d.channel) == v.end())
                v.push_back (d.channel);
        std::sort (v.begin(), v.end());
        return v;
    }

    /** 立体声通道标记：true = 该通道携带 L/R 两路（缓冲 2c=L, 2c+1=R）。
        compile() 据此把图边展开成缓冲级 L/R 边；改标记即重编译。 */
    void setChannelStereo (int ch, bool on)
    {
        if (! validChannel (ch)) return;
        stereo_[static_cast<size_t> (ch)] = on ? 1 : 0;
        publish (compile());
    }

    /** 通道当前是否为立体声（双声道）。 */
    bool isStereo (int ch) const
    {
        return validChannel (ch) && stereo_[static_cast<size_t> (ch)] != 0;
    }

    /** 批量同步立体声标记（加载会话 / 删除 / 清空后调用）。 */
    void setStereoFlags (const std::vector<std::uint8_t>& flags)
    {
        for (int c = 0; c < channels_; ++c)
            stereo_[static_cast<size_t> (c)] =
                (c < static_cast<int> (flags.size()) && flags[static_cast<size_t> (c)] != 0) ? 1 : 0;
        publish (compile());
    } 

    /** 端口 L/R 配对：每对 (L, R) 端口索引。compile() 据此把立体声通道的边
        映射到正确的 L/R 缓冲。纯整数，保持 RouteGraph 不依赖设备 / UI。 */
    void setPortPairs (const std::vector<std::pair<int, int>>& in,
                       const std::vector<std::pair<int, int>>& out)
    {
        inPairs_  = in;
        outPairs_ = out;
        publish (compile());
    }

    int addChannel()
    {
        const int id = channels_++;
        faders_.push_back (1.0f);
        pans_.push_back (0.0f);
        muted_.push_back (0);
        solo_.push_back (0);
        enabled_.push_back (1);
        stereo_.push_back (0);
        if (! savedEnabled_.empty())         // 独奏进行中加通道：快照同步扩容
            savedEnabled_.push_back (1);
        publish (compile());
        return id;
    }

    /** 通道激活（喇叭）：关闭后该通道整体不出声，独立于静音/独奏。 */
    void setEnabled (int ch, bool on)
    {
        if (! validChannel (ch)) return;
        enabled_[static_cast<size_t> (ch)] = on ? 1 : 0;
        publish (compile());
    }

    bool isEnabled (int ch) const { return validChannel (ch) && enabled_[static_cast<size_t> (ch)] != 0; }

    /** 会话存储用：独奏期间返回快照里的启停状态，避免把独奏扫过的停用
        状态写进会话文件（否则保存时独奏着、重开后通道全是停用）。 */
    bool storedEnabled (int ch) const
    {
        if (! validChannel (ch)) return false;
        if (static_cast<int> (savedEnabled_.size()) == channels_)
            return savedEnabled_[static_cast<size_t> (ch)] != 0;
        return enabled_[static_cast<size_t> (ch)] != 0;
    }

    void clearEdges()
    {
        edges_.clear();
        publish (compile());
    }

    /** 删除通道：其全部连接一并移除，其后通道前移补位（推子/静音/独奏/激活跟随）。 */
    void removeChannel (int ch)
    {
        if (! validChannel (ch)) return;

        edges_.erase (std::remove_if (edges_.begin(), edges_.end(), [&] (const RouteEdge& e)
        {
            if (e.kind == EdgeKind::InputToChannel)  return e.dst == ch;
            if (e.kind == EdgeKind::ChannelToOutput) return e.src == ch;
            return e.src == ch || e.dst == ch;   // C -> C 两端命中都删
        }), edges_.end());

        auto shift = [&] (auto& v)
        {
            for (int i = ch; i < channels_ - 1; ++i)
                v[static_cast<size_t> (i)] = v[static_cast<size_t> (i + 1)];
            v.pop_back();
        };
        shift (faders_);
        shift (pans_);
        shift (muted_);
        shift (solo_);
        shift (enabled_);
        shift (stereo_);
        if (static_cast<int> (savedEnabled_.size()) == channels_)
            shift (savedEnabled_);           // 独奏进行中删通道：快照同步收缩

        // 端口编号不变；通道编号 > ch 的边统一前移一位
        for (auto& e : edges_)
        {
            if (e.kind != EdgeKind::InputToChannel  && e.src > ch) --e.src;
            if (e.kind != EdgeKind::ChannelToOutput && e.dst > ch) --e.dst;
        }

        --channels_;
        publish (compile());
    }

    int channelCount()    const noexcept { return channels_; }
    int inputPortCount()  const noexcept { return inputs_; }
    int outputPortCount() const noexcept { return outputs_; }
    const std::vector<RouteEdge>& edges() const noexcept { return edges_; }

    /** 增加/更新一条边；成环、自环、越界、负增益一律拒绝并写入 why。 */
    bool tryAddEdge (const RouteEdge& e, std::string& why)
    {
        if (! validate (e, why))
            return false;

        for (auto& existing : edges_)
        {
            if (existing.src == e.src && existing.dst == e.dst && existing.kind == e.kind)
            {
                // 同边视为更新增益；C→C 互补接入（L+R 或任一侧 + 全部）→ 双声道
                if (e.kind == EdgeKind::ChannelToChannel && existing.side != e.side)
                    existing.side = -1;
                existing.gain = e.gain;
                publish (compile());
                return true;
            }
        }

        auto trial = edges_;
        trial.push_back (e);
        if (e.kind == EdgeKind::ChannelToChannel && hasCycle (trial, channels_, why))
            return false;                            // 拒绝反馈环，不污染原图

        edges_ = std::move (trial);
        publish (compile());
        return true;
    }

    bool removeEdge (int src, int dst, EdgeKind kind)
    {
        const auto before = edges_.size();
        edges_.erase (std::remove_if (edges_.begin(), edges_.end(), [&] (const RouteEdge& e)
        {
            return e.src == src && e.dst == dst && e.kind == kind;
        }), edges_.end());

        if (edges_.size() == before)
            return false;

        publish (compile());
        return true;
    }

    /** C→C 边的声道侧：-1 双声道 / 0 仅 L / 1 仅 R；无边返回 -2。 */
    int channelEdgeSide (int src, int dst) const
    {
        for (const auto& e : edges_)
            if (e.kind == EdgeKind::ChannelToChannel && e.src == src && e.dst == dst)
                return e.side;
        return -2;
    }

    /** 断开 C→C 连接的一条声道（side = 0 L / 1 R）：
        双声道边降级保留另一声道，单声道边整条删除。 */
    void removeChannelEdgeSide (int src, int dst, int side)
    {
        for (auto it = edges_.begin(); it != edges_.end(); ++it)
            if (it->kind == EdgeKind::ChannelToChannel && it->src == src && it->dst == dst)
            {
                if (it->side == -1) it->side = 1 - side;
                else                it = edges_.erase (it);
                publish (compile());
                return;
            }
    }

    void setFader (int ch, float gain)
    {
        if (! validChannel (ch)) return;
        faders_[static_cast<size_t> (ch)] = std::clamp (gain, 0.0f, 4.0f);
        publish (compile());
    }

    /** 声像：-1 = 全左，0 = 居中，+1 = 全右。等功率法则，居中增益归一为 1.0
        （与未加 pan 时的旧行为一致，不改变现有预设响度）。 */
    void setPan (int ch, float pan)
    {
        if (! validChannel (ch)) return;
        pans_[static_cast<size_t> (ch)] = std::clamp (pan, -1.0f, 1.0f);
        publish (compile());
    }

    void setMuted (int ch, bool on)
    {
        if (! validChannel (ch)) return;
        muted_[static_cast<size_t> (ch)] = on ? 1 : 0;
        publish (compile());
    }

    void setSolo (int ch, bool on)
    {
        if (! validChannel (ch)) return;
        solo_[static_cast<size_t> (ch)] = on ? 1 : 0;
        syncSoloSweep();                     // 独奏成员变化：重算「真实停用」集合
        publish (compile());
    }

    void clearSolo()
    {
        std::fill (solo_.begin(), solo_.end(), 0);
        syncSoloSweep();                     // 独奏清零：恢复独奏前启停快照
        publish (compile());
    }

    /** 独奏保活集合：独奏通道 ∪ 独奏豁免通道（soloExempt，如右对齐总线）
        及其 C→C 下游。保活通道在独奏时保持激活，其余通道被真实停用。 */
    std::vector<std::uint8_t> keepAliveSet() const
    {
        std::vector<std::uint8_t> keep (static_cast<size_t> (channels_), 0);
        for (int c = 0; c < channels_; ++c)
            if (solo_[static_cast<size_t> (c)] != 0
                || (soloExempt && soloExempt (c)))
                keep[static_cast<size_t> (c)] = 1;

        bool changed = true;                 // 不动点传播：保活 → 下游 → 下下游
        while (changed)
        {
            changed = false;
            for (const auto& e : edges_)
                if (e.kind == EdgeKind::ChannelToChannel && e.gain > 0.0f
                    && keep[static_cast<size_t> (e.src)] != 0
                    && keep[static_cast<size_t> (e.dst)] == 0)
                {
                    keep[static_cast<size_t> (e.dst)] = 1;
                    changed = true;
                }
        }
        return keep;
    }

    /** 独奏状态机：有独奏时以进入独奏前的快照为基准，把「不保活」的通道
        真实停用（UI 喇叭灭、电平停，与会话保存语义一致）；独奏清零后整体
        恢复快照。独奏期间每次成员变化都按快照重算，保证集合始终一致
        （独奏期间对其他通道的手动启停在下次独奏变化时回到快照值）。 */
    void syncSoloSweep()
    {
        const bool anySolo = std::any_of (solo_.begin(), solo_.end(),
                                          [] (std::uint8_t v) { return v != 0; });
        if (anySolo)
        {
            if (static_cast<int> (savedEnabled_.size()) != channels_)
                savedEnabled_ = enabled_;    // 首次进入独奏：快照
            enabled_ = savedEnabled_;        // 以快照为基准重算
            const auto keep = keepAliveSet();
            for (int c = 0; c < channels_; ++c)
                if (keep[static_cast<size_t> (c)] == 0)
                    enabled_[static_cast<size_t> (c)] = 0;
        }
        else if (! savedEnabled_.empty())
        {
            enabled_ = savedEnabled_;        // 退出独奏：恢复
            savedEnabled_.clear();
        }
    }

    /** 通道重排：order[newPos] = oldIdx。推子/静音/独奏/激活与全部连接跟随移动。 */
    void reorderChannels (const std::vector<int>& order)
    {
        const int n = channels_;
        if (static_cast<int> (order.size()) != n) return;

        std::vector<int> inv (static_cast<size_t> (n), -1);
        for (int i = 0; i < n; ++i)
        {
            const int old = order[static_cast<size_t> (i)];
            if (old < 0 || old >= n || inv[static_cast<size_t> (old)] != -1) return;
            inv[static_cast<size_t> (old)] = i;
        }

        auto move = [&] (auto& v)
        {
            auto t = v;
            for (int i = 0; i < n; ++i)
                t[static_cast<size_t> (i)] = v[static_cast<size_t> (order[static_cast<size_t> (i)])];
            v = std::move (t);
        };
        move (faders_);
        move (pans_);
        move (muted_);
        move (solo_);
        move (enabled_);
        move (stereo_);

        // 边重映射：输入边只改目标通道，输出边只改来源通道，C->C 两端都改
        for (auto& e : edges_)
        {
            if (e.kind != EdgeKind::InputToChannel)  e.src = inv[static_cast<size_t> (e.src)];
            if (e.kind != EdgeKind::ChannelToOutput) e.dst = inv[static_cast<size_t> (e.dst)];
        }

        publish (compile());
    }

    bool isMuted (int ch) const { return validChannel (ch) && muted_[static_cast<size_t> (ch)] != 0; }
    bool isSolo  (int ch) const { return validChannel (ch) && solo_[static_cast<size_t> (ch)] != 0; }
    float fader  (int ch) const { return validChannel (ch) ? faders_[static_cast<size_t> (ch)] : 0.0f; }
    float pan    (int ch) const { return validChannel (ch) ? pans_[static_cast<size_t> (ch)] : 0.0f; }

    /** 实时线程：整个 processBlock 持有同一份快照。 */
    std::shared_ptr<const ProcessingPlan> plan() const noexcept
    {
        return plan_.load (std::memory_order_acquire);
    }

    /** 对 C->C 子图做 Kahn 拓扑排序，存在环则返回 true。 */
    static bool hasCycle (const std::vector<RouteEdge>& edges, int channels, std::string& why)
    {
        const int n = std::max (0, channels);
        std::vector<std::vector<int>> adj (static_cast<size_t> (n));
        std::vector<int> indeg (static_cast<size_t> (n), 0);

        for (const auto& e : edges)
        {
            if (e.kind != EdgeKind::ChannelToChannel || e.gain <= 0.0f) continue;
            if (e.src < 0 || e.src >= n || e.dst < 0 || e.dst >= n) continue;
            adj[static_cast<size_t> (e.src)].push_back (e.dst);
            ++indeg[static_cast<size_t> (e.dst)];
        }

        std::vector<int> queue;
        queue.reserve (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            if (indeg[static_cast<size_t> (i)] == 0)
                queue.push_back (i);

        int visited = 0;
        for (size_t head = 0; head < queue.size(); ++head)
        {
            ++visited;
            for (int m : adj[static_cast<size_t> (queue[head])])
                if (--indeg[static_cast<size_t> (m)] == 0)
                    queue.push_back (m);
        }

        if (visited != n)
        {
            why = "feedback loop rejected (A -> B -> A)";
            return true;
        }
        return false;
    }

private:
    bool validChannel (int ch) const noexcept { return ch >= 0 && ch < channels_; }

    bool validate (const RouteEdge& e, std::string& why) const
    {
        switch (e.kind)
        {
            case EdgeKind::InputToChannel:
                if (e.src < 0 || e.src >= inputs_)  { why = "input port out of range"; return false; }
                if (! validChannel (e.dst))         { why = "destination channel out of range"; return false; }
                break;

            case EdgeKind::ChannelToChannel:
                if (! validChannel (e.src))         { why = "source channel out of range"; return false; }
                if (! validChannel (e.dst))         { why = "destination channel out of range"; return false; }
                if (e.src == e.dst)                 { why = "self-send rejected"; return false; }
                break;

            case EdgeKind::ChannelToOutput:
                if (! validChannel (e.src))         { why = "source channel out of range"; return false; }
                if (e.dst < 0 || e.dst >= outputs_){ why = "output port out of range"; return false; }
                break;
        }

        if (! std::isfinite (e.gain) || e.gain < 0.0f)
        {
            why = "gain must be finite and non-negative";
            return false;
        }
        return true;
    }

    static std::vector<int> topoOrder (const std::vector<RouteEdge>& edges, int channels)
    {
        const int n = std::max (0, channels);
        std::vector<std::vector<int>> adj (static_cast<size_t> (n));
        std::vector<int> indeg (static_cast<size_t> (n), 0);

        for (const auto& e : edges)
        {
            if (e.kind != EdgeKind::ChannelToChannel || e.gain <= 0.0f) continue;
            if (e.src < 0 || e.src >= n || e.dst < 0 || e.dst >= n) continue;
            adj[static_cast<size_t> (e.src)].push_back (e.dst);
            ++indeg[static_cast<size_t> (e.dst)];
        }

        std::vector<int> order;
        order.reserve (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            if (indeg[static_cast<size_t> (i)] == 0)
                order.push_back (i);

        for (size_t head = 0; head < order.size(); ++head)
            for (int m : adj[static_cast<size_t> (order[head])])
                if (--indeg[static_cast<size_t> (m)] == 0)
                    order.push_back (m);

        if (static_cast<int> (order.size()) < n)       // 防御：异常状态下不漏通道
            for (int i = 0; i < n; ++i)
                if (std::find (order.begin(), order.end(), i) == order.end())
                    order.push_back (i);

        return order;
    }

    std::shared_ptr<ProcessingPlan> compile() const
    {
        auto p = std::make_shared<ProcessingPlan>();
        p->channelCount    = channels_;
        p->inputPortCount  = inputs_;
        p->outputPortCount = outputs_;
        p->faders          = faders_;
        p->channelOrder    = topoOrder (edges_, channels_);

        p->active.assign (static_cast<size_t> (channels_), 1);

        // 独奏的「非保活通道静默」由 setSolo/clearSolo 里的 syncSoloSweep()
        // 以真实停用（enabled_=0）实现：UI 与音频一致、取消独奏可恢复、
        // 会话保存走 storedEnabled() 快照。计划层只保留静音/停用剪枝，
        // 不再做独奏剪枝（否则独奏期间手动重新激活的通道仍然无声）。
        for (int c = 0; c < channels_; ++c)
            if (muted_[static_cast<size_t> (c)]
                || enabled_[static_cast<size_t> (c)] == 0)
                p->active[static_cast<size_t> (c)] = 0;

        std::vector<int> rank (static_cast<size_t> (channels_), 0);
        for (size_t i = 0; i < p->channelOrder.size(); ++i)
            rank[static_cast<size_t> (p->channelOrder[i])] = static_cast<int> (i);

        // ---- 立体声展开辅助：通道缓冲索引 2c = L，2c+1 = R ----
        auto chBuf = [] (int ch, int side) { return 2 * ch + side; };

        // 端口配对：返回与 port 配对的另一端口索引，无配对返回 -1
        auto inPartner = [&] (int port) -> int
        {
            for (const auto& pr : inPairs_)
            {
                if (pr.first == port)  return pr.second;
                if (pr.second == port) return pr.first;
            }
            return -1;
        };
        auto outPartner = [&] (int port) -> int
        {
            for (const auto& pr : outPairs_)
            {
                if (pr.first == port)  return pr.second;
                if (pr.second == port) return pr.first;
            }
            return -1;
        };
        auto dedupe = [] (std::vector<PlanEdge>& v)
        {
            std::vector<PlanEdge> out;
            out.reserve (v.size());
            for (const auto& e : v)
            {
                bool dup = false;
                for (const auto& o : out)
                    if (o.src == e.src && o.dst == e.dst) { dup = true; break; }
                if (! dup) out.push_back (e);
            }
            v = std::move (out);
        };

        for (const auto& e : edges_)
        {
            if (e.gain <= 0.0f) continue;

            if (e.kind == EdgeKind::InputToChannel)
            {
                // 输入边不含推子；通道被静音/未独奏时其输出自然为零，无需剪枝。
                const int p0 = e.src, c = e.dst;
                if (c < 0 || c >= channels_) continue;
                if (stereo_[static_cast<size_t> (c)] != 0)
                {
                    const int partner = inPartner (p0);
                    if (partner >= 0)
                    {
                        const int side = (p0 < partner) ? 0 : 1;   // 较小者为 L
                        p->inputFeeds.push_back ({ p0,      chBuf (c, side),     e.gain });
                        p->inputFeeds.push_back ({ partner, chBuf (c, 1 - side), e.gain });
                    }
                    else   // 未配对的单声道输入进立体声通道：居中（L/R 都喂）
                    {
                        p->inputFeeds.push_back ({ p0, chBuf (c, 0), e.gain });
                        p->inputFeeds.push_back ({ p0, chBuf (c, 1), e.gain });
                    }
                }
                else
                {
                    p->inputFeeds.push_back ({ p0, chBuf (c, 0), e.gain });
                }
                continue;
            }

            if (p->active[static_cast<size_t> (e.src)] == 0) continue;

            const float g = e.gain * faders_[static_cast<size_t> (e.src)];
            if (g <= 0.0f) continue;                 // 稀疏化

            if (e.kind == EdgeKind::ChannelToChannel)
            {
                const int c = e.src, d = e.dst;
                if (c < 0 || c >= channels_ || d < 0 || d >= channels_) continue;
                if (p->active[static_cast<size_t> (d)] == 0) continue;
                const bool cS = stereo_[static_cast<size_t> (c)] != 0;
                const bool dS = stereo_[static_cast<size_t> (d)] != 0;

                // 源通道声相：等功率法则作用于 C→C 送出（居中 = 1.0，与
                // C→P 直连输出同一公式）。声相位置随信号写入下游总线的
                // L/R 缓冲后保留，经总线输出到耳机/扬声器仍可听到左右偏移。
                const float pv = pans_[static_cast<size_t> (c)];
                const float pa = (pv + 1.0f) * 0.5f * 1.57079632679f;   // (p+1)π/4
                const float gl = std::cos (pa) * 1.41421356f;
                const float gr = std::sin (pa) * 1.41421356f;

                if (e.side == 0)                     // 仅 L：源 L → 目标 L（单声道目标同一缓冲）
                {
                    p->channelSends.push_back ({ chBuf (c, 0), chBuf (d, 0), g * gl });
                }
                else if (e.side == 1)                // 仅 R：源 R → 目标 R；单声道源无 R 路
                {
                    if (cS)
                        p->channelSends.push_back ({ chBuf (c, 1),
                                                     dS ? chBuf (d, 1) : chBuf (d, 0), g * gr });
                }
                else if (cS && dS)                        // 立体声 → 立体声
                {
                    p->channelSends.push_back ({ chBuf (c, 0), chBuf (d, 0), g * gl });
                    p->channelSends.push_back ({ chBuf (c, 1), chBuf (d, 1), g * gr });
                }
                else if (cS && ! dS)                 // 立体声 → 单声道：L+R 求和（声相保留能量差）
                {
                    p->channelSends.push_back ({ chBuf (c, 0), chBuf (d, 0), g * gl });
                    p->channelSends.push_back ({ chBuf (c, 1), chBuf (d, 0), g * gr });
                }
                else if (! cS && dS)                 // 单声道 → 立体声：按声相摆位（居中 = 复制）
                {
                    p->channelSends.push_back ({ chBuf (c, 0), chBuf (d, 0), g * gl });
                    p->channelSends.push_back ({ chBuf (c, 0), chBuf (d, 1), g * gr });
                }
                else                                 // 单声道 → 单声道：声相只改能量（居中不变响度）
                {
                    p->channelSends.push_back ({ chBuf (c, 0), chBuf (d, 0), g * (gl + gr) * 0.5f });
                }
            }
            else   // ChannelToOutput
            {
                const int c = e.src, q = e.dst;
                if (c < 0 || c >= channels_ || q < 0 || q >= outputs_) continue;

                // 声像增益：等功率法则，居中归一为 1.0（与未加 pan 的旧行为响度一致）
                const float pv = pans_[static_cast<size_t> (c)];
                const float pa = (pv + 1.0f) * 0.5f * 1.57079632679f;   // (p+1)π/4
                const float gl = std::cos (pa) * 1.41421356f;
                const float gr = std::sin (pa) * 1.41421356f;

                if (stereo_[static_cast<size_t> (c)] != 0)
                {
                    const int partner = outPartner (q);
                    if (partner >= 0)
                    {
                        if (q < partner)
                        {
                            p->outputSends.push_back ({ chBuf (c, 0), q,       g * gl });
                            p->outputSends.push_back ({ chBuf (c, 1), partner, g * gr });
                        }
                        else
                        {
                            p->outputSends.push_back ({ chBuf (c, 0), partner, g * gl });
                            p->outputSends.push_back ({ chBuf (c, 1), q,       g * gr });
                        }
                    }
                    else   // 立体声通道 → 单声道输出：L+R 求和进同一端口（居中 1.0 不变响度）
                    {
                        const float pm = (gl + gr) * 0.5f;
                        p->outputSends.push_back ({ chBuf (c, 0), q, g * pm });
                        p->outputSends.push_back ({ chBuf (c, 1), q, g * pm });
                    }
                }
                else
                {
                    p->outputSends.push_back ({ chBuf (c, 0), q, g * (gl + gr) * 0.5f });
                }
            }
        }

        // 立体声展开后，立体声连接（NodeView 一次加两条边）会在两端各产生一遍
        // 成对边，按 (src,dst) 去重；增益相同，去重安全。
        dedupe (p->inputFeeds);
        dedupe (p->channelSends);
        dedupe (p->outputSends);

        std::stable_sort (p->channelSends.begin(), p->channelSends.end(),
                          [&] (const PlanEdge& a, const PlanEdge& b)
                          {
                              // channelSends 的 src 已是缓冲索引（偶数），除以 2 还原通道
                              return rank[static_cast<size_t> (a.src / 2)]
                                   < rank[static_cast<size_t> (b.src / 2)];
                          });
        return p;
    }

    void publish (std::shared_ptr<ProcessingPlan> p)
    {
        plan_.store (std::shared_ptr<const ProcessingPlan> (std::move (p)),
                     std::memory_order_release);
    }

    int channels_ = 0, inputs_ = 0, outputs_ = 0;
    std::vector<RouteEdge> edges_;
    std::vector<float> faders_;
    std::vector<float> pans_;
    std::vector<DroppedEdge> lastDropped_;
    std::vector<std::uint8_t> muted_, solo_, enabled_, stereo_;
    std::vector<std::uint8_t> savedEnabled_;   // 独奏期间的原启停快照（退出独奏恢复）
    std::vector<std::pair<int, int>> inPairs_, outPairs_;
    std::atomic<std::shared_ptr<const ProcessingPlan>> plan_;
};

} // namespace mb
