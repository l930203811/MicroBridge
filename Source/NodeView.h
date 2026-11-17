#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "AudioEngine.h"
#include "ChannelLinks.h"
#include "DeviceCatalog.h"
#include "I18n.h"
#include "ModernMenu.h"
#include "RouteGraph.h"
#include "Theme.h"

//==============================================================================
// 节点连接视图（三列 patchbay）：
//   左列 = 输入端口（按设备分组） · 中列 = 通道 · 右列 = 输出端口
//   通道卡片左缘 = 接收输入点（空心），右缘 = 输出发送点（实心带芯）；
//   每侧两个连接点：上 L 单声道小点 · 下 R 单声道小点（严格立体声连接）。
//   连任一声道都会自动补齐同设备另一声道的连线（成对落到 L/R 小点，事后可单独断开）；
//   单声道设备只有一条连线，引擎自动把该路复制到立体声通道的 L/R（居中，见 RouteGraph::compile）。
// 与调音台共用同一张 RouteGraph，只是另一种「投影」：
//   · 连线 = 三类边（P→C / C→C / C→P），颜色 = 通道颜色；
//   · C→C 立体声互送画成 L / R 两条曲线（锚定 L/R 小点；两端都单声道时单条），
//     从源通道右缘出发绕行落到目标通道左缘（带方向箭头），全程不穿卡片；
//   · 从连接点按下拖拽到另一连接点松手建线；悬停连线显示 CHa → CHb 标签；
//   · 点击连线弹出移除菜单；成环 / 越界仍由 tryAddEdge 把关并回报原因；
//   · 不提供右键菜单：点击通道 L/R 连接点 = 该点连接列表（逐条可移除）；
//     通道卡片正文三区 = 「+输入 / +插件 / +输出」调用调音台对应浮层；
//     双击通道卡片头部 = 重命名（与调音台共用名字表）；
//   · 连线上随实时电平流动的光点：P→C 随设备输入端口进线电平，
//     C→C / C→P 随源通道电平；20 Hz 由外部驱动 repaint。
// 本组件只画不改布局：所有几何每次 paint 前由 computeLayout() 现算，
// 鼠标命中使用同一份缓存，不存在与调音台不同步的独立状态。
//==============================================================================
namespace mb
{
    class NodeCanvas : public juce::Component
    {
    public:
        std::function<void()> onChanged;                       // 图被修改（建线 / 移除）
        std::function<void (const juce::String&)> onMessage;   // 提示 / 拒绝原因
        std::function<void (int ch)> onAddPlugin;              // 请求打开「添加插件」浮层（经典视图）
        std::function<void (const juce::Point<float>& pos)> onAddFreePlugins; // 空白区右键「+插件」：在光标处添加自由插件
        std::function<void (int ch, int idx)> onOpenPlugin;    // 请求打开插件编辑器
        std::function<void (int ch, int idx)> onRemovePlugin;  // 请求移除插件（主界面收尾）
        std::function<void (int ch, bool input)> onOpenSource; // 请求打开来源选择浮层（+输入 / +输出）
        std::function<void()> onAddChannel;                    // 空白区右键「+通道」
        std::function<void (int ch)> onDeleteChannel;          // 通道右键「删除通道」
        /** 插件移动 / 连接统一回调：srcCh / dstCh 可为 kFreePluginSlot；
            targetIdx >= 0 = 插到该序号之前，-1 = 追加到链尾。 */
        std::function<void (int srcCh, int idx, int dstCh, int targetIdx)> onMovePlugin;
        std::function<void (int ch, int idx, const juce::String& name)> onRenamePlugin; // 插件重命名（写备注）
        // —— 与经典视图右键菜单对齐所需的回调 ——
        std::function<void (int ch)> onDuplicateChannel;
        std::function<void (int ch, bool on)> onSetRightAligned;
        std::function<void (int ch)> onLinkChannel;            // 默认动作 = -1（整通道）
        std::function<void (int ch)> onPickColour;
        std::function<void (int ch, bool hidden)> onSetHidden;
        std::function<void()> onShowAllChannels;
        std::function<void (int ch, int idx)> onPluginBypass;
        std::function<void (int ch, int idx)> onPluginOffline;
        std::function<void (int ch, int action)> onPluginLink;
        std::function<void()> onSnapChanged;                 // 吸附状态变化（刷新固定工具栏按钮）

        /** 顶部固定工具栏的操作入口（工具栏已移出画布，见 NodeToolbar）。 */
        bool isSnapOn() const noexcept { return snapOn_; }
        void toggleSnap()
        {
            snapOn_ = ! snapOn_;
            if (onMessage) onMessage (snapOn_
                ? mb::tr ("网格吸附已开启", "Grid snap on")
                : mb::tr ("网格吸附已关闭", "Grid snap off"));
            if (onSnapChanged) onSnapChanged();
            repaint();
        }
        void resetNodeLayout()
        {
            chOff_.clear(); inOff_.clear(); outOff_.clear();
            updateSize(); repaint();
            if (onMessage) onMessage (mb::tr ("已重置节点布局", "Node layout reset"));
        }

        NodeCanvas (mb::RouteGraph& graph, mb::AudioEngine& engine)
            : graph_ (graph), engine_ (engine), catalog_ (engine.catalog())
        {
            setOpaque (true);
            setWantsKeyboardFocus (true);   // Esc 取消「已选中的起点」
        }

        /** MainComponent::resized() 注入主区宽度：窗口随通道数变宽时画布同步铺满，
            不再在输出端口列右侧留下大片空白。 */
        void setAvailableWidth (int w)
        {
            availableWidth_ = juce::jmax (0, w);
            updateSize();
        }

        /** 内容尺寸随端口 / 通道数量、以及主区宽度变化。 */
        void updateSize()
        {
            computeMetrics();
            computeLayout();
            setSize (metrics_.canvasW, juce::jmax (300, layoutBottom_ + 10));
        }

        void paint (juce::Graphics& g) override
        {
            computeLayout();

            g.fillAll (mb::ui::background);

            // 三列表头；中列额外标出「左入 / 右出」，配合通道卡片两侧的连接点
            g.setFont (mb::ui::font (10.0f, true));
            g.setColour (mb::ui::subText);
            g.drawText (mb::tr ("输入端口", "Inputs"),
                        juce::Rectangle<int> (metrics_.inX, kToolbarH + 10, metrics_.inW, 16),
                        juce::Justification::centredLeft, false);

            g.setFont (mb::ui::font (8.0f));
            g.setColour (mb::ui::faintText);
            g.drawText (mb::tr ("入", "IN"),
                        juce::Rectangle<int> (metrics_.chX, kToolbarH + 10, 26, 16),
                        juce::Justification::centredLeft, false);
            g.setFont (mb::ui::font (10.0f, true));
            g.setColour (mb::ui::subText);
            g.drawText (mb::tr ("通道", "Channels"),
                        juce::Rectangle<int> (metrics_.chX + 26, kToolbarH + 10, metrics_.chW - 52, 16),
                        juce::Justification::centred, false);
            g.setFont (mb::ui::font (8.0f));
            g.setColour (mb::ui::faintText);
            g.drawText (mb::tr ("出", "OUT"),
                        juce::Rectangle<int> (metrics_.chX + metrics_.chW - 26, kToolbarH + 10, 26, 16),
                        juce::Justification::centredRight, false);

            g.setFont (mb::ui::font (10.0f, true));
            g.setColour (mb::ui::subText);
            g.drawText (mb::tr ("输出端口", "Outputs"),
                        juce::Rectangle<int> (metrics_.outX, kToolbarH + 10, metrics_.outW, 16),
                        juce::Justification::centredRight, false);

            // 顶部工具栏已移出画布，改由 MainComponent 的 NodeToolbar 覆盖层固定显示
            drawPorts (g, true);
            drawPorts (g, false);
            drawPlugChains (g);                 // 插件链：相邻插件的输入/输出点自动连线（表达顺序）
            drawEdges (g);                      // 连接线先画
            drawChannels (g);                   // 通道卡片盖在连线上（与插件卡片一致）
            drawPlugCards (g);                  // 插件卡片盖在连线上，避免连线穿过卡片文字
            drawPending (g);
            drawPlugDragGhost (g);
            drawHoverLabel (g);
        }

        //======================================================================
        // 鼠标：连接点拖拽 / 点选建线；连线点击移除
        //======================================================================
        void mouseDown (const juce::MouseEvent& ev) override
        {
            grabKeyboardFocus();                 // 让 Esc「取消选中」可用
            computeLayout();
            const auto p = ev.position;

            // —— 右键菜单：空白 / 通道卡片 / 插件卡片 各不同 ——
            if (ev.mods.isRightButtonDown())
            {
                if (const auto* pc = pluginCardAt (p))       showPluginContextMenu (*pc, p);
                else if (channelAt (p) >= 0)                  showChannelContextMenu (channelAt (p), p);
                else                                          showCanvasContextMenu (p);
                return;
            }

            if (const auto a = anchorAt (p))
            {
                pending_ = a;                            // 按下连接点：拖拽建线 / 原地松手弹菜单
                mousePos_ = p;
                repaint();
                return;
            }

            // —— 通道节点控件：三区正文（+输入 / +插件 / +输出）/ 制式徽标 ——
            // （放在 edgeAt 之前：卡片正文点击优先于穿越卡片的连线）
            {
                const int ch = channelAt (p);
                if (ch >= 0)
                {
                    if (monoRectFor (ch).contains (p))
                    {
                        const bool toStereo = ! mb::ui::isChannelStereo (ch);
                        mb::ui::setChannelStereo (ch, toStereo);
                        graph_.setChannelStereo (ch, toStereo);
                        if (onChanged) onChanged();
                        repaint();
                        return;
                    }
                }
            }

            // —— 卡片优先于连线：点在任意卡片上只操作卡片，不穿透命中其下方的连线 ——
            if (const auto* pc = pluginCardAt (p))   // 插件卡片本体 = 自由拖动改位置（落到通道 = 连接）
            {
                dragging_ = DragNode { DragNode::Plug, pc->ch, pc->idx };
                dragHome_ = { pc->rect.getX(), pc->rect.getY() };
                dragGrab_ = dragHome_ - p;
                const int bc = pc->ch < 0 ? mb::ChannelPluginBank::kFreePluginSlot : pc->ch;
                dragPlugUid_ = engine_.pluginBank().pluginUid (bc, pc->idx);
                return;
            }

            if (auto dn = nodeBodyAt (p))            // 卡片主体 = 拖动节点（自由画布）
            {
                dragging_ = dn;
                dragHome_ = nodeHomePos (*dn);
                dragGrab_ = dragHome_ - p;
                return;
            }

            if (overAnyCard (p)) return;             // 空槽等不可拖卡片：吞掉点击，仍不碰连线

            if (const auto edge = edgeAt (p))        // 仅空白处才命中连线 → 移除菜单
            {
                showRemoveMenu (*edge, p);
                pending_.reset();
                repaint();
                return;
            }
        }

        void mouseDrag (const juce::MouseEvent& ev) override
        {
            if (dragging_)
            {
                mousePos_ = ev.position;
                if (dragging_->kind == DragNode::Plug)
                {
                    // 插件卡片 = 真正的自由节点：拖动直接改自身位置
                    if (dragPlugUid_.isNotEmpty())
                        plugPos_[dragPlugUid_] = snapPoint (ev.position + dragGrab_);
                    updateSize();
                }
                else                                    // 通道 / 端口节点：相对「家」的偏移
                {
                    const auto target = snapPoint (ev.position + dragGrab_);
                    setNodeOffset (*dragging_, target - dragHome_);
                    updateSize();
                }
                repaint();
                return;
            }
            if (! pending_) return;
            mousePos_ = ev.position;
            repaint();
        }

        void mouseUp (const juce::MouseEvent& ev) override
        {
            if (dragging_)
            {
                const auto dn = *dragging_;
                dragging_.reset();
                if (dn.kind == DragNode::Plug)
                {
                    // 插件卡片落在某通道卡片上 = 建立归属连接；落空 = 仅保留新的自由位置
                    if (ev.getDistanceFromDragStart() > 8)
                    {
                        const int dst = channelAt (ev.position);
                        if (dst >= 0 && dst != dn.id && onMovePlugin)
                        {
                            const int src = dn.id < 0 ? mb::ChannelPluginBank::kFreePluginSlot : dn.id;
                            onMovePlugin (src, dn.plugIdx, dst, -1);
                            if (onMessage) onMessage (mb::tr ("已连接插件到 ", "Plugin connected to ")
                                + mb::channelDisplayName (dst));
                        }
                        if (onChanged) onChanged();        // 持久化新位置
                    }
                }
                else if (onChanged) onChanged();             // 持久化节点布局
                repaint();
                return;
            }
            if (! pending_) return;

            // 原地松手（位移 <8px）= 点击连接点
            if (ev.getDistanceFromDragStart() <= 8)
            {
                const auto a = *pending_;
                pending_.reset();
                if (a.kind == Anchor::PlugIn || a.kind == Anchor::PlugOut)
                    showPlugDotMenu (a, ev.position);
                else
                    showAnchorMenu (a, ev.position);
                repaint();
                return;
            }

            computeLayout();
            if (const auto target = anchorAt (ev.position))
                makeEdge (*pending_, *target);
            pending_.reset();                                // 拖拽结束一律收起（落空 = 取消）
            repaint();
        }

        /** 双击通道卡片头部 = 重命名（与调音台共用同一张名字表，两边实时同步）。 */
        void mouseDoubleClick (const juce::MouseEvent& ev) override
        {
            computeLayout();
            for (int c = 0; c < graph_.channelCount(); ++c)
                if (layout_.chCards[static_cast<size_t> (c)].contains (ev.position)
                    && ev.position.y < layout_.chCards[static_cast<size_t> (c)].getY() + kChHeadH
                    && mb::ui::slotState (c) != mb::ui::kSlotEmpty)
                {
                    beginRename (c);
                    return;
                }
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::escapeKey && pending_)
            {
                pending_.reset();
                repaint();
                return true;
            }
            if (key.getTextCharacter() == 'g' || key.getTextCharacter() == 'G')
            {
                toggleSnap();
                return true;
            }
            return false;
        }

        void mouseMove (const juce::MouseEvent& ev) override
        {
            lastMouse_ = ev.position;
            const auto a = anchorAt (ev.position);
            // 卡片盖在连线上：鼠标在卡片上时取消连线悬停（不显示连接名 / 手型）
            const int e = overAnyCard (ev.position) ? -1 : edgeIndexAt (ev.position);
            const int hover = (a ? 1 : 0) | (e >= 0 ? 2 : 0);
            if (hover != hoverFlag_ || e != hoverEdge_)
            {
                hoverFlag_ = hover;
                hoverEdge_ = e;
                repaint();
            }
            setMouseCursor (hover != 0 ? juce::MouseCursor::PointingHandCursor
                                       : juce::MouseCursor::NormalCursor);
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            hoverEdge_ = -1;
            hoverFlag_ = 0;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            repaint();
        }

    private:
        //======================================================================
        // 几何常量
        //======================================================================
        static constexpr int kMargin = 12;
        // 端口列再收 1/5（168→134 / 160→128），通道列收 1/4（160→120）；
        // 基准 = 12+134+108+120+90+128+12 = 604
        static constexpr int kBaseWidth = 604;
        static constexpr int kMinCanvasWidth = 540;     // 窄窗口下限（超出部分由水平滚动兜底）
        // 紧凑纵向排布只作用于端口列：同一设备的 L/R 声道行几乎贴合（仅 2px 缝），
        // 设备分组标题只占 13px（原值：端口 42/34、分组 18），多设备时不必翻页。
        // 通道列节点高度随插件数变化（chNodeH），并按左右端口列的总高度垂直居中。
        static constexpr int kToolbarH = 26;            // 顶部独立工具栏带高度
        static constexpr int kTop = kToolbarH + 28;     // 工具栏带 + 列头(10..26) 之后起始
        static constexpr int kPortPitch = 28, kPortH = 26;
        // 通道节点为紧凑三区卡片：头部（编号 + 名称 + 制式 + M/S）+ 三区正文
        // （左输入 / 中插件 / 右输出，不再列出插件行），连接点锚定头部中心。
        static constexpr int kChHeadH   = 28;           // 头部行高（名称 + 制式）= 通道卡片高度
        static constexpr int kChGap     = 19;           // 通道节点纵向间距 = 卡片高度的 2/3（28×2/3≈19）
        static constexpr int kBodyMinH  = 34;           // 三区正文高度（保底）
        static constexpr int kGroupH = 13;
        static constexpr float kDotR = 5.0f, kDotHit = 9.0f;
        static constexpr float kMonoOffset = 7.0f;      // 通道 L/R 小点相对锚点的上下偏移（下半区）
        // 插件卡片：贴在所属通道左侧，宽 86 / 高 24，纵向按插件序号堆叠
        static constexpr int kPlugW = 86, kPlugH = 24, kPlugGap = 4;

        /** 三列横向几何：由 computeMetrics() 按主区宽度现算；基准宽度下还原旧布局。 */
        struct Metrics
        {
            int inX = 12, inW = 210;
            int chX = 330, chW = 160;
            int outX = 560, outW = 200;
            int gapL = 108, gapR = 90;
            int canvasW = kBaseWidth;
            float laneL = 300.0f, laneR = 492.0f;      // C→C 绕行竖线（左 / 右留白内）
            float bendL = 54.0f,  bendR = 54.0f;       // P→C / C→P 控制点外扩量
        };

        Metrics metrics_;
        int availableWidth_ = 0;                        // 由 MainComponent::resized() 注入

        /** 宽窗口：三列卡片适度加宽，剩余宽度全部分给两段列间留白，
            输出列右侧始终只有 12px 边距；窄窗口：压缩到 620 再交给水平滚动。 */
        void computeMetrics()
        {
            // 预留 20px 给 Viewport 垂直滚动条 / 边框，避免画布比可视区宽几像素而冒出横条
            const int usable = juce::jmax (kMinCanvasWidth, availableWidth_ - 20);
            const float extra = static_cast<float> (usable - kBaseWidth);

            Metrics m;
            // 端口列较上一版再收 1/5（134 / 128），通道列收 1/4（120），
            // 腾出的宽度进入列间留白；宽窗口仍按比例同步放大。
            m.inW  = juce::jlimit (112, 336, juce::roundToInt (134.0f + extra * 0.16f));
            m.chW  = juce::jlimit (110, 300, juce::roundToInt (120.0f + extra * 0.12f));
            m.outW = juce::jlimit (104, 320, juce::roundToInt (128.0f + extra * 0.16f));
            const int gaps = juce::jmax (90, usable - (2 * kMargin + m.inW + m.chW + m.outW));
            m.gapL = juce::roundToInt (gaps * 0.545f);
            m.gapR = gaps - m.gapL;
            m.inX  = kMargin;
            m.chX  = m.inX + m.inW + m.gapL;
            m.outX = m.chX + m.chW + m.gapR;
            m.canvasW = m.outX + m.outW + kMargin;
            m.laneL = static_cast<float> (m.inX + m.inW) + static_cast<float> (m.gapL) * 0.72f;
            m.laneR = static_cast<float> (m.chX + m.chW) + static_cast<float> (m.gapR) * 0.28f;
            m.bendL = static_cast<float> (m.gapL) * 0.50f;
            m.bendR = static_cast<float> (m.gapR) * 0.60f;
            metrics_ = m;
        }

        //======================================================================
        // 连接点描述
        //======================================================================
        struct Anchor
        {
            enum Kind { InPort, OutPort, ChIn, ChOut, PlugIn, PlugOut } kind = InPort;
            int id = -1;                                // 端口序号 / 通道序号 / 插件所属通道
            juce::Point<float> pos;                     // 点中心（画布坐标）
            bool hidden = false;                        // 未勾选的辅助设备：不画、不可点
            /** 通道连接点角色：0 = 左单声道 L（上），1 = 立体声 L+R（中间大点），
                2 = 右单声道 R（下）。端口连接点恒为 1。放在末尾可让原有
                聚合初始化 { kind, id, pos } / { kind, id, pos, hidden } 保持不变。
                PlugIn / PlugOut：id = 通道号，plugIdx = 插件序号，role = 0(L)/2(R)。 */
            int role = 1;
            int plugIdx = -1;                           // PlugIn/PlugOut：插件在该通道链中的序号

            bool operator== (const Anchor& o) const noexcept
                { return kind == o.kind && id == o.id && role == o.role && plugIdx == o.plugIdx; }
        };

        static float dotRadius (const Anchor&) noexcept { return kDotR; }
        static float dotHitRadius (const Anchor&) noexcept { return kDotHit; }

        struct EdgeDraw                                  // 一条可命中连线的几何缓存
        {
            mb::RouteEdge edge;
            /** 折线采样：绘制 / 电平光点 / 命中测试共用同一份几何。
                C→C 是多段绕行曲线，无法用单段三次贝塞尔表达。 */
            std::vector<juce::Point<float>> pts;
            std::vector<float> alpha;                    // 与 pts 平行（保留字段兼容旧布局缓存）
            bool arrow = false;                          // C→C：终点画方向箭头
        };

        /** 一张插件卡片 = 通道插件链里的一条（投影，不是独立音源）。
            ch/idx 定位到 pluginBank().slots_[ch][idx]；左右点用于表达「顺序与归属」。 */
        struct PlugCard
        {
            int ch = -1, idx = -1;
            juce::Rectangle<float> rect;
            Anchor inL, inR, outL, outR;                // 左输入 L/R + 右输出 L/R
        };

        struct Layout
        {
            std::vector<Anchor> inDots, outDots;         // 下标 = 端口序号
            std::vector<Anchor> chInDots, chOutDots;     // 下标 = 通道序号：中间立体声大点
            std::vector<Anchor> chInL, chInR, chOutL, chOutR;   // 左右单声道小点
            std::vector<juce::Rectangle<float>> inCards, chCards, outCards;
            std::vector<PlugCard> plugCards;            // 插件卡片（扁平，按 ch→idx 顺序）
            int bottom = kTop;
        };

        mutable Layout layout_;
        mutable int layoutBottom_ = kTop;
        mutable std::vector<EdgeDraw> edgeDraws_;
        mutable int hoverFlag_ = 0;
        mutable int hoverEdge_ = -1;                     // 悬停连线下标（显示 CHa → CHb 提示）

        std::optional<Anchor> pending_;                  // 按下的连接点（拖拽建线 / 松手弹菜单）
        juce::Point<float> mousePos_;

        // 自由画布：节点位置覆盖（相对三列「家」布局的偏移；拖动后生效）
        mutable         std::vector<juce::Point<float>> chOff_, inOff_, outOff_;
        // 插件卡片位置按 UID 存（序号会因增删 / 连接而变），绝对画布坐标 = 卡片左上角
        std::map<juce::String, juce::Point<float>> plugPos_;
        mutable juce::String dragPlugUid_;
        bool snapOn_ = false;                           // 网格吸附（顶部工具栏 / 'G' 切换）；默认关闭，自由摆放
        float grid_  = 20.0f;                           // 吸附网格尺寸
        struct DragNode { enum Kind { Ch, In, Out, Plug } kind; int id; int plugIdx = -1; };
        std::optional<DragNode> dragging_;
        juce::Point<float> dragGrab_;                    // 鼠标相对节点原点的抓取偏移
        juce::Point<float> dragHome_;                    // 拖动起点时节点的「家」坐标（零偏移）


        std::unique_ptr<juce::TextEditor> nameEditor_;   // 通道重命名行内编辑器
        bool renaming_ = false;
        int renamingChannel_ = -1;

        mb::RouteGraph& graph_;
        mb::AudioEngine& engine_;
        const mb::DeviceCatalog& catalog_;

        //======================================================================
        /** 通道节点高度 = 仅头部（编号 + 名称 + 制式 + M/S）。
            已移除下方「+输入 / +插件 / +输出」三区正文；连接点锚定头部垂直中心。
            空槽同样占位保持对齐。 */
        int chNodeH (int) const
        {
            return kChHeadH;
        }

        //======================================================================
        void computeLayout() const
        {
            layout_ = Layout();
            chOff_.resize (static_cast<size_t> (graph_.channelCount()), { 0.0f, 0.0f });
            inOff_.resize (catalog_.inputPorts().size(), { 0.0f, 0.0f });
            outOff_.resize (catalog_.outputPorts().size(), { 0.0f, 0.0f });
            const auto& ins = catalog_.inputPorts();

            // 端口下标必须与目录一致（连线按端口序号存取）；
            // 未勾选的辅助设备不占位置，但已有连接的端口仍保留显示。
            std::vector<bool> inUsed (ins.size(), false), outUsedAt (catalog_.outputPorts().size(), false);
            for (const auto& e : graph_.edges())
            {
                if (e.kind == mb::EdgeKind::InputToChannel
                    && e.src >= 0 && e.src < static_cast<int> (inUsed.size()))
                    inUsed[static_cast<size_t> (e.src)] = true;
                if (e.kind == mb::EdgeKind::ChannelToOutput
                    && e.dst >= 0 && e.dst < static_cast<int> (outUsedAt.size()))
                    outUsedAt[static_cast<size_t> (e.dst)] = true;
            }

            /** 预估单侧端口列的内容高度（不含 kTop 起点），规则与下面两列循环
                完全一致（隐藏端口不占高度、每个设备分组加一条标题行）。 */
            auto portColumnHeight = [this, &inUsed, &outUsedAt] (bool input) -> float
            {
                const auto& ports = input ? catalog_.inputPorts() : catalog_.outputPorts();
                const auto& used  = input ? inUsed : outUsedAt;
                float h = 0.0f;
                juce::String last;
                for (size_t i = 0; i < ports.size(); ++i)
                {
                    if (i < used.size() && ! used[i]
                        && ! catalog_.portVisible (input, static_cast<int> (i)))
                        continue;
                    if (ports[i].deviceName != last)
                    {
                        last = ports[i].deviceName;
                        h += static_cast<float> (kGroupH);
                    }
                    h += static_cast<float> (kPortPitch);
                }
                return h;
            };
            const float inH = portColumnHeight (true);
            const float outH = portColumnHeight (false);
            float chH = 0.0f;
            for (int c = 0; c < graph_.channelCount(); ++c)
                chH += static_cast<float> (chNodeH (c) + kChGap);
            if (graph_.channelCount() > 0) chH -= static_cast<float> (kChGap);

            /** 两组互相垂直居中：「输入 + 输出端口」作为一个整体（取两列较高者，
                保证左右顶部对齐），「通道」作为另一组；以较高的一组为内容区，
                较矮的一组居中。端口多时通道不贴顶，通道多时端口不贴顶，
                底部元素到对侧的距离都约缩短一半。 */
            const float portH    = juce::jmax (inH, outH);
            const float contentH = juce::jmax (portH, chH);
            auto columnTop = [contentH] (float h) -> float
            {
                return static_cast<float> (kTop)
                     + juce::jmax (0.0f, (contentH - h) * 0.5f);
            };

            float y = columnTop (portH);
            juce::String lastDevice;
            for (size_t i = 0; i < ins.size(); ++i)
            {
                if (! inUsed[i] && ! catalog_.portVisible (true, static_cast<int> (i)))
                {
                    // 占位：保持 inDots/inCards 下标 = 端口序号
                    layout_.inDots.push_back ({ Anchor::InPort, static_cast<int> (i),
                                                { -9999.0f, -9999.0f }, true });
                    layout_.inCards.push_back ({});
                    continue;
                }
                if (ins[i].deviceName != lastDevice)     // 按设备分组的小标题
                {
                    lastDevice = ins[i].deviceName;
                    y += static_cast<float> (kGroupH);
                }
                const float ox = inOff_[i].x, oy = inOff_[i].y;
                const auto cy = y + kPortH * 0.5f + oy;
                const float cardX = static_cast<float> (metrics_.inX) + ox;
                layout_.inCards.push_back ({ cardX, y + oy,
                                             static_cast<float> (metrics_.inW), kPortH });
                layout_.inDots.push_back ({ Anchor::InPort, static_cast<int> (i),
                                            { cardX + static_cast<float> (metrics_.inW), cy } });
                y += static_cast<float> (kPortPitch);
            }
            layout_.bottom = juce::jmax (layout_.bottom, static_cast<int> (y));

            /** 一张插件卡：优先按 UID 取用户摆放的绝对坐标，未摆放过才用默认位置。
                ch = -1 表示自由插件（引擎侧存在 kFreePluginSlot）。 */
            auto makePlugCard = [this] (int ch, int idx, float defX, float defY) -> PlugCard
            {
                PlugCard pc;
                pc.ch = ch; pc.idx = idx;
                const int bc = ch < 0 ? mb::ChannelPluginBank::kFreePluginSlot : ch;
                const auto uid = engine_.pluginBank().pluginUid (bc, idx);
                const auto it = plugPos_.find (uid);
                const float px = it != plugPos_.end() ? it->second.x : defX;
                const float py = it != plugPos_.end() ? it->second.y : defY;
                pc.rect = { px, py, static_cast<float> (kPlugW), static_cast<float> (kPlugH) };
                const float iy = py + kPlugH * 0.32f, oy2 = py + kPlugH * 0.68f;
                pc.inL  = { Anchor::PlugIn,  ch, { px, iy  }, false, 0, idx };
                pc.inR  = { Anchor::PlugIn,  ch, { px, oy2 }, false, 2, idx };
                pc.outL = { Anchor::PlugOut, ch, { px + kPlugW, iy  }, false, 0, idx };
                pc.outR = { Anchor::PlugOut, ch, { px + kPlugW, oy2 }, false, 2, idx };
                return pc;
            };

            // 通道列固定贴顶：不再与端口列垂直居中。端口多时旧逻辑把通道整列
            // 下压 (contentH-chH)/2，顶部空出一大片且拖动上限又受 home 牵连够不到顶。
            y = static_cast<float> (kTop);
            for (int c = 0; c < graph_.channelCount(); ++c)
            {
                const float ox = chOff_[static_cast<size_t> (c)].x, oy = chOff_[static_cast<size_t> (c)].y;
                const float xIn  = static_cast<float> (metrics_.chX) + ox;
                const float xOut = xIn + static_cast<float> (metrics_.chW);
                // 连接点锚定头部垂直中心，L/R 小点以其为中心 ±7px
                const auto cy = y + static_cast<float> (chNodeH (c)) * 0.5f + oy;
                // 空槽不画点：同时标记为 hidden，避免留下看不见却能命中的连接点
                const bool skip = mb::ui::slotState (c) == mb::ui::kSlotEmpty;
                layout_.chCards.push_back ({ xIn, y + oy, static_cast<float> (metrics_.chW),
                                             static_cast<float> (chNodeH (c)) });
                layout_.chInDots.push_back  ({ Anchor::ChIn,  c, { xIn,  cy }, skip });   // 中点锚（不绘制/不命中）
                layout_.chOutDots.push_back ({ Anchor::ChOut, c, { xOut, cy }, skip });
                // 声道连接点：立体声 = 上 L / 下 R 两个小点（锚在正文下半区）；
                // 单声道 = 单端口（L 点置于锚点，R 点隐藏不绘制/不命中）
                if (mb::ui::isChannelStereo (c))
                {
                    layout_.chInL.push_back  ({ Anchor::ChIn,  c, { xIn,  cy - kMonoOffset }, skip, 0 });
                    layout_.chInR.push_back  ({ Anchor::ChIn,  c, { xIn,  cy + kMonoOffset }, skip, 2 });
                    layout_.chOutL.push_back ({ Anchor::ChOut, c, { xOut, cy - kMonoOffset }, skip, 0 });
                    layout_.chOutR.push_back ({ Anchor::ChOut, c, { xOut, cy + kMonoOffset }, skip, 2 });
                }
                else
                {
                    layout_.chInL.push_back  ({ Anchor::ChIn,  c, { xIn,  cy }, skip, 0 });
                    layout_.chInR.push_back  ({ Anchor::ChIn,  c, { -9999.0f, -9999.0f }, true, 2 });
                    layout_.chOutL.push_back ({ Anchor::ChOut, c, { xOut, cy }, skip, 0 });
                    layout_.chOutR.push_back ({ Anchor::ChOut, c, { -9999.0f, -9999.0f }, true, 2 });
                }
                // 插件卡片：贴在所属通道左侧留白内，按插件序号纵向堆叠。
                // 纯投影：数据来自 pluginBank；左右 L/R 小点表达「顺序（插件→插件）」与
                // 「归属（插件→通道）」，不连设备端口。
                if (! skip)
                {
                    const int n = engine_.pluginBank().pluginCount (c);
                    for (int i = 0; i < n; ++i)
                        layout_.plugCards.push_back (makePlugCard (
                            c, i, xIn - static_cast<float> (kPlugW) - 16.0f,
                            y + oy + static_cast<float> (i * (kPlugH + kPlugGap))));
                }
                y += static_cast<float> (chNodeH (c) + kChGap);
            }

            // 自由插件（未连接任何通道）：默认放在输入列右侧留白里，
            // 实际位置由 setPluginPosition() / 拖动写入的 plugPos_ 决定。
            {
                constexpr int fs = mb::ChannelPluginBank::kFreePluginSlot;
                const int n = engine_.pluginBank().pluginCount (fs);
                for (int i = 0; i < n; ++i)
                {
                    const float dx = static_cast<float> (metrics_.inX + 18
                                       + (i % 3) * (kPlugW + kPlugGap + 8));
                    const float dy = static_cast<float> (kTop + 16
                                       + (i / 3) * (kPlugH + kPlugGap + 8));
                    layout_.plugCards.push_back (makePlugCard (-1, i, dx, dy));
                }
            }
            layout_.bottom = juce::jmax (layout_.bottom, static_cast<int> (y));

            y = columnTop (portH);                    // 与输入列共用基准，左右顶部对齐
            const auto& outs = catalog_.outputPorts();
            juce::String lastOut;
            for (size_t i = 0; i < outs.size(); ++i)
            {
                if (! outUsedAt[i] && ! catalog_.portVisible (false, static_cast<int> (i)))
                {
                    layout_.outDots.push_back ({ Anchor::OutPort, static_cast<int> (i),
                                                 { -9999.0f, -9999.0f }, true });
                    layout_.outCards.push_back ({});
                    continue;
                }
                if (outs[i].deviceName != lastOut)
                {
                    lastOut = outs[i].deviceName;
                    y += static_cast<float> (kGroupH);
                }
                const float ox = outOff_[i].x, oy = outOff_[i].y;
                const auto cy = y + kPortH * 0.5f + oy;
                const float cardX = static_cast<float> (metrics_.outX) + ox;
                layout_.outCards.push_back ({ cardX, y + oy,
                                              static_cast<float> (metrics_.outW), kPortH });
                layout_.outDots.push_back ({ Anchor::OutPort, static_cast<int> (i),
                                             { cardX, cy } });
                y += static_cast<float> (kPortPitch);
            }
            layout_.bottom = juce::jmax (layout_.bottom, static_cast<int> (y));
            // 自由画布：画布高度按所有节点实际（含偏移）底边取最大值
            {
                int maxBottom = kTop;
                auto scanBottom = [&] (const std::vector<juce::Rectangle<float>>& cards)
                {
                    for (const auto& c : cards)
                        if (! c.isEmpty())
                            maxBottom = juce::jmax (maxBottom, juce::roundToInt (c.getBottom()));
                };
                scanBottom (layout_.inCards);
                scanBottom (layout_.chCards);
                scanBottom (layout_.outCards);
                for (const auto& pc : layout_.plugCards)
                    maxBottom = juce::jmax (maxBottom, juce::roundToInt (pc.rect.getBottom()));
                layout_.bottom = maxBottom;
            }
            layoutBottom_ = layout_.bottom;

            // 立体声成对的两条边分别落到 L / R 小点，其余边汇聚到中点锚（L/R 之间）。
            // 判定与 ChannelStrip::meterPorts 一致：同设备 channelIndex 0 + 1。
            const int chCount = graph_.channelCount();
            std::vector<std::vector<int>> inPortsByCh (static_cast<size_t> (chCount)),
                                          outPortsByCh (static_cast<size_t> (chCount));
            for (const auto& ge : graph_.edges())
            {
                if (ge.kind == mb::EdgeKind::InputToChannel
                    && ge.dst >= 0 && ge.dst < chCount)
                    inPortsByCh[static_cast<size_t> (ge.dst)].push_back (ge.src);
                if (ge.kind == mb::EdgeKind::ChannelToOutput
                    && ge.src >= 0 && ge.src < chCount)
                    outPortsByCh[static_cast<size_t> (ge.src)].push_back (ge.dst);
            }
            auto hasStereoPair = [this] (bool input, const std::vector<int>& ids) -> bool
            {
                const auto& ports = input ? catalog_.inputPorts() : catalog_.outputPorts();
                for (int a : ids)
                    for (int b : ids)
                    {
                        if (a == b || a < 0 || b < 0
                            || a >= static_cast<int> (ports.size())
                            || b >= static_cast<int> (ports.size())) continue;
                        const auto& pa = ports[static_cast<size_t> (a)];
                        const auto& pb = ports[static_cast<size_t> (b)];
                        if (pa.deviceIndex == pb.deviceIndex
                            && ((pa.channelIndex == 0 && pb.channelIndex == 1)
                             || (pa.channelIndex == 1 && pb.channelIndex == 0)))
                            return true;
                    }
                return false;
            };
            std::vector<std::uint8_t> inStereo (static_cast<size_t> (chCount), 0),
                                      outStereo (static_cast<size_t> (chCount), 0);
            for (int c = 0; c < chCount; ++c)
            {
                inStereo[static_cast<size_t> (c)]  = hasStereoPair (true,  inPortsByCh[static_cast<size_t> (c)])  ? 1 : 0;
                outStereo[static_cast<size_t> (c)] = hasStereoPair (false, outPortsByCh[static_cast<size_t> (c)]) ? 1 : 0;
            }
            auto inDotFor = [this, &inStereo] (int ch, int port) -> const Anchor&
            {
                if (! mb::ui::isChannelStereo (ch))
                    return layout_.chInL[static_cast<size_t> (ch)];  // 单声道单端口（位于中心）
                if (inStereo[static_cast<size_t> (ch)] != 0)
                {
                    const auto& ports = catalog_.inputPorts();
                    const int ci = (port >= 0 && port < static_cast<int> (ports.size()))
                                 ? ports[static_cast<size_t> (port)].channelIndex : -1;
                    if (ci == 0) return layout_.chInL[static_cast<size_t> (ch)];
                    if (ci == 1) return layout_.chInR[static_cast<size_t> (ch)];
                }
                return layout_.chInDots[static_cast<size_t> (ch)];
            };
            auto outDotFor = [this, &outStereo] (int ch, int port) -> const Anchor&
            {
                if (! mb::ui::isChannelStereo (ch))
                    return layout_.chOutL[static_cast<size_t> (ch)]; // 单声道单端口（位于中心）
                if (outStereo[static_cast<size_t> (ch)] != 0)
                {
                    const auto& ports = catalog_.outputPorts();
                    const int ci = (port >= 0 && port < static_cast<int> (ports.size()))
                                 ? ports[static_cast<size_t> (port)].channelIndex : -1;
                    if (ci == 0) return layout_.chOutL[static_cast<size_t> (ch)];
                    if (ci == 1) return layout_.chOutR[static_cast<size_t> (ch)];
                }
                return layout_.chOutDots[static_cast<size_t> (ch)];
            };

            // 连线几何缓存（同 paint 顺序，供绘制 / 光点 / 命中测试共用）
            edgeDraws_.clear();
            int ccIndex = 0;
            for (const auto& e : graph_.edges())
            {
                EdgeDraw d;
                d.edge = e;
                switch (e.kind)
                {
                    case mb::EdgeKind::InputToChannel:
                    {
                        if (e.src >= (int) layout_.inDots.size()
                            || e.dst >= (int) layout_.chInDots.size()) continue;
                        const auto a = layout_.inDots[static_cast<size_t> (e.src)].pos;
                        const auto b = inDotFor (e.dst, e.src).pos;
                        addCubic (d.pts, a, { a.x + metrics_.bendL, a.y },
                                  { b.x - metrics_.bendL, b.y }, b, 24);
                        break;
                    }

                    case mb::EdgeKind::ChannelToOutput:
                    {
                        if (e.src >= (int) layout_.chOutDots.size()
                            || e.dst >= (int) layout_.outDots.size()) continue;
                        const auto a = outDotFor (e.src, e.dst).pos;
                        const auto b = layout_.outDots[static_cast<size_t> (e.dst)].pos;
                        addCubic (d.pts, a, { a.x + metrics_.bendR, a.y },
                                  { b.x - metrics_.bendR, b.y }, b, 24);
                        break;
                    }

                    case mb::EdgeKind::ChannelToChannel:
                    {
                        if (e.src >= (int) layout_.chOutDots.size()
                            || e.dst >= (int) layout_.chInDots.size()) continue;
                        const int idx = ccIndex++;
                        // 双声道呈现：side -1 且任一端立体声 → L / R 两条曲线（命中任一条
                        // 都指向同一条边）；side 0 / 1 → 仅 L / 仅 R 单条曲线；
                        // 双声道标记但两端都单声道 → 退回中点锚单条曲线。
                        const bool dual = e.side == -1
                            && (graph_.isStereo (e.src) || graph_.isStereo (e.dst));
                        // 单声道端 = 单端口（L 点已置于中心）：立体声→单声道两侧曲线
                        // 汇聚到一点；单声道→立体声由一点扇出到 L/R
                        const bool srcMono = ! graph_.isStereo (e.src);
                        const bool dstMono = ! graph_.isStereo (e.dst);
                        for (int s = 0; s < (dual ? 2 : 1); ++s)
                        {
                            const int use = dual ? s : e.side;   // -1 / 0 / 1
                            EdgeDraw d2;
                            d2.edge = e;
                            d2.arrow = true;
                            const auto a = (! srcMono && use == 1)
                                ? layout_.chOutR[static_cast<size_t> (e.src)].pos
                                : layout_.chOutL[static_cast<size_t> (e.src)].pos;
                            const auto b = (! dstMono && use == 1)
                                ? layout_.chInR[static_cast<size_t> (e.dst)].pos
                                : layout_.chInL[static_cast<size_t> (e.dst)].pos;
                            makeChannelLoop (d2, a, b, idx);
                            if (! d2.pts.empty()) edgeDraws_.push_back (std::move (d2));
                        }
                        break;
                    }
                }
                if (! d.pts.empty()) edgeDraws_.push_back (std::move (d));
            }

            // 连线全程保持原透明度（穿过通道卡片不再降透明）
            for (auto& d : edgeDraws_)
                d.alpha.assign (d.pts.size(), 1.0f);
        }

        /** 三次贝塞尔采样成折线；起点只在序列为空时写入，便于多段拼接。 */
        static void addCubic (std::vector<juce::Point<float>>& pts,
                              juce::Point<float> a, juce::Point<float> c1,
                              juce::Point<float> c2, juce::Point<float> b,
                              int steps)
        {
            if (pts.empty()) pts.push_back (a);
            for (int i = 1; i <= steps; ++i)
            {
                const float t = static_cast<float> (i) / static_cast<float> (steps);
                const float u = 1.0f - t;
                const float w0 = u * u * u, w1 = 3.0f * u * u * t,
                            w2 = 3.0f * u * t * t, w3 = t * t * t;
                pts.push_back ({ a.x * w0 + c1.x * w1 + c2.x * w2 + b.x * w3,
                                 a.y * w0 + c1.y * w1 + c2.y * w2 + b.y * w3 });
            }
        }

        /** C→C 连线（自由画布）：从源出点 → 目标入点的直接三次贝塞尔，控制点水平外扩，
            绕开卡片；不再依赖三列固定「留白车道」，节点任意摆放都能正确连接。 */
        void makeChannelLoop (EdgeDraw& d, juce::Point<float> a,
                              juce::Point<float> b, int idx) const
        {
            const float bulge = juce::jmin (140.0f, juce::jmax (40.0f,
                                       std::fabs (b.x - a.x) * 0.4f));
            const float jitter = static_cast<float> ((idx % 3) - 1) * 6.0f;  // 多条线纵向错开
            addCubic (d.pts, a, { a.x + bulge, a.y + jitter },
                      { b.x - bulge, b.y + jitter }, b, 28);
        }

        //======================================================================
        void drawPorts (juce::Graphics& g, bool input)
        {
            const auto& ports = input ? catalog_.inputPorts() : catalog_.outputPorts();
            const auto& cards = input ? layout_.inCards : layout_.outCards;
            const auto& dots  = input ? layout_.inDots  : layout_.outDots;

            juce::String lastDevice;
            for (size_t i = 0; i < ports.size(); ++i)
            {
                if (i >= cards.size() || cards[i].isEmpty()) continue;   // 未勾选的辅助设备
                if (ports[i].deviceName != lastDevice)       // 按设备分组的小标题
                {
                    lastDevice = ports[i].deviceName;       // 分组比较仍用原始名
                    // 标题显示「备注名在左、设备名在右」；长名缩放而不省略。
                    const auto label = ports[i].deviceLabel.isNotEmpty()
                                     ? ports[i].deviceLabel : ports[i].deviceName;
                    g.setFont (mb::ui::font (8.0f, true));
                    g.setColour (mb::ui::faintText);
                    g.drawFittedText (label,
                                juce::Rectangle<int> (input ? metrics_.inX : metrics_.outX,
                                                      static_cast<int> (cards[i].getY()) - kGroupH,
                                                      input ? metrics_.inW : metrics_.outW, kGroupH),
                                juce::Justification::centredLeft, 1, 0.65f);
                }

                const auto card = cards[i];
                g.setColour (mb::ui::card.withAlpha (0.72f));
                g.fillRoundedRectangle (card, 5.0f);
                g.setColour (mb::ui::border.withAlpha (0.6f));
                g.drawRoundedRectangle (card, 5.0f, 0.7f);

                // 卡片文字用 portLabel：通道备注优先、其次设备备注，
                // 格式「备注 (设备 · 通道)」；长名缩放而不省略。
                g.setFont (mb::ui::font (9.0f));
                g.setColour (mb::ui::text);
                g.drawFittedText (catalog_.portLabel (input, static_cast<int> (i)),
                                  juce::Rectangle<float> (card.getX() + 9.0f, card.getY(),
                                                          card.getWidth() - (input ? 26.0f : 18.0f),
                                                          card.getHeight()).toNearestInt(),
                                  juce::Justification::centredLeft, 1, 0.55f);

                // 连接点：悬停 / 已选中时放大加亮
                const auto& dot = dots[i];
                const bool hot = isHot (dot);
                const float r = kDotR + (hot ? 1.8f : 0.0f);
                g.setColour (hot ? mb::ui::accent[0] : mb::ui::subText.withAlpha (0.8f));
                g.fillEllipse (dot.pos.x - r, dot.pos.y - r, r * 2.0f, r * 2.0f);
                g.setColour (mb::ui::background);
                g.fillEllipse (dot.pos.x - r * 0.4f, dot.pos.y - r * 0.4f, r * 0.8f, r * 0.8f);
            }
        }

        void drawChannels (juce::Graphics& g)
        {
            // 预设体验 5b：设备不匹配被丢弃路由涉及的通道 → 红色悬空角标
            const auto dropped = graph_.droppedChannels();
            for (int c = 0; c < graph_.channelCount(); ++c)
            {
                const auto slot = mb::ui::slotState (c);
                const bool empty = slot == mb::ui::kSlotEmpty;
                const bool hidden = slot == mb::ui::kSlotHidden;
                auto card = layout_.chCards[static_cast<size_t> (c)];
                const auto colour = mb::ui::stripColour (c);
                const float alpha = hidden ? 0.45f : 1.0f;

                if (empty)
                {
                    // 空槽：虚线占位，不参与连线
                    g.setColour (mb::ui::border.withAlpha (0.4f));
                    g.drawRoundedRectangle (card, 5.0f, 0.8f);
                    g.setFont (mb::ui::font (8.0f));
                    g.setColour (mb::ui::faintText);
                    g.drawText (mb::tr ("空槽", "Empty"), card, juce::Justification::centred, false);
                    continue;
                }

                // 卡片主体：仅头部（编号 + 名称 + 制式）；已移除下方三区正文
                g.setColour (mb::ui::card.withAlpha (0.85f * alpha));
                g.fillRoundedRectangle (card, 5.0f);
                g.setColour (colour.withAlpha (0.85f * alpha));
                g.drawRoundedRectangle (card, 5.0f, 1.0f);
                const auto head = card;

                g.setFont (mb::ui::font (8.6f, true));
                g.setColour (mb::ui::faintText.withAlpha (alpha));
                g.drawText (mb::S("CH") + juce::String (c + 1),
                            head.getX() + 6.0f, head.getY(), 26.0f, head.getHeight(),
                            juce::Justification::centredLeft, false);
                g.setFont (mb::ui::font (9.0f));
                g.setColour (mb::ui::text.withAlpha (alpha));
                g.drawText (mb::channelDisplayName (c),
                            head.getX() + 30.0f, head.getY(),
                            juce::jmax (12.0f, head.getWidth() - 76.0f), head.getHeight(),
                            juce::Justification::centredLeft, true);

                // 通道链接角标：头部 M/S 左侧（如 A1）；制式徽标恒占右端，链接码左移
                {
                    const auto code = mb::links::codeString (mb::links::primaryOf (c));
                    if (code.isNotEmpty())
                    {
                        const float codeX = -52.0f;
                        g.setFont (mb::ui::font (7.0f, true));
                        g.setColour (colour.withAlpha (alpha));
                        g.drawText (code, head.getRight() + codeX, head.getY(),
                                    14.0f, head.getHeight(),
                                    juce::Justification::centredRight, false);
                    }
                }

                // 声道制式徽标（与调音台头部开关同态）：
                // L+R（通道色）= 立体声；L（灰）= 单声道。点击切换。
                {
                    const bool st = mb::ui::isChannelStereo (c);
                    g.setFont (mb::ui::font (6.8f, true));
                    g.setColour (colour.withAlpha (alpha));   // 双态同色：仅 L+R / L 字形区分
                    g.drawText (st ? mb::S("L+R") : mb::S("L"),
                                monoRectFor (c).toNearestInt(),
                                juce::Justification::centred, false);
                }

                // 悬空角标：设备不匹配导致本通道部分路由未恢复（红色 ! ）
                if (std::find (dropped.begin(), dropped.end(), c) != dropped.end())
                {
                    const auto badge = juce::Rectangle<float> (
                        card.getRight() - 10.0f, card.getY() - 5.0f, 12.0f, 12.0f);
                    g.setColour (mb::ui::meterRed);
                    g.fillEllipse (badge);
                    g.setFont (mb::ui::font (8.5f, true));
                    g.setColour (juce::Colours::white);
                    g.drawText (mb::S("!"), badge.toNearestInt(),
                                juce::Justification::centred, false);
                }

                const auto ci = static_cast<size_t> (c);
                if (mb::ui::isChannelStereo (c))
                {
                    // 每侧两个连接点：L 小点（上）/ R 小点（下）；立体声 = 成对补线
                    drawChannelDot (g, layout_.chInL[ci],  colour, true);
                    drawChannelDot (g, layout_.chInR[ci],  colour, true);
                    drawChannelDot (g, layout_.chOutL[ci], colour, false);
                    drawChannelDot (g, layout_.chOutR[ci], colour, false);
                }
                else
                {
                    // 单声道：每侧单端口（位于正文下半区锚点），不画 L/R 标注
                    drawChannelDot (g, layout_.chInL[ci],  colour, true,  false);
                    drawChannelDot (g, layout_.chOutL[ci], colour, false, false);
                }
            }
        }

        /** 插件链自动连线：同一通道相邻插件「上一个的输出 → 下一个的输入」，
            以及末个插件输出 → 通道左侧输入点。仅表达数组顺序（投影），不是独立路由边；
            颜色取所属通道色，圆点端画小箭头表意。 */
        void drawPlugChains (juce::Graphics& g)
        {
            const float phase = static_cast<float> (
                juce::Time::getMillisecondCounter() % 1600) / 1600.0f;
            const auto& pcs = layout_.plugCards;
            size_t i = 0;
            while (i < pcs.size())
            {
                const int ch = pcs[i].ch;
                size_t j = i;
                while (j < pcs.size() && pcs[j].ch == ch) ++j;   // [i, j) = 同一通道的插件链
                const auto colour = mb::ui::stripColour (juce::jmax (0, ch));
                // 链内流动光点随所在通道电平点亮（播放器 / 麦克风等任何链内音源）；
                // 自由停放插件（ch < 0）没有路由语义，保持静态线不点亮。
                const float level = ch >= 0
                    ? mb::ui::levelToMeter (engine_.channelPeak (ch)) : 0.0f;

                for (size_t k = i; k < j; ++k)
                {
                    const auto& cur = pcs[k];
                    // 相邻插件：cur.outL/inR → next.inL/inR（L 对 L、R 对 R）
                    if (k + 1 < j)
                    {
                        const auto& nxt = pcs[k + 1];
                        drawPlugLink (g, cur.outL.pos, nxt.inL.pos, colour, level, phase);
                        drawPlugLink (g, cur.outR.pos, nxt.inR.pos, colour, level, phase);
                    }
                    else if (ch >= 0 && ! layout_.chInL.empty())
                    {
                        // 末插件 → 通道左输入（L→L / R→R），落入通道卡片左缘
                        const auto& dstL = layout_.chInL[static_cast<size_t> (ch)];
                        const auto& dstR = layout_.chInR[static_cast<size_t> (ch)];
                        drawPlugLink (g, cur.outL.pos, dstL.pos, colour, level, phase);
                        if (! dstR.hidden) drawPlugLink (g, cur.outR.pos, dstR.pos, colour, level, phase);
                    }
                }
                i = j;
            }
        }

        /** 插件链连线：贝塞尔静态线 + 沿曲线流动的电平光点（与主连线同款，
            无声时只剩静态线，不画呼吸点——链线较细，避免常亮噪音）。 */
        void drawPlugLink (juce::Graphics& g, juce::Point<float> a,
                           juce::Point<float> b, juce::Colour colour,
                           float level = 0.0f, float phase = 0.0f)
        {
            const float bend = juce::jmax (12.0f, (b.x - a.x) * 0.5f);
            const auto c1 = juce::Point<float> { a.x + bend, a.y };
            const auto c2 = juce::Point<float> { b.x - bend, b.y };
            juce::Path p;
            p.startNewSubPath (a);
            p.cubicTo (c1, c2, b);
            g.setColour (colour.withAlpha (0.55f));
            g.strokePath (p, juce::PathStrokeType (1.5f));

            if (level <= 0.0f) return;
            const float t = juce::jlimit (0.0f, 1.0f, phase * (0.75f + 0.5f * level));
            const float u = 1.0f - t;
            const auto pos = u * u * u * a + 3.0f * u * u * t * c1
                           + 3.0f * u * t * t * c2 + t * t * t * b;
            const float r = 2.0f + 1.2f * level;
            g.setColour (juce::Colours::white.withAlpha (0.25f + 0.7f * level));
            g.fillEllipse (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f);
        }

        /** 插件卡片：小圆角卡 + 名称/备注文字 + 左侧 L/R 输入点 + 右侧 L/R 输出点。 */
        void drawPlugCards (juce::Graphics& g)
        {
            for (const auto& pc : layout_.plugCards)
            {
                const int bc = pc.ch < 0 ? mb::ChannelPluginBank::kFreePluginSlot : pc.ch;
                const auto& bank = engine_.pluginBank();
                const auto colour = mb::ui::stripColour (juce::jmax (0, pc.ch));
                const bool bypass = bank.pluginBypass (bc, pc.idx);
                const float alpha = bypass ? 0.5f : 1.0f;
                const auto note = bank.pluginNote (bc, pc.idx);
                const auto name = bank.pluginName (bc, pc.idx);
                const auto label = (pc.ch < 0 ? mb::S("◇ ") : juce::String())
                                 + (note.isNotEmpty() ? note + mb::S(" (") + name + mb::S(")") : name);

                g.setColour (mb::ui::card.withAlpha (0.92f * alpha));
                g.fillRoundedRectangle (pc.rect, 4.0f);
                g.setColour (colour.withAlpha (0.9f * alpha));
                g.drawRoundedRectangle (pc.rect, 4.0f, 1.0f);

                g.setFont (mb::ui::font (8.0f));
                g.setColour (mb::ui::text.withAlpha (alpha));
                g.drawFittedText (label,
                                  juce::Rectangle<int> (pc.rect.getX() + 10, pc.rect.getY(),
                                                        pc.rect.getWidth() - 20, pc.rect.getHeight()),
                                  juce::Justification::centred, 1, 0.6f);

                // 左输入（空心）/ 右输出（实心带芯），悬停放大
                auto drawDot = [&] (const Anchor& d, bool inSide)
                {
                    const bool hot = isHot (d);
                    const float r = kDotR * 0.8f + (hot ? 1.6f : 0.0f);
                    g.setColour ((hot ? juce::Colours::white : colour).withAlpha (0.95f * alpha));
                    g.fillEllipse (d.pos.x - r, d.pos.y - r, r * 2.0f, r * 2.0f);
                    if (inSide && ! hot)
                    {
                        g.setColour (mb::ui::background.withAlpha (0.9f));
                        g.fillEllipse (d.pos.x - r * 0.5f, d.pos.y - r * 0.5f, r, r);
                    }
                };
                drawDot (pc.inL, true);
                drawDot (pc.inR, true);
                drawDot (pc.outL, false);
                drawDot (pc.outR, false);
            }
        }

        /** 通道连接点：L（上小点）/ R（下小点）。
            左缘 = 接收输入（空心）；右缘 = 输出发送（实心带芯）。 */
        void drawChannelDot (juce::Graphics& g, const Anchor& dot,
                             juce::Colour colour, bool inSide, bool showLabel = true)
        {
            const bool hot = isHot (dot);
            const float r = dotRadius (dot) + (hot ? 1.6f : 0.0f);
            g.setColour ((hot ? juce::Colours::white : colour).withAlpha (0.95f));
            g.fillEllipse (dot.pos.x - r, dot.pos.y - r, r * 2.0f, r * 2.0f);

            if (! hot)
            {
                if (inSide)
                {
                    g.setColour (mb::ui::background.withAlpha (0.9f));
                    g.fillEllipse (dot.pos.x - r * 0.52f, dot.pos.y - r * 0.52f, r * 1.04f, r * 1.04f);
                }
                else
                {
                    g.setColour (juce::Colours::white.withAlpha (0.9f));
                    g.fillEllipse (dot.pos.x - r * 0.26f, dot.pos.y - r * 0.26f, r * 0.52f, r * 0.52f);
                }
            }
            // L / R 标注画在卡片外侧留白，不压卡片内文字（单声道单端口不标注）
            if (showLabel)
            {
                const auto label = dot.role == 0 ? mb::S("L") : mb::S("R");
                g.setFont (mb::ui::font (6.5f));
                g.setColour (mb::ui::faintText.withAlpha (hot ? 1.0f : 0.75f));
                const int lx = inSide ? juce::roundToInt (dot.pos.x) - 13
                                      : juce::roundToInt (dot.pos.x) + 3;
                g.drawText (label, lx, juce::roundToInt (dot.pos.y) - 5, 10, 10,
                            juce::Justification::centred, false);
            }
        }

        juce::Colour edgeColour (const mb::RouteEdge& e) const
        {
            // 颜色 = 「声音归属通道」的颜色：P→C 用目标通道，C→C / C→P 用源通道
            return mb::ui::stripColour (e.kind == mb::EdgeKind::InputToChannel ? e.dst : e.src);
        }

        void drawEdges (juce::Graphics& g)
        {
            const float phase = static_cast<float> (
                juce::Time::getMillisecondCounter() % 1600) / 1600.0f;

            for (const auto& d : edgeDraws_)
            {
                if (d.pts.empty()) continue;
                const auto colour = edgeColour (d.edge);
                const float width = d.arrow ? 2.0f : 1.8f;
                // 整条连线一次性描出（穿过卡片区段不再降透明，全程统一透明度）
                juce::Path run;
                run.startNewSubPath (d.pts.front());
                for (size_t i = 1; i < d.pts.size(); ++i)
                    run.lineTo (d.pts[i]);
                g.setColour (colour.withAlpha (d.arrow ? 0.88f : 0.75f));
                g.strokePath (run, juce::PathStrokeType (width,
                                   juce::PathStrokeType::curved,
                                   juce::PathStrokeType::rounded));
                if (d.arrow) drawArrow (g, colour, d.pts);

                // 电平流动光点：无声时只剩微弱呼吸点，有声时随电平加亮加速。
                // P→C 边只随「设备输入端口」的真实进线电平点亮——通道内
                // 播放器 / 互送等自发声源不会点亮设备进线；C→C / C→P 随源通道。
                const float level = mb::ui::levelToMeter (
                    d.edge.kind == mb::EdgeKind::InputToChannel
                        ? engine_.inputPortPeak (d.edge.src)
                        : engine_.channelPeak (d.edge.src));
                const float t = juce::jlimit (0.0f, 1.0f, phase * (0.75f + 0.5f * level));
                const auto pos = d.pts[static_cast<size_t> (
                    juce::roundToInt (t * static_cast<float> (d.pts.size() - 1)))];
                const float r = 2.6f + 1.6f * level;
                g.setColour (juce::Colours::white.withAlpha (0.25f + 0.7f * level));
                g.fillEllipse (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f);
            }
        }

        /** C→C 终点箭头：明确「落到接收输入点」的方向。 */
        static void drawArrow (juce::Graphics& g, juce::Colour colour,
                               const std::vector<juce::Point<float>>& pts)
        {
            if (pts.size() < 5) return;
            const auto end  = pts.back();
            const auto prev = pts[pts.size() - 5];
            const float dx = end.x - prev.x, dy = end.y - prev.y;
            const float len = std::hypot (dx, dy);
            if (len < 0.5f) return;
            const float ux = dx / len, uy = dy / len;
            const float px = -uy, py = ux;
            const float s = 6.0f;
            juce::Path tri;
            tri.addTriangle (end.x, end.y,
                             end.x - ux * s + px * s * 0.45f,
                             end.y - uy * s + py * s * 0.45f,
                             end.x - ux * s - px * s * 0.45f,
                             end.y - uy * s - py * s * 0.45f);
            g.setColour (colour.withAlpha (0.95f));
            g.fillPath (tri);
        }

        /** 悬停在连线上时显示「端口 / CHa → CHb」，多条绕行线也不必猜归属。 */
        void drawHoverLabel (juce::Graphics& g) const
        {
            if (hoverEdge_ < 0 || hoverEdge_ >= static_cast<int> (edgeDraws_.size())) return;
            const auto text = linkLabel (edgeDraws_[static_cast<size_t> (hoverEdge_)].edge);
            if (text.isEmpty()) return;

            g.setFont (mb::ui::font (9.0f));
            const int w = juce::jmin (280,
                juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 18);
            const int h = 20;
            const int x = juce::jlimit (4, metrics_.canvasW - w - 4,
                                        juce::roundToInt (lastMouse_.x) + 12);
            const int y = juce::jmax (4, juce::roundToInt (lastMouse_.y) - 26);
            const auto box = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                                     static_cast<float> (w), static_cast<float> (h));
            g.setColour (mb::ui::topBar.withAlpha (0.96f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (mb::ui::border.withAlpha (0.85f));
            g.drawRoundedRectangle (box, 4.0f, 0.7f);
            g.setColour (mb::ui::text);
            g.drawText (text, x + 9, y, w - 18, h, juce::Justification::centredLeft, true);
        }

        void drawPending (juce::Graphics& g)
        {
            if (! pending_) return;
            const auto from = pending_->pos;
            const auto to = mousePos_;

            juce::Path p;
            p.startNewSubPath (from);
            p.cubicTo ({ from.x + metrics_.bendL, from.y },
                       { to.x - metrics_.bendL, to.y }, to);

            const float dashes[] = { 6.0f, 5.0f };
            juce::Path dashed;
            juce::PathStrokeType (1.6f).createDashedStroke (dashed, p, dashes, 2);
            g.setColour (mb::ui::accent[0].withAlpha (0.85f));
            g.strokePath (dashed, juce::PathStrokeType (1.6f));
        }

        /** 拖动插件卡片：落到通道卡片上时高亮目标（卡片本体已随鼠标移动，不再画虚影）。 */
        void drawPlugDragGhost (juce::Graphics& g)
        {
            if (! dragging_ || dragging_->kind != DragNode::Plug) return;
            const int dst = channelAt (mousePos_);
            if (dst >= 0 && ! layout_.chCards.empty())
            {
                const auto hi = layout_.chCards[static_cast<size_t> (dst)];
                g.setColour (mb::ui::accent[0].withAlpha (0.22f));
                g.fillRoundedRectangle (hi, 5.0f);
                g.setColour (mb::ui::accent[0].withAlpha (0.9f));
                g.drawRoundedRectangle (hi, 5.0f, 1.6f);
            }
        }

        //======================================================================
        // 命中测试
        //======================================================================
        bool isHot (const Anchor& dot) const
        {
            if (pending_ && *pending_ == dot) return true;
            return dot.pos.getDistanceFrom (lastMouse_) < dotHitRadius (dot);
        }

        mutable juce::Point<float> lastMouse_;

        /** 连接点按「最近且命中」择优（中点锚已不参与命中，只作连线几何锚）。 */
        std::optional<Anchor> anchorAt (const juce::Point<float>& p) const
        {
            std::optional<Anchor> best;
            float bestDist = 1.0e9f;
            auto considerOne = [&] (const Anchor& d)
            {
                if (d.hidden) return;
                const float dist = d.pos.getDistanceFrom (p);
                if (dist < dotHitRadius (d) && dist < bestDist)
                {
                    bestDist = dist;
                    best = d;
                }
            };
            auto consider = [&] (const std::vector<Anchor>& v)
            {
                for (const auto& d : v) considerOne (d);
            };
            consider (layout_.inDots);
            consider (layout_.outDots);
            consider (layout_.chInL);    consider (layout_.chInR);
            consider (layout_.chOutL);   consider (layout_.chOutR);
            // 插件卡片四个点
            for (const auto& pc : layout_.plugCards)
            {
                considerOne (pc.inL);  considerOne (pc.inR);
                considerOne (pc.outL); considerOne (pc.outR);
            }
            return best;
        }

        /** 点到线段距离：绕行线横穿段采样间距可达 ~15px，只测采样点会漏检。 */
        static float distToSegment (juce::Point<float> p,
                                    juce::Point<float> a, juce::Point<float> b)
        {
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len2 = dx * dx + dy * dy;
            if (len2 < 0.0001f) return p.getDistanceFrom (a);
            float t = juce::jlimit (0.0f, 1.0f,
                                    ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2);
            return p.getDistanceFrom ({ a.x + t * dx, a.y + t * dy });
        }

        int edgeIndexAt (const juce::Point<float>& p) const
        {
            // 倒序：后画的线在上层，优先命中
            for (int k = static_cast<int> (edgeDraws_.size()) - 1; k >= 0; --k)
            {
                const auto& pts = edgeDraws_[static_cast<size_t> (k)].pts;
                if (pts.empty()) continue;
                if (pts.size() == 1)
                {
                    if (pts[0].getDistanceFrom (p) < 6.0f) return k;
                    continue;
                }
                for (size_t i = 1; i < pts.size(); ++i)
                    if (distToSegment (p, pts[i - 1], pts[i]) < 6.0f) return k;
            }
            return -1;
        }

        std::optional<mb::RouteEdge> edgeAt (const juce::Point<float>& p) const
        {
            const int k = edgeIndexAt (p);
            if (k >= 0) return edgeDraws_[static_cast<size_t> (k)].edge;
            return {};
        }

        //======================================================================
        // 自由画布：节点命中 / 偏移 / 吸附 / 空白画布菜单
        //======================================================================
        /** 命中插件卡片本体（倒序：后画的在上层）。 */
        const PlugCard* pluginCardAt (const juce::Point<float>& p) const
        {
            for (int i = static_cast<int> (layout_.plugCards.size()) - 1; i >= 0; --i)
                if (layout_.plugCards[static_cast<size_t> (i)].rect.contains (p))
                    return &layout_.plugCards[static_cast<size_t> (i)];
            return nullptr;
        }

        /** 鼠标是否落在任意卡片（通道 / 端口 / 插件）上。连线命中与悬停提示据此跳过，
            避免连线穿过卡片时误弹「移除连接」菜单或连接名。 */
        bool overAnyCard (const juce::Point<float>& p) const
        {
            if (pluginCardAt (p) != nullptr) return true;
            for (const auto& r : layout_.chCards) if (r.contains (p)) return true;
            for (const auto& r : layout_.inCards) if (r.contains (p)) return true;
            for (const auto& r : layout_.outCards) if (r.contains (p)) return true;
            return false;
        }

        std::optional<DragNode> nodeBodyAt (const juce::Point<float>& p) const
        {
            for (int c = 0; c < graph_.channelCount(); ++c)
                if (! layout_.chCards.empty()
                    && layout_.chCards[static_cast<size_t> (c)].contains (p)
                    && mb::ui::slotState (c) != mb::ui::kSlotEmpty)
                    return DragNode { DragNode::Ch, c };
            for (size_t i = 0; i < layout_.inCards.size(); ++i)
                if (layout_.inCards[i].contains (p)) return DragNode { DragNode::In, (int) i };
            for (size_t i = 0; i < layout_.outCards.size(); ++i)
                if (layout_.outCards[i].contains (p)) return DragNode { DragNode::Out, (int) i };
            return {};
        }

        juce::Point<float> nodeHomePos (const DragNode& dn) const
        {
            // 返回节点的「家」（零偏移）坐标：当前卡片位置减去已生效偏移
            const auto cur = dn.kind == DragNode::Ch ? layout_.chCards[static_cast<size_t> (dn.id)]
                            : dn.kind == DragNode::In ? layout_.inCards[static_cast<size_t> (dn.id)]
                                                      : layout_.outCards[static_cast<size_t> (dn.id)];
            const auto off = dn.kind == DragNode::Ch ? chOff_[static_cast<size_t> (dn.id)]
                            : dn.kind == DragNode::In ? inOff_[static_cast<size_t> (dn.id)]
                                                      : outOff_[static_cast<size_t> (dn.id)];
            return { cur.getX() - off.x, cur.getY() - off.y };
        }

        juce::Point<float> snapPoint (juce::Point<float> p) const
        {
            if (! snapOn_) return p;
            return { std::round (p.x / grid_) * grid_, std::round (p.y / grid_) * grid_ };
        }

        void setNodeOffset (const DragNode& dn, juce::Point<float> off)
        {
            off.x = juce::jlimit (-300.0f, 8000.0f, off.x);
            off.y = juce::jlimit (-static_cast<float> (kTop), 12000.0f, off.y);
            if (dn.kind == DragNode::Ch)
                chOff_[static_cast<size_t> (dn.id)] = off;
            else if (dn.kind == DragNode::In)
                inOff_[static_cast<size_t> (dn.id)] = off;
            else
                outOff_[static_cast<size_t> (dn.id)] = off;
        }

        //======================================================================
        // 通道节点控件几何 / 命中
        //======================================================================
        juce::Rectangle<float> monoRectFor (int c) const   // 声道制式徽标热区
        {
            const auto card = layout_.chCards[static_cast<size_t> (c)];
            return { card.getRight() - 28.0f, card.getY() + 5.0f, 18.0f, 17.0f };
        }

        juce::Rectangle<float> bodyRectFor (int c) const
        {
            const auto card = layout_.chCards[static_cast<size_t> (c)];
            return { card.getX(), card.getY() + (float) kChHeadH,
                     card.getWidth(), card.getHeight() - (float) kChHeadH };
        }

        /** 三区正文：左 30% 输入 / 中 40% 插件 / 右 30% 输出（不再列出插件行）。 */
        juce::Rectangle<float> inputZoneFor (int c) const
        {
            const auto b = bodyRectFor (c);
            return { b.getX(), b.getY(), b.getWidth() * 0.30f, b.getHeight() };
        }

        juce::Rectangle<float> pluginZoneFor (int c) const
        {
            const auto b = bodyRectFor (c);
            return { b.getX() + b.getWidth() * 0.30f, b.getY(),
                     b.getWidth() * 0.40f, b.getHeight() };
        }

        juce::Rectangle<float> outputZoneFor (int c) const
        {
            const auto b = bodyRectFor (c);
            return { b.getX() + b.getWidth() * 0.70f, b.getY(),
                     b.getWidth() * 0.30f, b.getHeight() };
        }

        /** 命中通道节点（空槽不算）：返回通道序号，未命中 -1。 */
        int channelAt (const juce::Point<float>& p) const
        {
            for (int c = 0; c < graph_.channelCount(); ++c)
                if (! layout_.chCards.empty()
                    && layout_.chCards[static_cast<size_t> (c)].contains (p)
                    && mb::ui::slotState (c) != mb::ui::kSlotEmpty)
                    return c;
            return -1;
        }

        //======================================================================
        // 「+插件」区：弹出调音台对应窗口列表（各插件窗口 / 添加插件）
        //======================================================================
        //======================================================================
        public:
        // 自由画布布局持久化：与 SessionStore 对接（session.json 的 nodeLayout 字段）
        //======================================================================
        /** 新增自由插件后由 MainComponent 调用：给该 UID 指定初始画布位置。 */
        void setPluginPosition (const juce::String& uid, const juce::Point<float>& pos)
        {
            if (uid.isEmpty()) return;
            plugPos_[uid] = pos;
            updateSize();
            repaint();
        }

        juce::var layoutToJson() const
        {
            auto* o = new juce::DynamicObject();
            auto pack = [] (const std::vector<juce::Point<float>>& v)
            {
                juce::Array<juce::var> a;
                for (const auto& pt : v) { a.add (pt.x); a.add (pt.y); }
                return juce::var (a);
            };
            o->setProperty ("ch",  pack (chOff_));
            o->setProperty ("in",  pack (inOff_));
            o->setProperty ("out", pack (outOff_));
            auto* po = new juce::DynamicObject();
            for (const auto& kv : plugPos_)
            {
                juce::Array<juce::var> a;
                a.add (kv.second.x); a.add (kv.second.y);
                po->setProperty (kv.first, juce::var (a));
            }
            o->setProperty ("plugPos", juce::var (po));
            return juce::var (o);
        }

        /** 载入后立即 updateSize()：computeLayout 会按当前通道/端口数补齐缺失项。 */
        void layoutFromJson (const juce::var& v)
        {
            chOff_.clear(); inOff_.clear(); outOff_.clear();
            auto* o = v.getDynamicObject();
            if (o == nullptr) { updateSize(); return; }
            auto unpack = [] (const juce::var& arr, std::vector<juce::Point<float>>& dst)
            {
                if (! arr.isArray()) return;
                const auto* a = arr.getArray();
                const int pairs = a->size() / 2;
                dst.assign (static_cast<size_t> (pairs), { 0.0f, 0.0f });
                for (int i = 0; i < pairs; ++i)
                    dst[static_cast<size_t> (i)] = {
                        static_cast<float> (static_cast<double> (a->getUnchecked (i * 2))),
                        static_cast<float> (static_cast<double> (a->getUnchecked (i * 2 + 1))) };
            };
            unpack (o->getProperty ("ch"),  chOff_);
            unpack (o->getProperty ("in"),  inOff_);
            unpack (o->getProperty ("out"), outOff_);
            plugPos_.clear();
            if (auto* po = o->getProperty ("plugPos").getDynamicObject())
                for (auto& kv : po->getProperties())
                {
                    const auto& a = kv.value;
                    if (a.isArray() && a.getArray()->size() >= 2)
                        plugPos_[kv.name.toString()] = {
                            static_cast<float> (static_cast<double> (a.getArray()->getUnchecked (0))),
                            static_cast<float> (static_cast<double> (a.getArray()->getUnchecked (1))) };
                }
            updateSize();
        }

        //======================================================================
        // 右键菜单：空白画布 / 通道卡片 / 插件卡片 / 插件圆点
        //======================================================================
        /** 空白区右键：新增通道 / 在光标处新增自由插件（多选，无需先选通道）。 */
        void showCanvasContextMenu (const juce::Point<float>& p)
        {
            juce::PopupMenu m;
            m.addSectionHeader (mb::tr ("节点视图", "Node view"));
            const bool canAddCh = graph_.channelCount() < 16;
            m.addItem (1, mb::S("+ ") + mb::tr ("通道", "Channel"), canAddCh);
            m.addItem (2, mb::S("+ ") + mb::tr ("插件", "Plugin"));

            juce::Component::SafePointer<NodeCanvas> safe (this);
            m.showMenuAsync (menuOptions (p), [safe, p] (int r)
            {
                if (safe == nullptr) return;
                if (r == 1) { if (safe->onAddChannel) safe->onAddChannel(); return; }
                if (r == 2 && safe->onAddFreePlugins) safe->onAddFreePlugins (p);
            });
        }

        /** 通道卡片右键：与经典视图「电平 / 通用区右键」菜单逐项一致。 */
        void showChannelContextMenu (int ch, const juce::Point<float>& p)
        {
            juce::PopupMenu m;
            m.addSectionHeader (mb::S("CH") + juce::String (ch + 1) + mb::S("  ")
                                + mb::channelDisplayName (ch));
            m.addItem (1, mb::tr ("复制通道", "Duplicate channel"));
            const bool right = mb::ui::isChannelRightAligned (ch);
            m.addItem (2, right ? mb::tr ("取消向右对齐", "Remove right alignment")
                                : mb::tr ("向右对齐", "Align right"), true, right);
            m.addSeparator();

            // 移除连接…：所有进入 / 离开本通道的边（经典视图同款）
            auto ctxEdges = std::make_shared<std::vector<mb::RouteEdge>>();
            for (const auto& e : graph_.edges())
                if (e.src == ch || e.dst == ch) ctxEdges->push_back (e);
            if (! ctxEdges->empty())
            {
                juce::PopupMenu rm;
                int id = 100;
                for (const auto& e : *ctxEdges)
                    rm.addItem (id++, mb::tr ("移除  ", "Remove  ") + linkLabel (e));
                m.addSubMenu (mb::tr ("移除连接…", "Remove connection…"), rm, true);
            }

            m.addItem (3, mb::tr ("颜色与图标…", "Colour & icon…"));
            m.addSeparator();
            m.addItem (4, mb::tr ("快捷键与链接…", "Hotkeys & links…"));
            m.addSeparator();
            m.addItem (5, mb::tr ("重命名通道", "Rename channel"));
            m.addItem (6, mb::tr ("隐藏通道", "Hide channel"));

            juce::PopupMenu reveal;
            int hidden = 0;
            for (int c = 0; c < graph_.channelCount(); ++c)
                if (mb::ui::slotState (c) == mb::ui::kSlotHidden)
                {
                    reveal.addItem (200 + c, mb::S("CH") + juce::String (c + 1)
                        + mb::S("  ·  ") + mb::channelDisplayName (c));
                    ++hidden;
                }
            if (hidden > 0)
            {
                reveal.addSeparator();
                reveal.addItem (199, mb::tr ("显示全部通道", "Show all channels"));
                m.addSubMenu (mb::tr ("显示已隐藏通道", "Show hidden channels"), reveal, true);
            }
            else
                m.addItem (199, mb::tr ("显示全部通道", "Show all channels"), false);

            m.addSeparator();
            m.addItem (7, mb::tr ("删除通道", "Delete channel"));

            juce::Component::SafePointer<NodeCanvas> safe (this);
            m.showMenuAsync (menuOptions (p),
                [safe, ch, ctxEdges, right] (int r)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    if (r == 1)      { if (self->onDuplicateChannel) self->onDuplicateChannel (ch); return; }
                    if (r == 2)      { if (self->onSetRightAligned) self->onSetRightAligned (ch, ! right); return; }
                    if (r == 3)      { if (self->onPickColour) self->onPickColour (ch); return; }
                    if (r == 4)      { if (self->onLinkChannel) self->onLinkChannel (ch); return; }
                    if (r == 5)      { self->beginRename (ch); return; }
                    if (r == 6)      { if (self->onSetHidden) self->onSetHidden (ch, true); return; }
                    if (r == 199)    { if (self->onShowAllChannels) self->onShowAllChannels(); return; }
                    if (r >= 200 && r < 200 + 32)
                    {
                        if (self->onSetHidden) self->onSetHidden (r - 200, false);
                        return;
                    }
                    if (r >= 100 && r - 100 < static_cast<int> (ctxEdges->size()))
                    {
                        const auto& e = (*ctxEdges)[static_cast<size_t> (r - 100)];
                        self->graph_.removeEdge (e.src, e.dst, e.kind);
                        if (self->onChanged) self->onChanged();
                        if (self->onMessage) self->onMessage (mb::tr ("已移除连接", "Connection removed"));
                        self->repaint();
                    }
                    if (r == 7 && self->onDeleteChannel) self->onDeleteChannel (ch);
                });
        }

        /** 插件卡片右键：与经典视图「插件行右键」逐项一致。
            顺序：旁通 → 离线 → 打开界面 → 重命名 → 快捷键与链接 → 查看目录 → 删除。 */
        void showPluginContextMenu (const PlugCard& pc, const juce::Point<float>& p)
        {
            const int bc = pc.ch < 0 ? mb::ChannelPluginBank::kFreePluginSlot : pc.ch;
            const auto& bank = engine_.pluginBank();
            const auto name = bank.pluginName (bc, pc.idx);
            const bool bypass = bank.pluginBypass (bc, pc.idx);
            const bool offline = bank.pluginOffline (bc, pc.idx);
            const bool builtIn = bank.pluginBuiltInId (bc, pc.idx) > 0;
            const auto note = bank.pluginNote (bc, pc.idx);
            const auto ident = bank.pluginIdentifier (bc, pc.idx);

            juce::PopupMenu m;
            m.addSectionHeader (note.isEmpty() ? name : note + mb::S(" (") + name + mb::S(")"));
            m.addItem (1, bypass ? mb::tr ("启用插件", "Enable plugin")
                                 : mb::tr ("旁通插件", "Bypass plugin"));
            m.addItem (2, offline ? mb::tr ("取消离线", "Bring online")
                                  : mb::tr ("离线插件", "Take offline"), ! builtIn);
            m.addItem (3, mb::tr ("打开插件界面", "Open editor window"), ! offline);
            m.addItem (4, mb::tr ("重命名", "Rename"));
            m.addItem (5, mb::tr ("快捷键与链接", "Hotkey & link"));
            m.addItem (6, mb::tr ("查看目录", "Reveal in folder"), ! builtIn);
            m.addSeparator();
            m.addItem (7, mb::tr ("删除插件", "Remove plugin"));

            juce::Component::SafePointer<NodeCanvas> safe (this);
            m.showMenuAsync (menuOptions (p),
                [safe, ch = pc.ch, idx = pc.idx, ident] (int r)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    if (r == 6)
                    {
                        const juce::File f (ident);
                        if (f.existsAsFile())           f.revealToUser();
                        else if (f.isDirectory())       f.startAsProcess();
                        else if (self->onMessage) self->onMessage (
                            mb::tr ("插件文件不存在", "Plugin file not found"));
                        return;
                    }
                    if (r == 1 && self->onPluginBypass)      { self->onPluginBypass (ch, idx); return; }
                    if (r == 2 && self->onPluginOffline)     { self->onPluginOffline (ch, idx); return; }
                    if (r == 3 && self->onOpenPlugin)        { self->onOpenPlugin (ch, idx); return; }
                    if (r == 4)
                    {
                        // 弹输入框重命名（与经典视图「重命名插件」一致），确认后写备注
                        const int bc = ch < 0 ? mb::ChannelPluginBank::kFreePluginSlot : ch;
                        auto& bank = self->engine_.pluginBank();
                        auto* aw = new juce::AlertWindow (
                            mb::tr ("重命名插件", "Rename plugin"), bank.pluginName (bc, idx),
                            juce::MessageBoxIconType::NoIcon);
                        aw->addTextEditor ("note", bank.pluginNote (bc, idx),
                                           mb::tr ("新名称（显示为「名称 (插件名)」，清空即恢复原名）",
                                                   "New name (shown as \"name (plugin)\"; empty = original)"));
                        aw->addButton (mb::tr ("保存", "Save"),   1);
                        aw->addButton (mb::tr ("取消", "Cancel"), 0);
                        aw->enterModalState (true,
                            juce::ModalCallbackFunction::create ([safe, ch, idx, aw] (int r2)
                            {
                                if (r2 == 1 && safe != nullptr && safe->onRenamePlugin)
                                    if (auto* ed = aw->getTextEditor ("note"))
                                        safe->onRenamePlugin (ch, idx, ed->getText());
                            }),
                            true);
                        return;
                    }
                    if (r == 5 && self->onPluginLink)
                    {
                        // 插件入口 = 控制整条效果链，默认预选「效果」动作
                        self->onPluginLink (ch < 0 ? 0 : ch, mb::links::kFx);
                        return;
                    }
                    if (r == 7 && self->onRemovePlugin)      { self->onRemovePlugin (ch, idx); return; }
                });
        }

        /** 点击插件输入/输出圆点：只断开与通道的连接，插件卡片保留在画布上。
            真正删除插件在插件卡片右键菜单里。 */
        void showPlugDotMenu (const Anchor& a, const juce::Point<float>& p)
        {
            const auto& bank = engine_.pluginBank();
            const int bc = a.id < 0 ? mb::ChannelPluginBank::kFreePluginSlot : a.id;
            if (a.plugIdx < 0 || a.plugIdx >= bank.pluginCount (bc)) return;
            const auto name = bank.pluginName (bc, a.plugIdx);

            juce::PopupMenu m;
            m.addSectionHeader (a.id < 0
                ? name
                : mb::S("CH") + juce::String (a.id + 1) + mb::S("  ") + name);
            if (a.id < 0)
                m.addItem (-1, mb::tr ("该插件尚未连接通道", "Plugin is not connected"), false);
            else
                m.addItem (1, mb::tr ("断开连接（保留插件）", "Disconnect (keep plugin)"));

            juce::Component::SafePointer<NodeCanvas> safe (this);
            m.showMenuAsync (menuOptions (p), [safe, bc, idx = a.plugIdx] (int r)
            {
                if (safe == nullptr || r != 1 || ! safe->onMovePlugin) return;
                safe->onMovePlugin (bc, idx, mb::ChannelPluginBank::kFreePluginSlot, -1);
                if (safe->onMessage) safe->onMessage (mb::tr ("已断开插件连接", "Plugin disconnected"));
            });
        }

        //======================================================================
        void showRemoveMenu (const mb::RouteEdge& e, const juce::Point<float>& p)
        {
            juce::PopupMenu m;
            m.addItem (1, mb::tr ("移除连接  ", "Remove  ") + linkLabel (e));
            if (e.kind == mb::EdgeKind::ChannelToChannel && e.side == -1)
            {
                m.addSeparator();
                m.addItem (2, mb::tr ("仅移除 L 声道", "Remove L channel only"));
                m.addItem (3, mb::tr ("仅移除 R 声道", "Remove R channel only"));
            }
            juce::Component::SafePointer<NodeCanvas> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options()
                               .withTargetScreenArea (juce::Rectangle<int> (
                                   juce::roundToInt (p.x) + 4,
                                   juce::roundToInt (p.y) - 8, 2, 2))
                               .withParentComponent (mb::ui::hostParentFor (*this)),
                [safe, e] (int r)
                {
                    if (safe == nullptr) return;
                    if (r == 2 || r == 3)          // 只移除一条声道（另一条保留）
                    {
                        safe->graph_.removeChannelEdgeSide (e.src, e.dst, r - 2);
                        if (safe->onChanged) safe->onChanged();
                        if (safe->onMessage) safe->onMessage (mb::tr ("已移除连接", "Connection removed"));
                        safe->repaint();
                        return;
                    }
                    if (r != 1) return;
                    safe->graph_.removeEdge (e.src, e.dst, e.kind);
                    if (safe->onChanged) safe->onChanged();
                    if (safe->onMessage) safe->onMessage (mb::tr ("已移除连接", "Connection removed"));
                    safe->repaint();
                });
        }

        juce::String linkLabel (const mb::RouteEdge& e) const
        {
            // 与调音台 edgeLabel 一致：通道段带名称（CHn  名称），移除时不用再猜编号
            auto ch = [] (int c)
            {
                return mb::S("CH") + juce::String (c + 1) + mb::S("  ")
                     + mb::channelDisplayName (c);
            };
            switch (e.kind)
            {
                case mb::EdgeKind::InputToChannel:
                    return catalog_.portLabel (true, e.src) + mb::S(" → ") + ch (e.dst);
                case mb::EdgeKind::ChannelToOutput:
                    return ch (e.src) + mb::S(" → ") + catalog_.portLabel (false, e.dst);
                case mb::EdgeKind::ChannelToChannel:
                    return ch (e.src) + mb::S(" → ") + ch (e.dst);
            }
            return {};
        }

        /** 点击通道 L/R 连接点（原地松手）：弹出锚定该点的连接列表，点击条目移除。
            输入点 = 所有进入本通道的连接；输出点 = 所有从本通道送出的连接。 */
        void showAnchorMenu (const Anchor& a, const juce::Point<float>& p)
        {
            const bool in = a.kind == Anchor::ChIn;
            std::vector<mb::RouteEdge> edges;
            for (const auto& e : graph_.edges())
                if (in ? e.dst == a.id : e.src == a.id)
                    edges.push_back (e);

            if (edges.empty())
            {
                if (onMessage)
                    onMessage (mb::tr ("该连接点没有连接", "No connections on this dot"));
                return;
            }

            juce::PopupMenu m;
            m.addSectionHeader (in ? mb::tr ("输入连接 · 点击移除", "Inputs · click to remove")
                                   : mb::tr ("输出连接 · 点击移除", "Outputs · click to remove"));
            int id = 100;
            for (const auto& e : edges)
                m.addItem (id++, mb::tr ("移除  ", "Remove  ") + linkLabel (e));

            juce::Component::SafePointer<NodeCanvas> safe (this);
            m.showMenuAsync (menuOptions (p),
                [safe, edges] (int r)
                {
                    if (safe == nullptr || r < 100 || r - 100 >= (int) edges.size()) return;
                    const auto& e = edges[static_cast<size_t> (r - 100)];
                    safe->graph_.removeEdge (e.src, e.dst, e.kind);
                    if (safe->onChanged) safe->onChanged();
                    if (safe->onMessage) safe->onMessage (mb::tr ("已移除连接", "Connection removed"));
                    safe->repaint();
                });
        }

        juce::PopupMenu::Options menuOptions (const juce::Point<float>& p) const
        {
            return juce::PopupMenu::Options().withTargetScreenArea (
                       juce::Rectangle<int> (juce::roundToInt (p.x) + 4,
                                             juce::roundToInt (p.y) - 8, 2, 2))
                       .withParentComponent (mb::ui::hostParentFor (*this));
        }

        //======================================================================
        // 通道重命名：与调音台 ChannelStrip 共用 mb::customChannelNames() 名字表
        //======================================================================
        void beginRename (int c)                 { beginRenameWith (c, mb::channelDisplayName (c)); }

        void beginRenameWith (int c, const juce::String& initial)
        {
            if (nameEditor_ != nullptr) return;
            renamingChannel_ = c;
            renaming_ = true;

            nameEditor_ = std::make_unique<juce::TextEditor>();
            nameEditor_->setText (initial, false);
            nameEditor_->setSelectAllWhenFocused (true);
            nameEditor_->setJustification (juce::Justification::centred);
            nameEditor_->setIndents (4, 0);
            nameEditor_->onReturnKey = [this] { commitRename(); };
            nameEditor_->onEscapeKey = [this] { closeRename(); };
            nameEditor_->onFocusLost = [this] { if (renaming_) commitRename(); };
            addAndMakeVisible (nameEditor_.get());

            const auto card = layout_.chCards[static_cast<size_t> (c)].toNearestInt();
            nameEditor_->setBounds (card.getX() + 28, card.getY() + 4,
                                    juce::jmax (40, card.getWidth() - 68), kChHeadH - 8);
            nameEditor_->grabKeyboardFocus();
        }

        void commitRename()
        {
            if (nameEditor_ == nullptr || ! renaming_) return;
            renaming_ = false;
            const auto t = nameEditor_->getText().trim();

            auto& names = mb::customChannelNames();
            while (names.size() <= renamingChannel_) names.add (juce::String());
            // 留空不写空串：保持名字表与通道一一对应（与调音台行为一致）
            names.set (renamingChannel_, t.isNotEmpty() ? t : mb::channelName (renamingChannel_));

            closeRename();
            if (onChanged) onChanged();              // 触发会话保存 + 调音台刷新
            if (onMessage) onMessage (mb::tr ("已重命名", "Renamed"));
            repaint();
        }

        void closeRename()
        {
            renaming_ = false;
            renamingChannel_ = -1;
            if (nameEditor_ == nullptr) return;
            nameEditor_ = nullptr;                   // 从组件树移除并析构
            repaint();
        }

        //======================================================================
        // 建线：拖拽起点 / 终点 → 三类边（合法性由 tryAddEdge 最终把关）
        //======================================================================
        /** 找与 portId 构成同设备 L/R 对的另一半端口（且可见）；找不到返回 -1。
            规则与 SourcePicker / MainComponent::collectPortPairs 一致。 */
        int stereoPartner (bool input, int portId) const
        {
            const auto& ports = input ? catalog_.inputPorts() : catalog_.outputPorts();
            if (portId < 0 || portId >= static_cast<int> (ports.size())) return -1;
            const auto& p = ports[static_cast<size_t> (portId)];
            if (p.channelIndex != 0 && p.channelIndex != 1) return -1;
            for (int i = 0; i < static_cast<int> (ports.size()); ++i)
            {
                if (i == portId) continue;
                const auto& q = ports[static_cast<size_t> (i)];
                if (q.input == p.input
                    && q.deviceIndex == p.deviceIndex
                    && q.channelIndex == 1 - p.channelIndex
                    && catalog_.portVisible (input, i))
                    return i;
            }
            return -1;
        }

        /** 插件卡片连线：PlugOut→PlugIn 表达顺序（同通道重排）/ 改归属（跨通道）；
            PlugOut→通道输入点 = 改归属到该通道。
            明确禁止插件点连接设备端口（InPort/OutPort）——插件是通道内部处理链，
            不是独立音源/汇，接设备没有音频语义。 */
        bool makePlugLink (const Anchor& a, const Anchor& b)
        {
            auto isPlug = [] (Anchor::Kind k) { return k == Anchor::PlugIn || k == Anchor::PlugOut; };
            auto bankCh = [] (int id) { return id < 0 ? mb::ChannelPluginBank::kFreePluginSlot : id; };
            // 插件点 ↔ 设备端口：禁用
            if ((isPlug (a.kind) && (b.kind == Anchor::InPort || b.kind == Anchor::OutPort))
                || (isPlug (b.kind) && (a.kind == Anchor::InPort || a.kind == Anchor::OutPort)))
            {
                if (onMessage) onMessage (mb::tr ("插件不能直连设备端口", "Plugins cannot connect to device ports"));
                return false;
            }

            // 归一化：确保 out 端在前、in 端在后（支持反向拖拽）
            Anchor out, in;
            bool pluginToCh = false;
            int targetCh = -1;
            if (a.kind == Anchor::PlugOut && b.kind == Anchor::PlugIn)       { out = a; in = b; }
            else if (a.kind == Anchor::PlugIn && b.kind == Anchor::PlugOut)  { out = b; in = a; }
            else if (a.kind == Anchor::PlugOut && (b.kind == Anchor::ChIn || b.kind == Anchor::ChOut))
            { out = a; pluginToCh = true; targetCh = b.id; }
            else if (b.kind == Anchor::PlugOut && (a.kind == Anchor::ChIn || a.kind == Anchor::ChOut))
            { out = b; pluginToCh = true; targetCh = a.id; }
            else
            {
                if (onMessage) onMessage (mb::tr ("该方向不支持连接", "Unsupported connection direction"));
                return false;
            }

            const int src = bankCh (out.id);
            auto& bank = engine_.pluginBank();
            if (pluginToCh)
            {
                // 插件 → 通道输入点：建立归属连接（自由插件 / 其他通道均可）
                if (src == targetCh) return false;
                if (onMovePlugin) onMovePlugin (src, out.plugIdx, targetCh, -1);
                else if (bank.movePlugin (src, out.plugIdx, targetCh) < 0) return false;
                if (onMessage) onMessage (mb::tr ("已连接插件到 ", "Plugin connected to ")
                    + mb::channelDisplayName (targetCh));
                if (onChanged) onChanged();
                return true;
            }

            const int dst = bankCh (in.id);
            const int targetIdx = in.plugIdx;      // 插到目标插件之前 = 排在它前面
            if (src == dst && out.plugIdx == targetIdx) return false;

            // 同通道内重排：同步交换两张卡片的画布位置，视觉顺序 = 处理顺序
            if (src == dst)
            {
                const auto ua = bank.pluginUid (src, out.plugIdx);
                const auto ub = bank.pluginUid (dst, targetIdx);
                if (ua.isNotEmpty() && ub.isNotEmpty())
                    std::swap (plugPos_[ua], plugPos_[ub]);
                if (onMovePlugin) onMovePlugin (src, out.plugIdx, dst, targetIdx);
                else if (! bank.reorderPlugin (src, out.plugIdx, targetIdx))
                {
                    if (ua.isNotEmpty() && ub.isNotEmpty()) std::swap (plugPos_[ua], plugPos_[ub]);
                    return false;
                }
                if (onMessage) onMessage (mb::tr ("已调整插件顺序", "Plugin order changed"));
                if (onChanged) onChanged();
                return true;
            }

            // 跨通道 / 自由插件之间：移动并按落点插入
            if (onMovePlugin) onMovePlugin (src, out.plugIdx, dst, targetIdx);
            else
            {
                const int ni = bank.movePlugin (src, out.plugIdx, dst);
                if (ni < 0) return false;
                if (targetIdx >= 0) bank.reorderPlugin (dst, ni, targetIdx);
            }
            if (onMessage) onMessage (mb::tr ("已移动插件到 ", "Plugin moved to ")
                + (dst == mb::ChannelPluginBank::kFreePluginSlot
                   ? mb::tr ("未连接区", "Unconnected")
                   : mb::channelDisplayName (dst)));
            if (onChanged) onChanged();
            return true;
        }

        bool makeEdge (const Anchor& a, const Anchor& b)
        {
            // —— 插件卡片连接：不产生 RouteEdge，纯表达「顺序 / 归属」——
            if (a.kind == Anchor::PlugIn || a.kind == Anchor::PlugOut
                || b.kind == Anchor::PlugIn || b.kind == Anchor::PlugOut)
                return makePlugLink (a, b);

            mb::RouteEdge e { -1, -1, mb::EdgeKind::InputToChannel, 1.0f };
            bool valid = false;
            int portId = -1;                 // 端口端 id（C→C 时为 -1）
            bool portIsInput = true;

            if (a.kind == Anchor::InPort
                && (b.kind == Anchor::ChIn || b.kind == Anchor::ChOut))
            {
                e.src = a.id; e.dst = b.id; e.kind = mb::EdgeKind::InputToChannel;
                portId = a.id; portIsInput = true; valid = true;
            }
            else if (a.kind == Anchor::ChIn && b.kind == Anchor::InPort)
            {
                // 反向拖拽：通道接收端 → 输入端口
                e.src = b.id; e.dst = a.id; e.kind = mb::EdgeKind::InputToChannel;
                portId = b.id; portIsInput = true; valid = true;
            }
            else if (a.kind == Anchor::ChOut && b.kind == Anchor::OutPort)
            {
                e.src = a.id; e.dst = b.id; e.kind = mb::EdgeKind::ChannelToOutput;
                portId = b.id; portIsInput = false; valid = true;
            }
            else if (a.kind == Anchor::OutPort && b.kind == Anchor::ChOut)
            {
                // 反向拖拽：输出端口 → 通道发送端
                e.src = b.id; e.dst = a.id; e.kind = mb::EdgeKind::ChannelToOutput;
                portId = a.id; portIsInput = false; valid = true;
            }
            else if (a.kind == Anchor::ChOut
                     && (b.kind == Anchor::ChIn || b.kind == Anchor::ChOut))
            {
                e.src = a.id; e.dst = b.id; e.kind = mb::EdgeKind::ChannelToChannel;
                e.side = (a.role == 2) ? 1 : 0;    // 从 R 点拖出 = 仅 R；否则仅 L
                valid = true;
            }
            else if (a.kind == Anchor::ChIn && b.kind == Anchor::ChOut && a.id != b.id)
            {
                // 反向拖拽：从接收端拖到发送端，仍建成同一条 C→C（src = 发送方）
                e.src = b.id; e.dst = a.id; e.kind = mb::EdgeKind::ChannelToChannel;
                e.side = (b.role == 2) ? 1 : 0;
                valid = true;
            }

            if (! valid)
            {
                if (onMessage) onMessage (mb::tr ("该方向不支持连接", "Unsupported connection direction"));
                return false;
            }

            std::vector<mb::RouteEdge> toAdd { e };
            // C→C 互送同样成对补齐：L/R 任一声道接入即自动补另一条（可单独断开）
            if (e.kind == mb::EdgeKind::ChannelToChannel)
            {
                auto e2 = e;
                e2.side = 1 - e.side;             // 自动补齐另一声道
                toAdd.push_back (e2);
            }
            // 严格立体声连接：端口有同设备互补声道就自动补齐另一条（L/R 任一接入皆成对，
            // 事后可单独断开任意一条）；单声道设备无互补声道 → 单条边，引擎自动居中到 L/R
            else if (portId >= 0)
            {
                const int partner = stereoPartner (portIsInput, portId);
                if (partner >= 0)
                {
                    auto e2 = e;
                    if (portIsInput) e2.src = partner;
                    else             e2.dst = partner;
                    toAdd.push_back (e2);
                }
            }

            std::string why;
            int added = 0;
            for (const auto& edge : toAdd)
                if (graph_.tryAddEdge (edge, why)) ++added;

            if (added == 0)
            {
                if (onMessage) onMessage (mb::tr ("连接被拒绝：", "Rejected: ") + juce::String (why));
                return false;                        // 拒绝后保留选中，可立即改连别处
            }
            if (onChanged) onChanged();
            if (onMessage) onMessage (added > 1
                ? mb::tr ("已建立立体声连接 (L+R)", "Stereo connection added (L+R)")
                : (toAdd.size() > 1
                   ? mb::tr ("仅建立单条连接（立体声另一侧不可用）",
                             "Only one connection added (stereo partner unavailable)")
                   : mb::tr ("已建立连接", "Connection added")));
            return true;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodeCanvas)
    };

    //==============================================================================
    /** 节点视图顶部固定工具栏：吸附开关 / 重置布局。
        作为 MainComponent 的覆盖层浮在 nodeViewport_ 之上，不随画布内容滚动。 */
    //==============================================================================
    class NodeToolbar : public juce::Component
    {
    public:
        NodeToolbar() = default;

        std::function<void()> onToggleSnap;
        std::function<void()> onResetLayout;
        std::function<bool()> getSnap;

        void paint (juce::Graphics& g) override
        {
            const float w = static_cast<float> (getWidth());
            const float h = static_cast<float> (getHeight());
            g.setColour (mb::ui::topBar.withAlpha (0.96f));
            g.fillRect (0.0f, 0.0f, w, h);
            g.setColour (mb::ui::border.withAlpha (0.85f));
            g.drawHorizontalLine (h - 1.0f, 0.0f, w);

            const bool snap = (getSnap != nullptr) && getSnap();
            drawButton (g, snapRect(), snap
                ? mb::tr ("吸附 开", "Snap on") : mb::tr ("吸附 关", "Snap off"), snap);
            drawButton (g, resetRect(), mb::tr ("重置布局", "Reset layout"), false);
        }

        void mouseMove (const juce::MouseEvent& ev) override
        {
            lastMouse_ = ev.position;
            const bool s = snapRect().contains (ev.position);
            const bool r = resetRect().contains (ev.position);
            if (s != hotSnap_ || r != hotReset_)
            {
                hotSnap_ = s; hotReset_ = r; repaint();
            }
            setMouseCursor ((s || r) ? juce::MouseCursor::PointingHandCursor
                                     : juce::MouseCursor::NormalCursor);
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            if (hotSnap_ || hotReset_) { hotSnap_ = hotReset_ = false; repaint(); }
            setMouseCursor (juce::MouseCursor::NormalCursor);
        }

        void mouseDown (const juce::MouseEvent& ev) override
        {
            if (snapRect().contains (ev.position))   { if (onToggleSnap) onToggleSnap(); return; }
            if (resetRect().contains (ev.position))  { if (onResetLayout) onResetLayout(); return; }
        }

    private:
        bool hotSnap_ = false, hotReset_ = false;

        juce::Rectangle<float> snapRect() const
            { return { static_cast<float> (getWidth()) - 142.0f, 6.0f, 64.0f, 18.0f }; }
        juce::Rectangle<float> resetRect() const
            { return { static_cast<float> (getWidth()) - 72.0f, 6.0f, 60.0f, 18.0f }; }

        void drawButton (juce::Graphics& g, const juce::Rectangle<float>& r,
                         const juce::String& text, bool lit)
        {
            const bool hot = r.contains (lastMouse_);
            g.setColour ((lit ? mb::ui::accent[0].withAlpha (0.20f)
                              : mb::ui::card.withAlpha (hot ? 0.95f : 0.72f)));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour ((lit ? mb::ui::accent[0] : mb::ui::border).withAlpha (hot ? 0.9f : 0.6f));
            g.drawRoundedRectangle (r, 4.0f, 0.8f);
            g.setFont (mb::ui::font (8.5f, lit));
            g.setColour (lit ? mb::ui::accent[0].brighter (0.2f) : mb::ui::subText);
            g.drawText (text, r.toNearestInt(), juce::Justification::centred, false);
        }

        juce::Point<float> lastMouse_ { -1000.0f, -1000.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodeToolbar)
    };
}
