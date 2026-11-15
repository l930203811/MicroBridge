#include "RouteGraph.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (! (expr))                                                          \
        {                                                                      \
            std::printf ("FAIL line %d: %s\n", __LINE__, #expr);                \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

static int rankOf (const mb::ProcessingPlan& p, int channel)
{
    for (size_t i = 0; i < p.channelOrder.size(); ++i)
        if (p.channelOrder[i] == channel)
            return static_cast<int> (i);
    return -1;
}

int main()
{
    using namespace mb;
    std::string why;

    // 1. 三边模型：输入端口 0/1 -> CH0，CH0 -> CH1，CH0/CH1 -> 输出端口
    RouteGraph g (3, 4, 5);
    CHECK (g.channelCount() == 3);
    CHECK (g.inputPortCount() == 4);
    CHECK (g.outputPortCount() == 5);

    CHECK (g.tryAddEdge ({ 0, 0, EdgeKind::InputToChannel,  1.0f }, why));
    CHECK (g.tryAddEdge ({ 1, 0, EdgeKind::InputToChannel,  0.5f }, why));
    CHECK (g.tryAddEdge ({ 0, 1, EdgeKind::ChannelToChannel, 0.7f }, why));
    CHECK (g.tryAddEdge ({ 0, 2, EdgeKind::ChannelToOutput,  1.0f }, why));
    CHECK (g.tryAddEdge ({ 1, 3, EdgeKind::ChannelToOutput,  0.8f }, why));

    auto p = g.plan();
    CHECK (p != nullptr);
    CHECK (p->channelOrder.size() == 3);
    CHECK (p->inputFeeds.size() == 2);
    CHECK (p->channelSends.size() == 1);
    CHECK (p->outputSends.size() == 2);
    CHECK (p->edgeCount() == 5);
    CHECK (rankOf (*p, 0) >= 0 && rankOf (*p, 1) > rankOf (*p, 0));  // 拓扑序
    CHECK (std::abs (p->inputFeeds[0].gain - 1.0f) < 1e-6f);          // 输入边不含推子
    CHECK (std::abs (p->channelSends[0].gain - 0.7f) < 1e-6f);        // 出边含推子(=1)

    // 2. 输入边不参与环检测；C->C 成环必须拒绝且不污染图
    CHECK (! g.tryAddEdge ({ 1, 0, EdgeKind::ChannelToChannel, 1.0f }, why));
    CHECK (! why.empty());
    CHECK (g.edges().size() == 5);
    CHECK (g.tryAddEdge ({ 0, 2, EdgeKind::InputToChannel, 1.0f }, why));  // 同端口多通道合法
    CHECK (g.edges().size() == 6);
    g.removeEdge (0, 2, EdgeKind::InputToChannel);

    // 3. 自环 / 越界 / 负增益 / 非有限值
    CHECK (! g.tryAddEdge ({ 0, 0, EdgeKind::ChannelToChannel, 1.0f }, why));
    CHECK (! g.tryAddEdge ({ 9, 1, EdgeKind::ChannelToChannel, 1.0f }, why));
    CHECK (! g.tryAddEdge ({ 4, 0, EdgeKind::InputToChannel,  1.0f }, why));  // 端口越界
    CHECK (! g.tryAddEdge ({ 0, 9, EdgeKind::InputToChannel,  1.0f }, why));  // 通道越界
    CHECK (! g.tryAddEdge ({ 0, 5, EdgeKind::ChannelToOutput, 1.0f }, why));
    CHECK (! g.tryAddEdge ({ 0, 1, EdgeKind::ChannelToOutput, -1.0f }, why));
    CHECK (! g.tryAddEdge ({ 0, 1, EdgeKind::ChannelToOutput,
                             std::numeric_limits<float>::quiet_NaN() }, why));

    // 4. 重复加同一条边 = 更新增益，边数不变
    CHECK (g.tryAddEdge ({ 0, 1, EdgeKind::ChannelToChannel, 0.25f }, why));
    CHECK (g.edges().size() == 5);
    CHECK (std::abs (g.plan()->channelSends[0].gain - 0.25f) < 1e-6f);

    // 5. 快照不可变：改图后旧 shared_ptr 内容不变
    auto oldPlan = g.plan();
    g.setFader (0, 0.5f);
    auto newPlan = g.plan();
    CHECK (oldPlan.get() != newPlan.get());
    CHECK (std::abs (oldPlan->channelSends[0].gain - 0.25f) < 1e-6f);
    CHECK (std::abs (newPlan->channelSends[0].gain - 0.125f) < 1e-6f);
    CHECK (std::abs (newPlan->outputSends[0].gain - 0.5f) < 1e-6f);
    CHECK (std::abs (newPlan->inputFeeds[0].gain - 1.0f) < 1e-6f);     // 输入不受推子影响
    g.setFader (0, 1.0f);

    // 6. 静音 = 断开该通道所有出边（输入边保留）
    g.setMuted (0, true);
    for (const auto& e : g.plan()->channelSends) CHECK (e.src != 0);
    for (const auto& e : g.plan()->outputSends)  CHECK (e.src != 0);
    CHECK (g.plan()->active[0] == 0);
    CHECK (g.plan()->inputFeeds.size() == 2);
    g.setMuted (0, false);

    // 7. 独奏 = 只保留独奏通道的出边（缓冲索引 2*ch，CH1 单声道 → 2）
    g.setSolo (1, true);
    for (const auto& e : g.plan()->channelSends) CHECK (e.src == 2);
    for (const auto& e : g.plan()->outputSends)  CHECK (e.src == 2);
    g.clearSolo();

    // 8. 零增益稀疏化
    g.setFader (0, 0.0f);
    for (const auto& e : g.plan()->channelSends) CHECK (e.src != 0);
    for (const auto& e : g.plan()->outputSends)  CHECK (e.src != 0);
    g.setFader (0, 1.0f);

    // 9. 一条通道可同时送任意多个输出端口（无总线），也可有多路输入源
    RouteGraph h (1, 3, 2);
    for (int i = 0; i < 3; ++i)
        CHECK (h.tryAddEdge ({ i, 0, EdgeKind::InputToChannel, 1.0f }, why));
    CHECK (h.tryAddEdge ({ 0, 0, EdgeKind::ChannelToOutput, 1.0f }, why));
    CHECK (h.tryAddEdge ({ 0, 1, EdgeKind::ChannelToOutput, 0.7f }, why));
    CHECK (h.plan()->inputFeeds.size() == 3);
    CHECK (h.plan()->outputSends.size() == 2);

    // 10. 删边后重新编译
    CHECK (h.removeEdge (0, 0, EdgeKind::ChannelToOutput));
    CHECK (h.plan()->outputSends.size() == 1);
    CHECK (! h.removeEdge (0, 0, EdgeKind::ChannelToOutput));
    CHECK (h.removeEdge (2, 0, EdgeKind::InputToChannel));
    CHECK (h.plan()->inputFeeds.size() == 2);

    // 11. resize 清空图并重建快照
    g.resize (2, 2, 2);
    CHECK (g.edges().empty());
    CHECK (g.plan()->channelCount == 2);
    CHECK (g.plan()->empty());

    // 12. addChannel 追加通道，保留既有边 / 推子 / 状态
    RouteGraph k (2, 2, 2);
    CHECK (k.tryAddEdge ({ 0, 1, EdgeKind::ChannelToChannel, 0.5f }, why));
    CHECK (k.tryAddEdge ({ 0, 0, EdgeKind::ChannelToOutput,  1.0f }, why));
    k.setFader (0, 0.5f);
    const int newId = k.addChannel();
    CHECK (newId == 2);
    CHECK (k.channelCount() == 3);
    CHECK (k.edges().size() == 2);
    CHECK (k.plan()->channelCount == 3);
    CHECK (std::abs (k.plan()->faders[0] - 0.5f) < 1e-6f);
    CHECK (k.tryAddEdge ({ 2, 1, EdgeKind::ChannelToOutput, 1.0f }, why));

    // 13. clearEdges 只清连接
    k.clearEdges();
    CHECK (k.edges().empty());
    CHECK (k.channelCount() == 3);
    CHECK (k.plan()->empty());
    CHECK (std::abs (k.plan()->faders[0] - 0.5f) < 1e-6f);

    // 14. setPortCounts 缩减端口时丢弃越界端口边，保留通道互送
    RouteGraph q (2, 4, 4);
    CHECK (q.tryAddEdge ({ 3, 0, EdgeKind::InputToChannel,  1.0f }, why));
    CHECK (q.tryAddEdge ({ 0, 3, EdgeKind::ChannelToOutput, 1.0f }, why));
    CHECK (q.tryAddEdge ({ 0, 1, EdgeKind::ChannelToChannel, 1.0f }, why));
    q.setPortCounts (2, 2);
    CHECK (q.plan()->inputFeeds.empty());
    CHECK (q.plan()->outputSends.empty());
    CHECK (q.plan()->channelSends.size() == 1);
    CHECK (q.edges().size() == 1);

    // 15. reorderChannels：推子/静音/连接随通道移动（order[newPos] = oldIdx）
    RouteGraph r (4, 2, 3);
    CHECK (r.tryAddEdge ({ 0, 2, EdgeKind::ChannelToChannel, 0.6f }, why));
    CHECK (r.tryAddEdge ({ 1, 0, EdgeKind::InputToChannel,  1.0f }, why));
    CHECK (r.tryAddEdge ({ 3, 1, EdgeKind::ChannelToOutput, 0.9f }, why));
    r.setFader (2, 0.4f);
    r.setMuted (1, true);
    r.reorderChannels ({ 2, 0, 1, 3 });            // 老 CH2 移到第 0 位
    CHECK (std::abs (r.fader (0) - 0.4f) < 1e-6f); // 老 CH2 的推子跟着走
    CHECK (r.isMuted (2));                          // 老 CH1 的静音跟着走
    CHECK (! r.isMuted (1));
    {
        bool sawCC = false, sawIn = false, sawOut = false;
        for (const auto& e : r.edges())
        {
            if (e.kind == EdgeKind::ChannelToChannel)
            {
                sawCC = true;
                CHECK (e.src == 1 && e.dst == 0);   // 老 0→2 重标为 1→0
            }
            else if (e.kind == EdgeKind::InputToChannel)
            {
                sawIn = true;
                CHECK (e.src == 1 && e.dst == 1);   // 输入端口不变，目标通道跟随
            }
            else
            {
                sawOut = true;
                CHECK (e.src == 3 && e.dst == 1);   // 输出端口不变，来源通道跟随
            }
        }
        CHECK (sawCC && sawIn && sawOut);
    }
    // 非法 order 一律拒绝且图不变
    r.reorderChannels ({ 0, 1 });
    r.reorderChannels ({ 0, 0, 2, 3 });
    CHECK (r.edges().size() == 3);
    CHECK (std::abs (r.fader (0) - 0.4f) < 1e-6f);

    // 16. removeChannel：连接删除、后继通道前移补位
    RouteGraph d (4, 2, 3);
    CHECK (d.tryAddEdge ({ 0, 1, EdgeKind::InputToChannel, 1.0f }, why));   // dst==1 删除
    CHECK (d.tryAddEdge ({ 0, 3, EdgeKind::InputToChannel, 0.9f }, why));   // dst 3 → 2
    CHECK (d.tryAddEdge ({ 0, 1, EdgeKind::ChannelToOutput, 0.9f }, why));  // src=CH0、dst=端口1，保留
    CHECK (d.tryAddEdge ({ 1, 3, EdgeKind::ChannelToChannel, 0.5f }, why)); // 两端命中删除
    CHECK (d.tryAddEdge ({ 3, 0, EdgeKind::ChannelToOutput, 0.8f }, why));  // src 3 → 2
    d.setFader (2, 0.7f);
    d.removeChannel (1);
    CHECK (d.channelCount() == 3);
    CHECK (std::abs (d.fader (1) - 0.7f) < 1e-6f);     // 老 CH3 前移到 1
    bool ok16 = true;
    for (const auto& e : d.edges())
    {
        if (e.kind == EdgeKind::InputToChannel)        // 保留的输入边目标前移
            ok16 &= (e.src == 0 && e.dst == 2);
        else if (e.kind == EdgeKind::ChannelToOutput)  // 来源通道前移，端口不变
            ok16 &= ((e.src == 2 && e.dst == 0) || (e.src == 0 && e.dst == 1));
        else
            ok16 = false;                               // C→C 已全部删除
    }
    CHECK (ok16);
    CHECK (d.edges().size() == 3);

    // 17. 立体声展开：缓冲级 L/R 边、端口配对、居中规则
    //     CH0 立体声、CH1 单声道；输入/输出端口 (0,1) 配对为 L/R。
    RouteGraph s (2, 4, 4);
    s.setChannelStereo (0, true);
    s.setPortPairs ({{0, 1}}, {{0, 1}});
    CHECK (s.tryAddEdge ({ 0, 0, EdgeKind::InputToChannel,  1.0f }, why)); // port0→CH0 L
    CHECK (s.tryAddEdge ({ 1, 0, EdgeKind::InputToChannel,  0.5f }, why)); // port1→CH0 R
    CHECK (s.tryAddEdge ({ 1, 0, EdgeKind::ChannelToChannel, 0.7f }, why)); // CH1(单)→CH0(立体)
    CHECK (s.tryAddEdge ({ 0, 0, EdgeKind::ChannelToOutput,  1.0f }, why)); // CH0 L→out0
    CHECK (s.tryAddEdge ({ 1, 2, EdgeKind::ChannelToOutput,  0.8f }, why)); // CH1→out2
    auto sp = s.plan();
    // 输入：配对端口 → 正确的 L/R 缓冲（port0→0, port1→1）
    bool sawInL = false, sawInR = false;
    for (const auto& e : sp->inputFeeds)
    {
        if (e.src == 0 && e.dst == 0) sawInL = true;   // port0 → CH0 L
        if (e.src == 1 && e.dst == 1) sawInR = true;   // port1 → CH0 R
    }
    CHECK (sawInL && sawInR);
    CHECK (sp->inputFeeds.size() == 2);
    // 单声道 → 立体声：CH1 缓冲 2 复制进 CH0 的 L(0)/R(1)（居中）
    bool sawCenter0 = false, sawCenter1 = false;
    for (const auto& e : sp->channelSends)
    {
        if (e.src == 2 && e.dst == 0) sawCenter0 = true;
        if (e.src == 2 && e.dst == 1) sawCenter1 = true;
    }
    CHECK (sawCenter0 && sawCenter1);
    CHECK (sp->channelSends.size() == 2);
    // 立体声 → 输出配对：CH0 L(0)→out0，CH0 R(1)→out1；CH1 单声道 → out2(缓冲 2)
    bool sawOutL = false, sawOutR = false, sawMonoOut = false;
    for (const auto& e : sp->outputSends)
    {
        if (e.src == 0 && e.dst == 0) sawOutL = true;
        if (e.src == 1 && e.dst == 1) sawOutR = true;
        if (e.src == 2 && e.dst == 2) sawMonoOut = true;
    }
    CHECK (sawOutL && sawOutR && sawMonoOut);
    CHECK (sp->outputSends.size() == 3);
    // 立体声 → 单声道输出（无配对端口）：L+R 求和进同一端口
    RouteGraph t (1, 0, 2);
    t.setChannelStereo (0, true);
    CHECK (t.tryAddEdge ({ 0, 0, EdgeKind::ChannelToOutput, 1.0f }, why));
    auto tp = t.plan();
    CHECK (tp->outputSends.size() == 2);            // L(0)→out0 与 R(1)→out0
    bool sawSum = false;
    for (const auto& e : tp->outputSends)
        if (e.src == 1 && e.dst == 0) sawSum = true; // R 也进 out0
    CHECK (sawSum);

    // 18. C→C 声道侧（side）：仅 L / 仅 R 发送、合并为双声道、降级保留另一声道
    RouteGraph w (2, 0, 0);
    w.setChannelStereo (0, true);
    w.setChannelStereo (1, true);
    CHECK (w.tryAddEdge ({ 0, 1, EdgeKind::ChannelToChannel, 0.8f, 0 }, why)); // 仅 L
    auto wp = w.plan();
    CHECK (wp->channelSends.size() == 1);
    CHECK (wp->channelSends[0].src == 0 && wp->channelSends[0].dst == 2); // L(0)→L(2)
    CHECK (w.tryAddEdge ({ 0, 1, EdgeKind::ChannelToChannel, 0.8f, 1 }, why)); // 补 R → 合并
    CHECK (w.channelEdgeSide (0, 1) == -1);
    wp = w.plan();
    CHECK (wp->channelSends.size() == 2);                          // L→L 与 R→R
    w.removeChannelEdgeSide (0, 1, 0);                             // 断 L → 剩仅 R
    CHECK (w.channelEdgeSide (0, 1) == 1);
    wp = w.plan();
    CHECK (wp->channelSends.size() == 1);
    CHECK (wp->channelSends[0].src == 1 && wp->channelSends[0].dst == 3); // R(1)→R(3)
    CHECK (w.removeEdge (0, 1, EdgeKind::ChannelToChannel));       // 整条移除
    CHECK (w.channelEdgeSide (0, 1) == -2);
    // 单声道源没有 R 路：side=1 不产生任何发送
    RouteGraph x18 (2, 0, 0);
    CHECK (x18.tryAddEdge ({ 0, 1, EdgeKind::ChannelToChannel, 1.0f, 1 }, why));
    CHECK (x18.plan()->channelSends.empty());

    if (failures == 0)
        std::printf ("RouteGraphTest: ALL PASS (18 groups)\n");
    else
        std::printf ("RouteGraphTest: %d FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
