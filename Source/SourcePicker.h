#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <vector>

#include "AudioEngine.h"
#include "DeviceCatalog.h"
#include "I18n.h"
#include "RouteGraph.h"
#include "Theme.h"

//==============================================================================
// 浮层卡片：标题行 + 双栏标题 + 双栏滚动列表（输入源 / 添加插件浮层共用）。
//==============================================================================
class PickerCard : public juce::Component
{
public:
    juce::String title, channel, hint, titleLeft, titleRight;
    int devSplit = 0;                 // 左右栏分界 x（卡片坐标）
    std::function<void()> onCloseClicked;
    std::function<void()> onConfirmClicked;   // 多选模式「确定」
    std::function<void()> onCancelClicked;    // 多选模式「取消」
    bool showConfirmButtons = false;          // 多选模式：底部显示 取消 / 确定
    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const int w = getWidth(), h = getHeight();
        g.setColour (mb::ui::background);
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (r, 10.0f, 1.0f);

        g.setFont (mb::ui::font (14.0f, true));
        g.setColour (mb::ui::text);
        g.drawText (title, 18, 12, w - 60, 22, juce::Justification::centredLeft);

        // 编号与通道名：紧跟标题右侧，同字号同色，整体加括号
        const int titleW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), title) + 10;
        g.drawText (channel, 18 + titleW, 12, juce::jmax (40, w - 60 - titleW), 22,
                    juce::Justification::centredLeft);

        g.setFont (mb::ui::font (9.5f));
        g.setColour (mb::ui::faintText);
        g.drawText (hint, 18, 38, w - 60, 13, juce::Justification::centredLeft);

        // 双栏标题：无高亮，字号比列表条目略大
        g.setFont (mb::ui::font (12.5f, true));
        g.setColour (mb::ui::text);
        g.drawText (titleLeft,  16, 62, devSplit - 30, 20, juce::Justification::centredLeft);
        g.drawText (titleRight, devSplit + 14, 62, w - devSplit - 28, 20,
                    juce::Justification::centredLeft);

        // 分隔线：标题区之下 + 左右栏之间
        g.setColour (mb::ui::border);
        g.fillRect (14, 86, w - 28, 1);
        g.fillRect (devSplit, 86, 1, h - (showConfirmButtons ? 130 : 86));

        // 多选模式底部按钮：左「取消」右「确定」，关闭 × 仍可用（等同取消）
        if (showConfirmButtons)
        {
            g.setColour (mb::ui::border);
            g.fillRect (14, h - 44, w - 28, 1);
            const auto drawBtn = [&] (const juce::Rectangle<int>& r,
                                      const juce::String& t, bool primary)
            {
                const bool hot = r.contains (getMouseXYRelative());
                g.setColour (primary ? mb::ui::accent[0].withAlpha (hot ? 1.0f : 0.86f)
                                     : mb::ui::card.withAlpha (hot ? 0.98f : 0.80f));
                g.fillRoundedRectangle (r.toFloat(), 5.0f);
                g.setColour (primary ? mb::ui::accent[0].brighter (0.25f) : mb::ui::border);
                g.drawRoundedRectangle (r.toFloat(), 5.0f, 0.9f);
                g.setFont (mb::ui::font (11.0f, true));
                g.setColour (primary ? juce::Colours::white : mb::ui::text);
                g.drawText (t, r, juce::Justification::centred, false);
            };
            drawBtn (cancelRect(), mb::tr ("取消", "Cancel"), false);
            drawBtn (confirmRect(), mb::tr ("确定", "OK"), true);
        }

        // 右上角关闭 ×
        const auto cr = closeRect();
        g.setColour (cr.contains (getMouseXYRelative())
                         ? mb::ui::text : mb::ui::subText);
        g.drawLine (cr.getX() + 5, cr.getY() + 5, cr.getRight() - 5, cr.getBottom() - 5, 1.6f);
        g.drawLine (cr.getRight() - 5, cr.getY() + 5, cr.getX() + 5, cr.getBottom() - 5, 1.6f);
    }

    juce::Rectangle<int> closeRect() const
    {
        return { getWidth() - 40, 12, 26, 26 };
    }

    juce::Rectangle<int> cancelRect() const
        { return { getWidth() - 172, getHeight() - 34, 72, 24 }; }

    juce::Rectangle<int> confirmRect() const
        { return { getWidth() - 92, getHeight() - 34, 72, 24 }; }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        if (showConfirmButtons)
        {
            if (confirmRect().contains (ev.getPosition()))
            {
                if (onConfirmClicked) onConfirmClicked();
                return;
            }
            if (cancelRect().contains (ev.getPosition()))
            {
                if (onCancelClicked) onCancelClicked();
                return;
            }
        }
        if (closeRect().contains (ev.getPosition()) && onCloseClicked)
            onCloseClicked();
    }
};

//==============================================================================
// 「输入源 / 输出目标」选择浮层（替代旧的多级 PopupMenu）。
//
// 双栏卡片：
//   左栏 设备通道 —— 每台设备一组，只列 L / R 独立声道条目
//   （点选任一声道自动补选同设备另一声道，可单独取消任意一条；见 MainComponent::onTogglePort）
//   右栏 通道     —— CH 列表平铺
// 已连接的设备 / 通道排在各自栏的最上面，条目打勾、亮色显示：
//   点击未连接条目 = 接入；点击已连接条目 = 断开。
// （接入时自动继承本通道已有同类连接的增益，等效旧的「切换设备」）
// 点击卡片外任意位置或 × 关闭。
//==============================================================================
class SourcePickerOverlay : public juce::Component
{
public:
    // 点端口 / 通道行 = 切换连接：已连接 → 断开；未连接 → 接入（增益自动继承）
    // 通道行带声道侧 side（0 = L / 1 = R），接入后由 MainComponent 自动补选另一声道
    std::function<void (int ch, int port, bool stereo, bool input)> onTogglePort;
    std::function<void (int ch, int other, int side, bool input)> onToggleChannel;
    std::function<void()> onClose;

    SourcePickerOverlay (mb::RouteGraph& graph, mb::DeviceCatalog& catalog,
                         int ch, bool input)
        : graph_ (graph), catalog_ (catalog), ch_ (ch), input_ (input)
    {
        setAlwaysOnTop (true);
        setWantsKeyboardFocus (true);
        card_.onCloseClicked = [this] { if (onClose) onClose(); };
        vpDev_.setViewedComponent (&devList_, false);
        vpChan_.setViewedComponent (&chanList_, false);
        card_.addAndMakeVisible (vpDev_);
        card_.addAndMakeVisible (vpChan_);
        addAndMakeVisible (card_);
        refresh();
    }

    /** 图变化后重建列表（MainComponent 在每次增删连接后调用）。 */
    void refresh() { rebuild(); }

    void rebuild()
    {
        layoutCard();          // 先定卡片 / 双栏宽度，行布局依赖它
        buildDevRows();
        buildChanRows();
        orderFixed_ = true;    // 首次构建完成：之后的刷新不再把已连接条目重排到顶部
        layoutCard();          // 再按新内容高度重设列表尺寸
        devList_.repaint();
        chanList_.repaint();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.45f));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey) && onClose) { onClose(); return true; }
        return false;
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        if (onClose) onClose();          // 点卡片外 = 关闭
    }

    void resized() override { rebuild(); }

private:
    //==========================================================================
    // 列表行：绘制与命中全部在一张表里，避免管理一堆子组件。
    // x / w 为行在其所属栏内的横向范围（设备栏里区分左右子列）。
    //==========================================================================
    struct Row
    {
        enum Kind { Group, Port, Chan, Note } kind = Note;
        juce::String text;
        int a = -1;                       // 端口号 / 对端通道号
        int side = -1;                    // Chan 行：声道侧 0 = L / 1 = R
        int x = 12, w = 100;              // 行横向范围（栏内坐标）
        bool stereo = false, dim = false; // dim = 已连接
        int y = 0, h = 0;
    };

    class ListBody : public juce::Component,
                     public juce::SettableTooltipClient
    {
    public:
        ListBody (SourcePickerOverlay& o, int panel) : owner (o), panel_ (panel) {}
        void paint (juce::Graphics& g) override { owner.paintList (panel_, g); }
        void mouseMove (const juce::MouseEvent& e) override { owner.hoverList (panel_, e); }
        void mouseDown (const juce::MouseEvent& e) override { owner.clickList (panel_, e); }
        void mouseExit (const juce::MouseEvent&) override
        {
            int& hov = (panel_ == 0 ? owner.hoverDev_ : owner.hoverChan_);
            if (hov >= 0) { hov = -1; owner.repaint(); }
        }

    private:
        SourcePickerOverlay& owner;
        int panel_ = 0;                   // 0 = 设备栏，1 = 通道栏
    };

    //==========================================================================
    static constexpr int kRowH   = 24;    // 端口行 / 通道行
    static constexpr int kChanPairH = 21; // 通道互连：同通道 L/R 紧凑行高
    static constexpr int kChanGap   = 6;  // 通道互连：通道组之间的间距
    static constexpr int kGroupH = 20;    // 设备分组头
    static constexpr int kNoteH  = 20;

    //==========================================================================
    // 左栏：设备端口。立体声对（L+R）与单声道（L/R）全部靠左对齐、立体声对在前；
    // 已连接的设备排到最上面。
    //==========================================================================
    void buildDevRows()
    {
        rowsDev_.clear();
        int y = 2;
        const auto& ports = input_ ? catalog_.inputPorts() : catalog_.outputPorts();

        // 按端口顺序收集设备（去重），已连接（任一端口已用）的设备排前面
        struct Dev { juce::String name; bool connected = false; };
        std::vector<Dev> devs;
        for (int p = 0; p < static_cast<int> (ports.size()); ++p)
        {
            if (! catalog_.portVisible (input_, p)) continue;
            const auto& pi = ports[static_cast<size_t> (p)];
            auto it = std::find_if (devs.begin(), devs.end(),
                                    [&] (const Dev& d) { return d.name == pi.deviceName; });
            if (it == devs.end()) { devs.push_back ({ pi.deviceName, false }); it = devs.end() - 1; }
            if (input_ ? hasInputEdge (p) : hasOutputEdge (p)) it->connected = true;
        }
        // 已连接设备置顶只在浮层打开后的首次构建生效；之后原地刷新不再重排，
        // 避免「点选一条声道后设备突然跳到最上面」导致误点其他条目。
        if (! orderFixed_)
            std::stable_partition (devs.begin(), devs.end(),
                                   [] (const Dev& d) { return d.connected; });

        for (const auto& dev : devs)
        {
            const int devW = devList_.getWidth();
            Row g; g.kind = Row::Group;
            juce::String tag, devLabel = dev.name;
            int devIdx = -1;
            for (int p = 0; p < static_cast<int> (ports.size()); ++p)
                if (catalog_.portVisible (input_, p)
                    && ports[static_cast<size_t> (p)].deviceName == dev.name)
                {
                    devIdx = ports[static_cast<size_t> (p)].deviceIndex;
                    if (ports[static_cast<size_t> (p)].deviceLabel.isNotEmpty())
                        devLabel = ports[static_cast<size_t> (p)].deviceLabel;
                    break;
                }
            if (devIdx >= 0 && devIdx < static_cast<int> (catalog_.devices().size()))
                tag = catalog_.devices()[static_cast<size_t> (devIdx)].typeShortName;
            // 分组标题：备注名在左、设备名在右，驱动短标签仍缀在最后
            g.text = tag.isEmpty() ? devLabel : devLabel + mb::S("  ·  ") + tag;
            g.x = 12; g.w = devW - 24;
            add (rowsDev_, g, y, kGroupH);

            // 该设备的全部端口逐条列出（只保留 L / R 独立声道条目，无 L+R 合并项）
            for (int p = 0; p < static_cast<int> (ports.size()); ++p)
            {
                if (! catalog_.portVisible (input_, p)) continue;
                if (ports[static_cast<size_t> (p)].deviceName != dev.name) continue;
                Row m; m.kind = Row::Port; m.a = p;
                m.dim = input_ ? hasInputEdge (p) : hasOutputEdge (p);
                m.x = 12; m.w = devW - 24;
                m.text = shortPortName (ports[static_cast<size_t> (p)]);
                add (rowsDev_, m, y, kRowH);
            }
            y += 2;                            // 设备组间距
        }

        if (devs.empty())
            note (rowsDev_, y, mb::tr ("（没有可选的设备端口）", "(no device ports)"));
        contentHDev_ = y + 4;
    }

    //==========================================================================
    // 右栏：通道互连。已互连的通道排前面。
    //==========================================================================
    void buildChanRows()
    {
        rowsChan_.clear();
        int y = 2;

        std::vector<int> chans;
        for (int c = 0; c < graph_.channelCount(); ++c)
            if (c != ch_) chans.push_back (c);
        if (! orderFixed_)                   // 与设备栏一致：置顶只在首次构建生效
            std::stable_partition (chans.begin(), chans.end(),
                                   [&] (int c) { return input_ ? hasCCEdge (c, ch_)
                                                               : hasCCEdge (ch_, c); });

        int prevChan = -1;
        for (int c : chans)
        {
            // 每通道列 L / R 两条目：点选任一声道自动补选另一条，可单独取消任一条
            // （side 语义见 RouteGraph::channelEdgeSide；-1 双声道 = 两侧都勾选）。
            // 同一通道的 L/R 紧贴成组，通道之间留明显间距，视觉上一眼区分。
            const int st = graph_.channelEdgeSide (input_ ? c : ch_, input_ ? ch_ : c);
            if (prevChan >= 0)
                y += kChanGap;                        // 通道组间距
            for (int s = 0; s < 2; ++s)
            {
                Row r; r.kind = Row::Chan; r.a = c; r.side = s;
                r.dim = st == -1 || st == s;
                r.text = mb::S("CH") + juce::String (c + 1) + mb::S("    ")
                       + mb::channelDisplayName (c)
                       + (s == 0 ? mb::S("  · L") : mb::S("  · R"));
                add (rowsChan_, r, y, kChanPairH);
            }
            prevChan = c;
        }
        if (chans.empty())
            note (rowsChan_, y, mb::tr ("（没有其他通道）", "(no other channels)"));

        contentHChan_ = y + 4;
    }

    static void add (std::vector<Row>& rows, Row& r, int& y, int h)
    { r.y = y; r.h = h; rows.push_back (r); y += h; }

    static void note (std::vector<Row>& rows, int& y, const juce::String& t)
    {
        Row r; r.kind = Row::Note; r.text = t;
        r.x = 12; r.w = 400;
        add (rows, r, y, kNoteH);
    }

    //==========================================================================
    void paintList (int panel, juce::Graphics& g)
    {
        g.fillAll (mb::ui::background);
        const auto& rows = panel == 0 ? rowsDev_ : rowsChan_;
        const int& hover = panel == 0 ? hoverDev_ : hoverChan_;

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            const int idx = static_cast<int> (i);
            const auto b = juce::Rectangle<int> (r.x, r.y, r.w, r.h).toFloat();

            switch (r.kind)
            {
                case Row::Group:
                    g.setFont (mb::ui::font (10.0f, true));
                    g.setColour (mb::ui::faintText);
                    // 备注 + 原名可能较长，缩放显示而不截断掉右侧设备名
                    g.drawFittedText (r.text, b.toNearestInt(),
                                      juce::Justification::centredLeft, 1, 0.65f);
                    break;

                case Row::Note:
                    g.setFont (mb::ui::font (9.5f));
                    g.setColour (mb::ui::faintText);
                    g.drawText (r.text, b.toNearestInt(), juce::Justification::centredLeft);
                    break;

                case Row::Port:
                case Row::Chan:
                {
                    if (idx == hover)
                    {
                        g.setColour (mb::ui::accent[0].withAlpha (0.14f));
                        g.fillRoundedRectangle (b, 5.0f);
                    }
                    const bool hasBar = r.kind == Row::Chan;
                    if (hasBar)                       // 通道色条：同通道 L/R 一色，与其他通道明显区分
                    {
                        g.setColour (mb::ui::stripColour (r.a).withAlpha (r.dim ? 0.9f : 0.55f));
                        g.fillRoundedRectangle (b.getX() + 1.0f, b.getY() + 3.0f,
                                                2.5f, b.getHeight() - 6.0f, 1.2f);
                    }
                    const float tx = b.getX() + (hasBar ? 5.5f : 2.0f);   // 右移避开色条
                    if (r.dim)                        // 已连接：对勾
                    {
                        g.setColour (mb::ui::accent[0]);
                        g.drawLine (tx, b.getCentreY(), tx + 2.5f, b.getCentreY() + 3.0f, 1.6f);
                        g.drawLine (tx + 2.5f, b.getCentreY() + 3.0f,
                                    tx + 8.0f, b.getCentreY() - 3.5f, 1.6f);
                    }
                    else                              // 未连接：空心点
                    {
                        g.setColour (mb::ui::faintText);
                        g.drawEllipse (tx, b.getCentreY() - 3.0f, 6.0f, 6.0f, 1.2f);
                    }
                    g.setFont (mb::ui::font (11.5f, r.stereo || r.dim));
                    g.setColour (r.dim ? mb::ui::accent[0] : mb::ui::text);
                    g.drawText (r.text, b.reduced (14.0f, 0.0f).toNearestInt(),
                                juce::Justification::centredLeft, true);
                    break;
                }
            }
        }
    }

    /** 悬停提示：分组/通道行 = 完整文字；端口行 = 「备注 (完整原名)」，
        避免行内长名被截断后看不到备注与原名的对应关系。 */
    juce::String tooltipFor (int panel, int idx) const
    {
        if (idx < 0) return {};
        const auto& rows = panel == 0 ? rowsDev_ : rowsChan_;
        if (idx >= static_cast<int> (rows.size())) return {};
        const auto& r = rows[static_cast<size_t> (idx)];

        if (r.kind == Row::Group || r.kind == Row::Chan || r.kind == Row::Note)
            return r.kind == Row::Note ? juce::String() : r.text;
        if (r.kind != Row::Port) return {};

        const auto& ports = input_ ? catalog_.inputPorts() : catalog_.outputPorts();
        if (r.a < 0 || r.a >= static_cast<int> (ports.size())) return r.text;
        const auto& p = ports[static_cast<size_t> (r.a)];
        juce::String orig = p.name;                       // 完整原名（设备 · 通道）
        if (r.stereo && r.a + 1 < static_cast<int> (ports.size()))
            orig += mb::S(" + ") + ports[static_cast<size_t> (r.a + 1)].name;
        const auto cn = catalog_.channelNoteForPort (p);
        return cn.isNotEmpty() ? cn + mb::S(" (") + orig + mb::S(")") : orig;
    }

    void hoverList (int panel, const juce::MouseEvent& e)
    {
        const int hit = rowAt (panel, e.x, e.y);
        int& hov = panel == 0 ? hoverDev_ : hoverChan_;
        if (hit != hov) { hov = hit; repaint(); }
        auto* body = panel == 0 ? static_cast<juce::SettableTooltipClient*> (&devList_)
                                : static_cast<juce::SettableTooltipClient*> (&chanList_);
        body->setTooltip (tooltipFor (panel, hit));
    }

    void clickList (int panel, const juce::MouseEvent& e)
    {
        const int idx = rowAt (panel, e.x, e.y);
        if (idx < 0) return;
        const auto& r = (panel == 0 ? rowsDev_ : rowsChan_)[static_cast<size_t> (idx)];

        switch (r.kind)
        {
            case Row::Port:
                if (onTogglePort) onTogglePort (ch_, r.a, r.stereo, input_);
                return;

            case Row::Chan:
                if (onToggleChannel) onToggleChannel (ch_, r.a, r.side, input_);
                return;

            default: return;
        }
    }

    int rowAt (int panel, int x, int y) const
    {
        const auto& rows = panel == 0 ? rowsDev_ : rowsChan_;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            if (y >= r.y && y < r.y + r.h && x >= r.x && x < r.x + r.w)
                return static_cast<int> (i);
        }
        return -1;
    }

    //==========================================================================
    /** 端口短名：通道对备注优先（备注替代通道名），
        否则剥掉设备名前缀（L / R / Input 3 …）。 */
    juce::String shortPortName (const mb::PortInfo& p) const
    {
        const auto cn = catalog_.channelNoteForPort (p);
        if (cn.isNotEmpty()) return cn;
        if (p.channelName.isNotEmpty()) return p.channelName;
        juce::String s = p.name;
        if (p.deviceName.isNotEmpty() && s.startsWith (p.deviceName))
            s = s.substring (p.deviceName.length());
        s = s.trimCharactersAtStart (mb::S(" \u00B7\u3000 "));
        return s.isEmpty() ? p.name : s;
    }

    bool hasInputEdge (int port) const
    {
        for (const auto& e : graph_.edges())
            if (e.kind == mb::EdgeKind::InputToChannel && e.src == port && e.dst == ch_)
                return true;
        return false;
    }

    bool hasOutputEdge (int port) const
    {
        for (const auto& e : graph_.edges())
            if (e.kind == mb::EdgeKind::ChannelToOutput && e.src == ch_ && e.dst == port)
                return true;
        return false;
    }

    bool hasCCEdge (int src, int dst) const
    {
        for (const auto& e : graph_.edges())
            if (e.kind == mb::EdgeKind::ChannelToChannel && e.src == src && e.dst == dst)
                return true;
        return false;
    }

    //==========================================================================
    void layoutCard()
    {
        const int pw = getWidth(), ph = getHeight();
        const int w = juce::jlimit (480, 640, juce::jmin (600, pw - 60));
        const int h = juce::jlimit (300, 560, juce::jmin (540, ph - 80));
        card_.setBounds ((pw - w) / 2, (ph - h) / 2, w, h);
        card_.devSplit = juce::jmax (260, static_cast<int> (w * 0.62f));

        card_.title = input_ ? mb::tr ("输入源", "Input Source")
                             : mb::tr ("输出目标", "Output Destination");
        card_.channel = mb::S(" (") + mb::S("CH") + juce::String (ch_ + 1) + mb::S("  ")
                      + mb::channelDisplayName (ch_) + mb::S(")");
        card_.hint = mb::tr ("点击条目接入 · 已连接条目再次点击即断开",
                             "Click to connect · click a connected item again to disconnect");
        card_.titleLeft  = input_ ? mb::tr ("接入输入设备", "Input devices")
                                  : mb::tr ("接入输出设备", "Output devices");
        card_.titleRight = input_ ? mb::tr ("接收其他通道", "Receive from channel")
                                  : mb::tr ("送进其他通道", "Feed another channel");

        const int listY = 90, listH = h - 92;
        vpDev_.setBounds  (2, listY, card_.devSplit - 2, listH);
        vpChan_.setBounds (card_.devSplit + 1, listY, w - card_.devSplit - 3, listH);
        devList_.setSize  (card_.devSplit - 2, juce::jmax (contentHDev_, listH));
        chanList_.setSize (w - card_.devSplit - 3, juce::jmax (contentHChan_, listH));
        devList_.repaint();
        chanList_.repaint();
    }

    mb::RouteGraph& graph_;
    mb::DeviceCatalog& catalog_;
    int ch_ = 0;
    bool input_ = true;

    PickerCard card_;
    juce::Viewport vpDev_, vpChan_;
    ListBody devList_  { *this, 0 };
    ListBody chanList_ { *this, 1 };
    std::vector<Row> rowsDev_, rowsChan_;
    int contentHDev_ = 0, contentHChan_ = 0;
    int hoverDev_ = -1, hoverChan_ = -1;
    bool orderFixed_ = false;          // 首次构建后锁定排序（刷新不重排）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SourcePickerOverlay)
};

//==============================================================================
// 「添加插件」选择浮层：与 SourcePickerOverlay 同款卡片样式。
//   左栏 自带插件（内置效果链模块）
//   右栏 第三方插件 · VST3（按厂商分组平铺，无子菜单）
// 前置小圆点 = 勾选态：已添加实心强调色，未添加空心；点击条目切换添加/移除，
// 操作后列表原地刷新（不关闭浮层）。onPick(id)：
// 1..kBuiltInPluginCount = 自带模块，1000+i = 目录索引。
// 点击卡片外任意位置、× 或 Esc 关闭。
//==============================================================================
class PluginPickerOverlay : public juce::Component
{
public:
    std::function<void (int id)> onPick;
    std::function<void()> onClose;
    /** 多选模式（节点视图空白区 +插件）：勾选不立即生效，点「确定」才按勾选顺序添加。 */
    std::function<void (const std::vector<int>& ids)> onMultiConfirm;

    PluginPickerOverlay (mb::AudioEngine& engine, int ch, bool multi = false)
        : engine_ (engine), ch_ (ch), multi_ (multi)
    {
        setAlwaysOnTop (true);
        setWantsKeyboardFocus (true);
        card_.showConfirmButtons = multi;
        card_.onCloseClicked = [this] { if (onClose) onClose(); };
        if (multi)
        {
            card_.onCancelClicked = [this] { if (onClose) onClose(); };
            card_.onConfirmClicked = [this]
            {
                if (onMultiConfirm)
                {
                    std::vector<int> ids;
                    ids.reserve (static_cast<size_t> (selected_.size()));
                    for (int v : selected_) ids.push_back (v);
                    onMultiConfirm (ids);
                }
                if (onClose) onClose();
            };
        }
        vpLeft_.setViewedComponent (&leftList_, false);
        vpRight_.setViewedComponent (&rightList_, false);
        card_.addAndMakeVisible (vpLeft_);
        card_.addAndMakeVisible (vpRight_);
        addAndMakeVisible (card_);
        rebuild();
    }

    void rebuild()
    {
        layoutCard();
        buildLeft();
        buildRight();
        layoutCard();                 // 列表高度更新后重排滚动区
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.45f));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey) && onClose) { onClose(); return true; }
        return false;
    }

    void mouseDown (const juce::MouseEvent&) override { if (onClose) onClose(); }

    void resized() override { layoutCard(); }

private:
    //==========================================================================
    struct Row
    {
        enum Kind { Group, Item, Note } kind = Note;
        juce::String text;
        int a = -1;                       // 自带 id / 1000+目录索引
        bool checked = false;             // 已添加（小圆点实心勾选）
        int x = 12, w = 100, y = 0, h = 0;
    };

    class ListBody : public juce::Component
    {
    public:
        ListBody (PluginPickerOverlay& o, int panel) : owner (o), panel_ (panel) {}
        void paint (juce::Graphics& g) override { owner.paintList (panel_, g); }
        void mouseMove (const juce::MouseEvent& e) override { owner.hoverList (panel_, e.x, e.y); }
        void mouseDown (const juce::MouseEvent& e) override { owner.clickList (panel_, e.x, e.y); }
        void mouseExit (const juce::MouseEvent&) override
        {
            int& hov = (panel_ == 0 ? owner.hoverLeft_ : owner.hoverRight_);
            if (hov >= 0) { hov = -1; owner.repaint(); }
        }

    private:
        PluginPickerOverlay& owner;
        int panel_ = 0;                   // 0 = 自带插件栏，1 = 第三方栏
    };

    static constexpr int kRowH = 24, kGroupH = 20, kNoteH = 20;

    /** 条目名实测宽度（11.5px 字体），用于自适应格宽。 */
    static int textW (const juce::String& t)
    {
        return juce::GlyphArrangement::getStringWidth (mb::ui::font (11.5f), t);
    }

    /** 自适应格宽：名字多宽格子就多宽，但不小于 minCell（保证常规列数）、
        不超过 avail（超长省略号）。x 为当前行已占宽度；放不下时自动换行。 */
    static int cellW (const juce::String& t, int avail, int minCell)
    {
        return juce::jmin (avail, juce::jmax (minCell, 14 + textW (t) + 10));
    }

    //==========================================================================
    // 左栏：自带插件（内置效果链模块），自适应两列；已添加实心勾选
    //==========================================================================
    void buildLeft()
    {
        rowsLeft_.clear();
        const auto& bank = engine_.pluginBank();
        const int avail = juce::jmax (80, leftList_.getWidth()) - 24;
        const int minCell = juce::jmax (90, (avail - 6) / 2);   // 常规两列（扣除列间距）
        int x = 0, y = 2;
        for (const auto& p : mb::builtInPluginTable)
        {
            Row r; r.kind = Row::Item; r.a = p.id;
            r.text = mb::builtInPluginName (p.id);
            if (multi_) r.checked = selected_.contains (p.id);
            else
                for (int i = 0; i < bank.pluginCount (ch_); ++i)
                    if (bank.pluginBuiltInId (ch_, i) == p.id) { r.checked = true; break; }
            r.w = cellW (r.text, avail, minCell);
            if (x > 0 && x + r.w > avail) { x = 0; y += kRowH; }
            r.x = 12 + x; r.y = y; r.h = kRowH;
            rowsLeft_.push_back (r);
            x += r.w + 6;
        }
        if (x != 0) y += kRowH;
        contentLeft_ = y + 4;
    }

    //==========================================================================
    // 右栏：第三方 VST3 按厂商分组，组内自适应流式平铺（常规四列，长名加宽）
    //==========================================================================
    void buildRight()
    {
        rowsRight_.clear();
        int y = 2;
        const auto& cat = engine_.pluginCatalog();
        const int colW = juce::jmax (80, rightList_.getWidth());
        if (cat.count() == 0)
        {
            note (rowsRight_, y,
                  mb::tr ("（插件库为空：请在设置 · 插件库 扫描）",
                          "(library empty: scan in Settings · Plugins)"), colW);
            contentRight_ = y + 4;
            return;
        }
        juce::StringArray vendors;
        std::vector<std::vector<int>> per;
        for (int i = 0; i < cat.count(); ++i)
        {
            const auto d = cat.get (i);
            const auto vendor = d.manufacturerName.isNotEmpty() ? d.manufacturerName
                                                                : mb::tr ("其他", "Other");
            int vi = vendors.indexOf (vendor);
            if (vi < 0)
            {
                vendors.add (vendor);
                per.emplace_back();
                vi = static_cast<int> (per.size()) - 1;
            }
            per[static_cast<size_t> (vi)].push_back (i);
        }
        const int avail = juce::jmax (80, colW) - 24;
        const int minCell = juce::jmax (90, (avail - 18) / 4); // 常规四列（扣除列间距）
        for (int v = 0; v < vendors.size(); ++v)
        {
            Row g; g.kind = Row::Group; g.text = vendors[static_cast<size_t> (v)];
            g.x = 12; g.w = colW - 24;
            add (rowsRight_, g, y, kGroupH);
            int x = 0;
            for (int i : per[static_cast<size_t> (v)])
            {
                Row r; r.kind = Row::Item; r.a = 1000 + i;
                r.text = cat.get (i).name;
                if (multi_) r.checked = selected_.contains (1000 + i);
                else
                {
                    for (int p = 0; p < engine_.pluginBank().pluginCount (ch_); ++p)
                        if (engine_.pluginBank().pluginBuiltInId (ch_, p) <= 0
                            && engine_.pluginBank().pluginIdentifier (ch_, p) == cat.get (i).fileOrIdentifier)
                        { r.checked = true; break; }
                }
                r.w = cellW (r.text, avail, minCell);
                if (x > 0 && x + r.w > avail) { x = 0; y += kRowH; }
                r.x = 12 + x; r.y = y; r.h = kRowH;
                rowsRight_.push_back (r);
                x += r.w + 6;
            }
            if (x != 0) y += kRowH;
            y += 2;
        }
        contentRight_ = y + 4;
    }

    static void add (std::vector<Row>& rows, Row& r, int& y, int h)
    {
        r.y = y; r.h = h;
        rows.push_back (r);
        y += h;
    }

    static void note (std::vector<Row>& rows, int& y, const juce::String& t, int colW)
    {
        Row r; r.kind = Row::Note; r.text = t; r.x = 12; r.w = colW - 24;
        add (rows, r, y, kNoteH);
    }

    //==========================================================================
    void paintList (int panel, juce::Graphics& g)
    {
        g.fillAll (mb::ui::background);
        const auto& rows = panel == 0 ? rowsLeft_ : rowsRight_;
        const int hover = panel == 0 ? hoverLeft_ : hoverRight_;

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            const auto b = juce::Rectangle<int> (r.x, r.y, r.w, r.h).toFloat();

            if (r.kind == Row::Group)
            {
                g.setFont (mb::ui::font (10.0f, true));
                g.setColour (mb::ui::faintText);
                g.drawFittedText (r.text, b.toNearestInt(), juce::Justification::centredLeft, 1, 0.65f);
                continue;
            }
            if (r.kind == Row::Note)
            {
                g.setFont (mb::ui::font (9.5f));
                g.setColour (mb::ui::faintText);
                g.drawText (r.text, b.toNearestInt(), juce::Justification::centredLeft);
                continue;
            }

            if (static_cast<int> (i) == hover)
            {
                g.setColour (mb::ui::accent[0].withAlpha (0.14f));
                g.fillRoundedRectangle (b, 5.0f);
            }
            // 前置小圆点：已添加 = 实心强调色勾选；未添加 = 空心灰点
            const float cx = b.getX() + 5.0f;
            g.setColour (r.checked ? mb::ui::accent[0] : mb::ui::faintText);
            if (r.checked) g.fillEllipse (cx, b.getCentreY() - 2.5f, 5.0f, 5.0f);
            else           g.drawEllipse (cx, b.getCentreY() - 2.5f, 5.0f, 5.0f, 1.2f);

            g.setFont (mb::ui::font (11.5f));
            g.setColour (r.checked ? mb::ui::accent[0] : mb::ui::text);
            g.drawText (r.text, b.reduced (14.0f, 0.0f).toNearestInt(),
                        juce::Justification::centredLeft, true);
        }
    }

    void hoverList (int panel, int x, int y)
    {
        const int hit = rowAt (panel, x, y);
        int& hov = panel == 0 ? hoverLeft_ : hoverRight_;
        if (hit != hov) { hov = hit; repaint(); }
        auto* body = panel == 0 ? static_cast<juce::Component*> (&leftList_)
                                : static_cast<juce::Component*> (&rightList_);
        body->setMouseCursor (hit >= 0 ? juce::MouseCursor::PointingHandCursor
                                       : juce::MouseCursor::NormalCursor);
    }

    void clickList (int panel, int x, int y)
    {
        const int idx = rowAt (panel, x, y);
        if (idx < 0) return;
        const auto& r = (panel == 0 ? rowsLeft_ : rowsRight_)[static_cast<size_t> (idx)];
        if (r.kind != Row::Item) return;
        if (x < r.x || x >= r.x + r.w) return;
        if (multi_)
        {
            // 多选：勾选态只存在浮层里，点「确定」才真正添加；「取消」全部丢弃
            if (selected_.contains (r.a)) selected_.removeFirstMatchingValue (r.a);
            else                          selected_.add (r.a);
            rebuild();
            return;
        }
        if (onPick) onPick (r.a);
    }

    /** 网格命中：多列共享同一行 y，必须 x、y 都落在条目范围内才算命中。 */
    int rowAt (int panel, int x, int y) const
    {
        const auto& rows = panel == 0 ? rowsLeft_ : rowsRight_;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            if (y >= r.y && y < r.y + r.h && x >= r.x && x < r.x + r.w)
                return static_cast<int> (i);
        }
        return -1;
    }

    //==========================================================================
    void layoutCard()
    {
        const int pw = getWidth(), ph = getHeight();
        const int w = juce::jlimit (500, 680, juce::jmin (680, pw - 60));
        const int h = juce::jlimit (300, 560, juce::jmin (540, ph - 80));
        card_.setBounds ((pw - w) / 2, (ph - h) / 2, w, h);
        card_.devSplit = juce::jmax (260, static_cast<int> (w * 0.42f));

        card_.title = multi_ ? mb::tr ("添加插件 · 多选", "Add Plugins · multi-select")
                             : mb::tr ("添加插件", "Add Plugin");
        card_.channel = multi_ ? juce::String()
                               : mb::S(" (") + mb::S("CH") + juce::String (ch_ + 1) + mb::S("  ")
                                 + mb::channelDisplayName (ch_) + mb::S(")");
        card_.hint = multi_
            ? mb::tr ("左：内置效果 · 右：VST3 插件 · 勾选后点「确定」在光标处添加",
                      "Left: built-in · right: VST3 · check items, then OK to add at cursor")
            : mb::tr ("左：内置效果 · 右：VST3 插件 · 点击条目添加到通道",
                      "Left: built-in · right: VST3 · click an item to add");
        card_.titleLeft  = mb::tr ("内置插件", "Built-in plugins");
        card_.titleRight = mb::tr ("第三方插件 · VST3", "Third-party plugins · VST3");

        const int listY = 90, listH = h - (multi_ ? 138 : 92);
        vpLeft_.setBounds  (2, listY, card_.devSplit - 2, listH);
        vpRight_.setBounds (card_.devSplit + 1, listY, w - card_.devSplit - 3, listH);
        leftList_.setSize  (card_.devSplit - 2, juce::jmax (contentLeft_, listH));
        rightList_.setSize (w - card_.devSplit - 3, juce::jmax (contentRight_, listH));
        leftList_.repaint();
        rightList_.repaint();
    }

    mb::AudioEngine& engine_;
    int ch_ = 0;
    bool multi_ = false;
    juce::Array<int> selected_;            // 多选勾选（按勾选顺序，即最终添加顺序）

    PickerCard card_;
    juce::Viewport vpLeft_, vpRight_;
    ListBody leftList_  { *this, 0 };
    ListBody rightList_ { *this, 1 };
    std::vector<Row> rowsLeft_, rowsRight_;
    int contentLeft_ = 0, contentRight_ = 0;
    int hoverLeft_ = -1, hoverRight_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPickerOverlay)
};
