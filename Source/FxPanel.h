#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "ChannelFx.h"
#include "I18n.h"
#include "PanelChrome.h"
#include "Theme.h"

//==============================================================================
// 通道效果器面板（浮层）—— 自带插件统一 UI
//
// · 每个内置效果 = 一个模块组：模块开关（EQ 为固定标题）+ 纵向推子列 +
//   参数名标签；推子上方实时显示数值，双击推子回到默认值。
// · 整链模式（fxSlot = -1）：8 个模块按信号链顺序排两行；
//   单效果模式：只显示当前模块，居中。
// · 所有控件直接写入 AudioEngine 的 ChannelFxBank，音频侧按快照换系数。
//==============================================================================
class FxPanel : public FloatingCardPanel
{
public:
    std::function<void()> onChanged;

    /** fxSlot：-1 = 整条链；1..8 = 单个内置效果（对应 builtInPluginTable 的 id）。 */
    FxPanel (mb::AudioEngine& engine, int channelIndex, int fxSlot = -1)
        : engine_ (engine), ch_ (channelIndex), fxSlot_ (fxSlot)
    {
        // 图标长度运行时查询：换图标后即使本翻译单元未重编也不会越界读资源。
        int iconSize = 0;
        const char* iconData = BinaryData::getNamedResource ("app_icon_png", iconSize);
        logo_ = iconData != nullptr && iconSize > 0
              ? juce::ImageCache::getFromMemory (iconData, iconSize)
              : juce::Image();
        faderLf_.railRightInset = 16.0f;      // 导轨靠右，左侧留给参考值刻度，右侧留 3px
        buildUi();
    }

    /** 主题切换：自绘推子卡实时读主题色，构造期 setColour 的控件在此统一刷新。 */
    void applyTheme() { syncThemeColours(); repaint(); }

    /** 语言切换：刷新构造期缓存的模块名 / 参数标签 / 按钮文案。
        推子值来自引擎，重建不涉及，参数不会丢。 */
    void applyLanguage()
    {
        for (auto& f : textRefreshers_) f();
        for (auto& m : mods_)
        {
            const auto name = mb::builtInPluginName (m.fxId);
            if (m.toggle != nullptr) m.toggle->setButtonText (name);
            if (m.header != nullptr) m.header->setText (name, juce::dontSendNotification);
        }
        bypassBtn_.setButtonText (mb::tr ("旁通", "Bypass"));
        resetBtn_.setButtonText (fxSlot_ < 0 ? mb::tr ("全部重置", "Reset all")
                                             : mb::tr ("重置", "Reset"));
        repaint();
    }

    /** 单效果模式的标题（与「自带插件」菜单同名）。 */
    juce::String fxName() const
    {
        if (fxSlot_ < 0) return mb::tr ("内置效果链", "Built-in FX");
        return mb::builtInPluginName (fxSlot_);
    }

    /** 单效果模式的说明文字。 */
    juce::String fxHint() const
    {
        switch (fxSlot_)
        {
            case 1: return mb::tr ("低于阈值的输入被静音 · 适合消除底噪与房间声",
                                   "Mutes input below threshold · removes noise & room tone");
            case 2: return mb::tr ("切除低频隆隆声 / 喷麦 · 人声常用 80–120 Hz",
                                   "Cuts rumble & plosives · 80–120 Hz works well for voice");
            case 3: return mb::tr ("低频 @180 Hz · 中频中心可调 · 高频 @5.2 kHz",
                                   "Low @180 Hz · adjustable mid centre · High @5.2 kHz");
            case 4: return mb::tr ("压缩动态范围 · 慢启动保留瞬态，快启动更平稳",
                                   "Evens out dynamics · slow attack keeps transients");
            case 5: return mb::tr ("湿度决定效果量，空间大小决定尾巴长短",
                                   "Mix sets the amount, room size sets the tail");
            case 6: return mb::tr ("输出上限，防止爆音 · 一般留 0 到 -1 dB",
                                   "Output ceiling to prevent clipping · keep near 0 to -1 dB");
            case 7: return mb::tr ("延迟对齐 · 默认无回声（反馈 0）；拉高反馈产生回音与空间感",
                                   "Delay · no echo by default (feedback 0); raise feedback for echoes / space");
            case 8: return mb::tr ("tanh 饱和激励 · 增加密度与温暖感，适量即可",
                                   "tanh saturation · adds density & warmth, use in moderation");
            case 10: return mb::tr ("源通道出声时自动压低本通道（音乐垫在人声之下） · 左栏选择源通道",
                                    "Ducks this channel while the source speaks (music bed under voice) · pick source in the left rail");
            default: break;
        }
        return mb::tr ("信号链：门 → 高通 → EQ → 压缩 → 驱动 → 延迟 → 混响 → 限制器 · 双击推子回默认值",
                       "Chain: Gate > HPF > EQ > Comp > Drive > Delay > Reverb > Limiter · double-click a fader to reset");
    }

    void paint (juce::Graphics& g) override
    {
        if (themeDark_ != mb::ui::isDarkTheme())
            syncThemeColours();

        // 通道条上点击插件圆点旁通时，面板按钮状态同步（面板可能正开着）
        if (fxSlot_ >= 0)
        {
            const bool byp = currentBypass();
            if (bypassBtn_.getToggleState() != byp)
                bypassBtn_.setToggleState (byp, juce::dontSendNotification);
        }

        // 不铺遮罩：卡片以外的主窗口始终保持可见且可点击（与折叠态一致）

        const auto card = cardBounds();
        g.setColour (mb::ui::sectionBg);
        g.fillRoundedRectangle (card, 10.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (card, 10.0f, 1.2f);

        // 微音桥 logo（所有效果面板统一显示）
        const bool hasLogo = logo_.isValid();
        if (hasLogo)
        {
            const auto lr = juce::Rectangle<float> (card.getX() + 16, card.getY() + 7, 22, 22);
            g.drawImage (logo_, lr);
        }

        g.setFont (mb::ui::font (14.0f, true));
        g.setColour (mb::ui::text);
        g.drawText (mb::channelDisplayName (ch_) + mb::S("  ·  ") + fxName(),
                    card.toNearestInt().withHeight (34)
                        .withTrimmedLeft (18 + (hasLogo ? 26 : 0)),
                    juce::Justification::centredLeft, false);

        if (minimised()) return;               // 收起态：只留标题条

        g.setFont (mb::ui::font (8.5f));
        g.setColour (mb::ui::subText);
        g.drawText (fxHint(),
                    card.toNearestInt().withY (card.toNearestInt().getY() + 24)
                        .withHeight (16).withTrimmedLeft (18),
                    juce::Justification::centredLeft, false);

        // 单效果模式：卡片最左侧辟出一条独立操作栏（从标题之下一直到卡片底部），
        // 「旁通」贴左上角、「重置」贴左下角，整链模式不显示。
        if (const int rail = leftRailWidth(); rail > 0)
        {
            const float railTop = card.getY() + static_cast<float> (kRailTop);
            const float railH = juce::jmax (10.0f,
                                            card.getHeight() - kRailTop - kRailBottom);
            const auto railRect = juce::Rectangle<float> (
                card.getX() + 4.0f, railTop,
                static_cast<float> (rail - 8), railH);
            g.setColour (mb::ui::sectionBg.withAlpha (0.55f));
            g.fillRoundedRectangle (railRect, 6.0f);
            g.setColour (mb::ui::border.withAlpha (0.85f));
            g.fillRect (card.getX() + static_cast<float> (rail) - 4.0f,
                        railTop, 1.0f, railH);
        }

        drawFaderCards (g);
    }

    void resized() override
    {
        const auto cardF = cardBounds();
        const auto card = cardF.toNearestInt();

        layoutChrome (cardF, minimised());
        bypassBtn_.setVisible (fxSlot_ >= 0);       // 整链模式不显示单个插件的旁通
        if (minimised()) return;

        const int rail = leftRailWidth();           // 左侧操作栏宽度（仅单效果模式）
        // 模块按行排布：整链模式固定两行（链序前半 / 后半），单效果一行居中
        std::vector<std::vector<int>> rows;
        if (fxSlot_ < 0 && mods_.size() > 4)
        {
            const int half = static_cast<int> ((mods_.size() + 1) / 2);
            rows.push_back ({});
            rows.push_back ({});
            int acc = 0;
            for (int i = 0; i < static_cast<int> (mods_.size()); ++i)
            {
                if (i >= half) { rows[1].push_back (i); continue; }
                acc += moduleWidth (i);
                if (acc > card.getWidth() - 24 - rail && ! rows[0].empty())
                    rows[1].push_back (i);
                else
                    rows[0].push_back (i);
            }
            if (rows[0].empty()) { rows[0].swap (rows[1]); rows.pop_back(); }
        }
        else
        {
            rows.emplace_back();
            for (int i = 0; i < static_cast<int> (mods_.size()); ++i)
                rows[0].push_back (i);
        }

        int y = card.getY() + 50;
        // 单效果模式：推子高度按需填满卡片（预留底部「重置」行），不再留大片空白；
        // 整链模式保持固定高度（两行布局）。
        const int faderH = (fxSlot_ >= 0)
            ? juce::jmax (150, (card.getBottom() - kBottomReserve) - (y + 22))
            : kFaderH;
        for (const auto& row : rows)
        {
            int total = 0;
            for (int i : row) total += moduleWidth (i);
            total += juce::jmax (0, static_cast<int> (row.size()) - 1) * kModuleGap;

            // 让出左侧操作栏后再居中，推子不再与「旁通」叠在同一行
            int x = card.getX() + rail
                  + juce::jmax (16, (card.getWidth() - rail - total) / 2);
            for (int i : row)
            {
                auto& m = mods_[static_cast<size_t> (i)];
                const int w = moduleWidth (i);
                if (m.toggle != nullptr) m.toggle->setBounds (x, y, w, 20);
                else if (m.header != nullptr) m.header->setBounds (x, y, w, 20);

                const int fy = y + 22;
                int fx = x;
                for (auto& f : m.faders)
                {
                    f.s->setBounds (fx, fy, kFaderW, faderH);
                    if (f.l != nullptr) f.l->setBounds (fx, fy + faderH + 2, kFaderW, 13);
                    fx += kFaderW + kFaderGap;
                }
                x += w + kModuleGap;
            }
            y += 22 + 2 + faderH + 15 + 12;
        }

        // 单效果模式：左侧操作栏内「旁通」收进左上角、「重置」收进左下角（卡片真·底部），
        // 二者统一尺寸（以旁通为准）；整链模式无旁通，仅保留底部「全部重置」。
        if (fxSlot_ >= 0)
        {
            const int bw = kLeftRailW - 16;              // 旁通 / 重置 同宽同高
            bypassBtn_.setBounds (card.getX() + 8,
                                  card.getY() + kRailTop + 6,  // 左上角：紧贴操作栏顶部
                                  bw, 24);
            resetBtn_.setBounds (card.getX() + 8,
                                 card.getBottom() - kRailBottom - 6 - 24,  // 左下角：卡片真·底部
                                 bw, 24);
            // 闪避：源通道循环按钮放左栏中部（旁通之下、重置之上）
            if (duckSrcBtn_ != nullptr && duckSrcBtn_->isVisible())
                duckSrcBtn_->setBounds (card.getX() + 8, bypassBtn_.getBottom() + 8, bw, 24);
        }
        else
        {
            resetBtn_.setBounds (card.getX() + 16, card.getBottom() - 38, 92, 26);
        }
    }

private:
    static constexpr int kFaderW    = 54;    // 单列纵向推子宽
    static constexpr int kFaderH    = 150;   // 整链模式纵向推子高（含顶部数值框）
    static constexpr int kFaderHDefault = 205; // 单效果推子默认高度（充满卡片）
    static constexpr int kFaderGap  = 4;
    static constexpr int kModuleGap = 18;
    static constexpr int kLeftRailW = 64;    // 单效果模式左侧操作栏（放「旁通」「重置」）
    static constexpr int kRailTop    = 48;   // 左侧操作栏上沿（标题/说明之下）
    static constexpr int kRailBottom = 10;   // 左侧操作栏下沿距卡片底
    static constexpr int kBottomReserve = 36; // 单效果底部预留给「重置」行的高度

    struct Fader  { juce::Slider* s = nullptr; juce::Label* l = nullptr; };

    /** 单条推子的参考值刻度缓存：(纵轴位置比例 0=顶 1=底, 紧凑数值文本)。 */
    struct FaderMeta
    {
        juce::Slider* s = nullptr;
        std::vector<std::pair<float, juce::String>> ticks;
        size_t majorIndex = 0;
    };
    struct Module
    {
        int fxId = 0;
        juce::ToggleButton* toggle = nullptr;   // 可开关模块（EQ 用 header 标题）
        juce::Label* header = nullptr;
        std::vector<Fader> faders;
    };

    /** 正常态卡片尺寸（位置 / 收起 / 最大化由 FloatingCardPanel 处理）。 */
    juce::Point<float> normalSize() const override
    {
        const int rows = (fxSlot_ < 0 && mods_.size() > 4) ? 2 : 1;
        const int faderHSize = (fxSlot_ >= 0) ? kFaderHDefault : kFaderH;
        const int cardH = 50 + rows * (22 + 2 + faderHSize + 15)
                        + (rows - 1) * 12 + 46;
        int maxRow = 0;
        if (fxSlot_ < 0 && mods_.size() > 4)
        {
            // 两行布局按 8 模块静态估算（前 4 / 后 4），避免 resize 抖动
            int w1 = 0, w2 = 0;
            for (int i = 0; i < juce::jmin (4, (int) mods_.size()); ++i) w1 += moduleWidth (i);
            for (int i = 4; i < (int) mods_.size(); ++i)                 w2 += moduleWidth (i);
            maxRow = juce::jmax (w1, w2) + kModuleGap * 3;
        }
        else if (! mods_.empty())
        {
            for (int i = 0; i < (int) mods_.size(); ++i) maxRow += moduleWidth (i);
            maxRow += kModuleGap * juce::jmax (0, (int) mods_.size() - 1);
        }
        const int rail = leftRailWidth();
        const int needW = maxRow + 24 + rail;
        // 卡片宽度贴合内容：单效果模式收窄（不出现大片空白），
        // 整链模式按最宽一行计算；上限 860，且不超过窗口内边距
        const int maxW = juce::jmin (860, getWidth() - 80);
        const int minW = (fxSlot_ < 0 ? 430 : 320) + rail;
        const int cardW = juce::jlimit (minW, juce::jmax (minW, maxW), needW);
        return { static_cast<float> (cardW), static_cast<float> (cardH) };
    }

    /** 缩放下限：卡片不能小于内容（推子 + 模块行），否则控件会画到卡片外；
        因此只允许用户放大 / 加宽，缩小到此为止。 */
    juce::Point<float> minCardSize() const override { return normalSize(); }

    /** 单效果模式才辟出左侧操作栏：把「旁通」从推子上方挪到窗口左缘。 */
    int leftRailWidth() const noexcept { return fxSlot_ >= 0 ? kLeftRailW : 0; }

    int moduleWidth (int i) const
    {
        const int n = static_cast<int> (mods_[static_cast<size_t> (i)].faders.size());
        return n * kFaderW + (n - 1) * kFaderGap;
    }

    mb::FxParams read() const { return engine_.fxParams (ch_); }
    void write (const mb::FxParams& p)
    {
        engine_.setFxParams (ch_, p);
        if (onChanged) onChanged();
    }

    /** 当前插件的旁通状态（与通道条插件圆点同源：PluginBank 的 bypass 标志）。 */
    bool currentBypass() const
    {
        auto& bank = engine_.pluginBank();
        const int idx = bank.builtInSlot (ch_, fxSlot_);
        return idx >= 0 && bank.pluginBypass (ch_, idx);
    }

    /** 切换旁通：插件链标志与内置效果链模块开关必须同步写，
        否则「旁通」只改了 UI、声音依旧在（与 ChannelStrip 一致）。 */
    void setBypass (bool bypass)
    {
        auto& bank = engine_.pluginBank();
        if (const int idx = bank.builtInSlot (ch_, fxSlot_); idx >= 0)
            bank.setBypass (ch_, idx, bypass);

        auto p = engine_.fxParams (ch_);
        mb::applyBuiltInToParams (p, fxSlot_, ! bypass);
        engine_.setFxParams (ch_, p);

        refreshControls();                  // 推子 / 隐藏模块开关同步到旁通后的参数
        bypassBtn_.setToggleState (bypass, juce::dontSendNotification);
        if (onChanged) onChanged();         // 通道条插件圆点即时刷新
        repaint();
    }

    juce::ToggleButton* addToggle (const juce::String& text, bool on)
    {
        auto* t = new juce::ToggleButton (text);
        t->setColour (juce::ToggleButton::textColourId,     mb::ui::text);
        t->setColour (juce::ToggleButton::tickColourId,     mb::ui::accent[2]);
        t->setClickingTogglesState (true);
        t->setToggleState (on, juce::dontSendNotification);
        addAndMakeVisible (t);
        owned_.add (t);
        return t;
    }

    /** 纵向推子：调音台同款物理手柄 + 左侧参考值刻度；数值框在上，双击回默认值。 */
    juce::Slider* addFader (double lo, double hi, double step, double value,
                            const juce::String& suffix, double dblDefault,
                            double skewMid = 0.0, bool percent = false)
    {
        auto* s = new juce::Slider (juce::Slider::LinearVertical,
                                    juce::Slider::TextBoxAbove);
        s->setRange (lo, hi, step);
        s->setValue (value, juce::dontSendNotification);
        if (suffix.isNotEmpty()) s->setTextValueSuffix (suffix);
        if (percent)
        {
            s->textFromValueFunction = [] (double v)
                { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
            s->valueFromTextFunction = [] (const juce::String& t)
                { return t.trimCharactersAtEnd ("%").getDoubleValue() / 100.0; };
        }
        s->setTextBoxStyle (juce::Slider::TextBoxAbove, false, kFaderW, 15);
        s->setSliderStyle (juce::Slider::LinearVertical);
        s->setPopupMenuEnabled (false);
        s->setDoubleClickReturnValue (true, dblDefault);        // 双击回默认值
        if (skewMid > 0.0) s->setSkewFactorFromMidPoint (skewMid);
        s->setLookAndFeel (&faderLf_);                          // 导轨 / 手柄：主题色
        s->setTextBoxIsEditable (true);
        addAndMakeVisible (s);
        owned_.add (s);

        // 参考值：5 等分刻度 + 默认值所在的主刻度（与通道推子卡同款）
        const auto textFor = [percent] (double v)
        {
            if (percent) return juce::String (juce::roundToInt (v * 100.0));
            juce::String t (v, 1);
            if (t.endsWith (".0")) t = t.dropLastCharacters (2);
            return t;
        };
        FaderMeta meta;
        meta.s = s;
        const double defProp = juce::jlimit (0.0, 1.0, s->valueToProportionOfLength (dblDefault));
        const float defP = static_cast<float> (1.0 - defProp);
        for (double prop : { 1.0, 0.75, 0.5, 0.25, 0.0 })
        {
            const float p = static_cast<float> (1.0 - prop);
            meta.ticks.emplace_back (p, textFor (s->proportionOfLengthToValue (prop)));
        }
        size_t nearest = 0;
        float best = 1.0e6f;
        for (size_t i = 0; i < meta.ticks.size(); ++i)
        {
            const float d = std::abs (meta.ticks[i].first - defP);
            if (d < best) { best = d; nearest = i; }
        }
        if (best < 0.06f)
        {
            meta.majorIndex = nearest;
            meta.ticks[nearest].second = textFor (dblDefault);
        }
        else
        {
            meta.ticks.emplace_back (defP, textFor (dblDefault));
            meta.majorIndex = meta.ticks.size() - 1;
        }
        faderMetas_.push_back (std::move (meta));
        return s;
    }

    /** 每个推子一张「调音台同款推子卡」：主题 card 底 + 边框 + 左侧参考值刻度。 */
    void drawFaderCards (juce::Graphics& g)
    {
        for (const auto& m : faderMetas_)
        {
            auto* s = m.s;
            if (s == nullptr || ! s->isVisible()) continue;
            const auto r = s->getBounds().toFloat();
            g.setColour (mb::ui::card);
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (mb::ui::border);
            g.drawRoundedRectangle (r, 6.0f, 1.0f);

            // 纵轴 = JUCE 实际轨道区（含 TextBoxAbove 偏置），与手柄位置严格对齐
            const auto lay = s->getLookAndFeel().getSliderLayout (*s);
            const auto axis = lay.sliderBounds.toFloat()
                                .translated (static_cast<float> (s->getX()),
                                             static_cast<float> (s->getY()));
            if (axis.getHeight() < 30.0f) continue;
            for (size_t i = 0; i < m.ticks.size(); ++i)
            {
                const float y = axis.getY() + m.ticks[i].first * axis.getHeight();
                mb::ui::drawFaderRefTick (g, r.getX(), y, m.ticks[i].second,
                                          i == m.majorIndex);
            }
        }
    }

    /** 构造期 setColour 捕获的颜色切主题后不会自动变，这里统一刷新。 */
    void syncThemeColours()
    {
        themeDark_ = mb::ui::isDarkTheme();
        for (int i = 0; i < owned_.size(); ++i)
        {
            auto* c = owned_[static_cast<size_t> (i)];
            if (auto* s = dynamic_cast<juce::Slider*> (c))
            {
                s->setColour (juce::Slider::textBoxTextColourId,      mb::ui::text);
                s->setColour (juce::Slider::textBoxBackgroundColourId, mb::ui::card);
                s->setColour (juce::Slider::textBoxOutlineColourId,    mb::ui::border);
                s->setColour (juce::Slider::textBoxHighlightColourId,  mb::ui::accent[0]);
            }
            else if (auto* l = dynamic_cast<juce::Label*> (c))
            {
                const bool bold = (l->getFont().getStyleFlags() & juce::Font::bold) != 0;
                l->setColour (juce::Label::textColourId,
                              bold ? mb::ui::text : mb::ui::faintText);
            }
            else if (auto* t = dynamic_cast<juce::ToggleButton*> (c))
            {
                t->setColour (juce::ToggleButton::textColourId, mb::ui::text);
                t->setColour (juce::ToggleButton::tickColourId,  mb::ui::accent[2]);
            }
        }
        resetBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
        resetBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::subText);
        bypassBtn_.setColour (juce::TextButton::buttonColourId,    mb::ui::slotBg);
        bypassBtn_.setColour (juce::TextButton::textColourOffId,   mb::ui::subText);
        bypassBtn_.setColour (juce::TextButton::buttonOnColourId,  mb::ui::accent[0]);
        bypassBtn_.setColour (juce::TextButton::textColourOnId,    juce::Colours::white);
    }

    /** 随语言切换刷新的文案绑定（构造期缓存的 Label 需要原地改文本）。 */
    std::vector<std::function<void()>> textRefreshers_;

    juce::Label* addLocalisedLabel (const char* zh, const char* en, bool bold = false)
    {
        auto* l = addLabel (mb::tr (zh, en), bold);
        textRefreshers_.push_back ([l, zh, en]
        {
            l->setText (mb::tr (zh, en), juce::dontSendNotification);
        });
        return l;
    }

    juce::Label* addLabel (const juce::String& text, bool bold = false)
    {
        auto* l = new juce::Label ({}, text);
        l->setFont (mb::ui::font (8.5f, bold));
        l->setColour (juce::Label::textColourId, bold ? mb::ui::text : mb::ui::faintText);
        l->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
        owned_.add (l);
        return l;
    }

    /** 把界面控件同步回参数当前值（重置用）。 */
    void refreshControls()
    {
        const auto p = read();
        auto set = [] (juce::Slider* s, double v)
        {
            if (s != nullptr) s->setValue (v, juce::dontSendNotification);
        };
        auto setT = [] (juce::ToggleButton* t, bool v)
        {
            if (t != nullptr) t->setToggleState (v, juce::dontSendNotification);
        };
        setT (gateToggle_, p.gateOn);   set (gateSlider_, p.gateThreshDb);
        setT (hpfToggle_,  p.hpfOn);    set (hpfSlider_,  p.hpfHz);
        set (eqLo_, p.eqLowDb);  set (eqMidHz_, p.eqMidHz);
        set (eqMid_, p.eqMidDb); set (eqHi_, p.eqHighDb);
        setT (compToggle_, p.compOn);   set (compThresh_, p.compThreshDb);
        set (compRatio_, p.compRatio);  set (compAtk_, p.compAtkMs);
        set (compRel_, p.compRelMs);
        setT (drvToggle_, p.driveOn);   set (drvAmt_, p.driveAmt);
        set (drvMix_, p.driveMix);
        setT (dlyToggle_, p.delayOn);   set (dlyMs_, p.delayMs);
        set (dlyFb_, p.delayFb);        set (dlyMix_, p.delayMix);
        setT (revToggle_, p.reverbOn);  set (revMix_, p.reverbMix);
        set (revSize_, p.reverbSize);
        setT (limToggle_, p.limitOn);   set (limCeil_, p.limitCeilDb);
        setT (duckToggle_, p.duckOn);   set (duckThresh_, p.duckThreshDb);
        set (duckDepth_, p.duckDepthDb); set (duckAtk_, p.duckAtkMs);
        set (duckRel_, p.duckRelMs);
        if (duckSrcRefresh_) duckSrcRefresh_();
    }

    void buildUi()
    {
        const auto p = read();
        const bool all = fxSlot_ < 0;

        // ---- 噪声门 --------------------------------------------------------
        if (all || fxSlot_ == 1)
        {
            auto* t = addToggle (mb::builtInPluginName (1), p.gateOn);
            auto* s = addFader (-80.0, -20.0, 0.5, p.gateThreshDb, mb::S(" dB"),
                                -52.0, 0.0, false);
            auto* l = addLocalisedLabel ("阈值", "Threshold");
            t->onStateChange = [this, t, s]
            {
                auto q = read(); q.gateOn = t->getToggleState();
                q.gateThreshDb = static_cast<float> (s->getValue()); write (q);
            };
            s->onValueChange = [this, t, s]
            {
                auto q = read(); q.gateOn = t->getToggleState();
                q.gateThreshDb = static_cast<float> (s->getValue()); write (q);
            };
            gateToggle_ = t; gateSlider_ = s;
            mods_.push_back ({ 1, t, nullptr, { { s, l } } });
        }

        // ---- 高通 ----------------------------------------------------------
        if (all || fxSlot_ == 2)
        {
            auto* t = addToggle (mb::builtInPluginName (2), p.hpfOn);
            auto* s = addFader (20.0, 500.0, 1.0, p.hpfHz, mb::S(" Hz"), 90.0, 120.0);
            auto* l = addLocalisedLabel ("截止频率", "Cutoff");
            t->onStateChange = [this, t, s]
            {
                auto q = read(); q.hpfOn = t->getToggleState();
                q.hpfHz = static_cast<float> (s->getValue()); write (q);
            };
            s->onValueChange = [this, t, s]
            {
                auto q = read(); q.hpfOn = t->getToggleState();
                q.hpfHz = static_cast<float> (s->getValue()); write (q);
            };
            hpfToggle_ = t; hpfSlider_ = s;
            mods_.push_back ({ 2, t, nullptr, { { s, l } } });
        }

        // ---- 三段 EQ（无开关：增益全零 = 不处理）---------------------------
        if (all || fxSlot_ == 3)
        {
            auto* sLo  = addFader (-12.0, 12.0, 0.1, p.eqLowDb,  mb::S(" dB"), 0.0);
            auto* sHz  = addFader (200.0, 5000.0, 1.0, p.eqMidHz, mb::S(" Hz"), 1000.0, 1000.0);
            auto* sMid = addFader (-12.0, 12.0, 0.1, p.eqMidDb,  mb::S(" dB"), 0.0);
            auto* sHi  = addFader (-12.0, 12.0, 0.1, p.eqHighDb, mb::S(" dB"), 0.0);
            auto commit = [this, sLo, sHz, sMid, sHi]
            {
                auto q = read();
                q.eqLowDb  = static_cast<float> (sLo->getValue());
                q.eqMidHz  = static_cast<float> (sHz->getValue());
                q.eqMidDb  = static_cast<float> (sMid->getValue());
                q.eqHighDb = static_cast<float> (sHi->getValue());
                write (q);
            };
            sLo->onValueChange = commit; sHz->onValueChange = commit;
            sMid->onValueChange = commit; sHi->onValueChange = commit;
            eqLo_ = sLo; eqMidHz_ = sHz; eqMid_ = sMid; eqHi_ = sHi;
            mods_.push_back ({ 3, nullptr, addLabel (mb::builtInPluginName (3), true),
                { { sLo, addLocalisedLabel ("低频", "Low") },
                  { sHz, addLocalisedLabel ("中频", "Mid Freq") },
                  { sMid, addLocalisedLabel ("中频增益", "Mid Gain") },
                  { sHi, addLocalisedLabel ("高频", "High") } } });
        }

        // ---- 压缩器 --------------------------------------------------------
        if (all || fxSlot_ == 4)
        {
            auto* t  = addToggle (mb::builtInPluginName (4), p.compOn);
            auto* sT = addFader (-60.0, 0.0, 0.5, p.compThreshDb, mb::S(" dB"), -24.0);
            auto* sR = addFader (1.0, 12.0, 0.1, p.compRatio, mb::S(" : 1"), 3.0);
            auto* sA = addFader (1.0, 100.0, 1.0, p.compAtkMs, mb::S(" ms"), 12.0, 12.0);
            auto* sRl= addFader (30.0, 1000.0, 1.0, p.compRelMs, mb::S(" ms"), 180.0, 180.0);
            auto commit = [this, t, sT, sR, sA, sRl]
            {
                auto q = read();
                q.compOn = t->getToggleState();
                q.compThreshDb = static_cast<float> (sT->getValue());
                q.compRatio    = static_cast<float> (sR->getValue());
                q.compAtkMs    = static_cast<float> (sA->getValue());
                q.compRelMs    = static_cast<float> (sRl->getValue());
                write (q);
            };
            t->onStateChange = commit;
            sT->onValueChange = commit; sR->onValueChange = commit;
            sA->onValueChange = commit; sRl->onValueChange = commit;
            compToggle_ = t; compThresh_ = sT; compRatio_ = sR;
            compAtk_ = sA; compRel_ = sRl;
            mods_.push_back ({ 4, t, nullptr,
                { { sT,  addLocalisedLabel ("阈值", "Threshold") },
                  { sR,  addLocalisedLabel ("压缩比", "Ratio") },
                  { sA,  addLocalisedLabel ("启动", "Attack") },
                  { sRl, addLocalisedLabel ("释放", "Release") } } });
        }

        // ---- 激励驱动 ------------------------------------------------------
        if (all || fxSlot_ == 8)
        {
            auto* t  = addToggle (mb::builtInPluginName (8), p.driveOn);
            auto* sA = addFader (1.0, 10.0, 0.1, p.driveAmt, mb::S(" x"), 3.0);
            auto* sM = addFader (0.0, 1.0, 0.01, p.driveMix, juce::String(), 0.35, 0.0, true);
            auto commit = [this, t, sA, sM]
            {
                auto q = read();
                q.driveOn  = t->getToggleState();
                q.driveAmt = static_cast<float> (sA->getValue());
                q.driveMix = static_cast<float> (sM->getValue());
                write (q);
            };
            t->onStateChange = commit;
            sA->onValueChange = commit; sM->onValueChange = commit;
            drvToggle_ = t; drvAmt_ = sA; drvMix_ = sM;
            mods_.push_back ({ 8, t, nullptr,
                { { sA, addLocalisedLabel ("驱动量", "Drive") },
                  { sM, addLocalisedLabel ("混合", "Mix") } } });
        }

        // ---- 延迟 ----------------------------------------------------------
        if (all || fxSlot_ == 7)
        {
            auto* t  = addToggle (mb::builtInPluginName (7), p.delayOn);
            auto* sMs= addFader (10.0, 1000.0, 1.0, p.delayMs, mb::S(" ms"), 240.0, 120.0);
            auto* sF = addFader (0.0, 0.95, 0.01, p.delayFb, juce::String(), 0.0, 0.0, true);
            auto* sM = addFader (0.0, 1.0, 0.01, p.delayMix, juce::String(), 0.25, 0.0, true);
            auto commit = [this, t, sMs, sF, sM]
            {
                auto q = read();
                q.delayOn  = t->getToggleState();
                q.delayMs  = static_cast<float> (sMs->getValue());
                q.delayFb  = static_cast<float> (sF->getValue());
                q.delayMix = static_cast<float> (sM->getValue());
                write (q);
            };
            t->onStateChange = commit;
            sMs->onValueChange = commit; sF->onValueChange = commit;
            sM->onValueChange = commit;
            dlyToggle_ = t; dlyMs_ = sMs; dlyFb_ = sF; dlyMix_ = sM;
            mods_.push_back ({ 7, t, nullptr,
                { { sMs, addLocalisedLabel ("时间", "Time") },
                  { sF,  addLocalisedLabel ("反馈", "Feedback") },
                  { sM,  addLocalisedLabel ("湿度", "Mix") } } });
        }

        // ---- 混响 ----------------------------------------------------------
        if (all || fxSlot_ == 5)
        {
            auto* t  = addToggle (mb::builtInPluginName (5), p.reverbOn);
            auto* sM = addFader (0.0, 1.0, 0.01, p.reverbMix, juce::String(), 0.22, 0.0, true);
            auto* sS = addFader (0.0, 1.0, 0.01, p.reverbSize, juce::String(), 0.5, 0.0, true);
            auto commit = [this, t, sM, sS]
            {
                auto q = read();
                q.reverbOn   = t->getToggleState();
                q.reverbMix  = static_cast<float> (sM->getValue());
                q.reverbSize = static_cast<float> (sS->getValue());
                write (q);
            };
            t->onStateChange = commit;
            sM->onValueChange = commit; sS->onValueChange = commit;
            revToggle_ = t; revMix_ = sM; revSize_ = sS;
            mods_.push_back ({ 5, t, nullptr,
                { { sM, addLocalisedLabel ("湿度", "Mix") },
                  { sS, addLocalisedLabel ("空间", "Room") } } });
        }

        // ---- 限制器 --------------------------------------------------------
        if (all || fxSlot_ == 6)
        {
            auto* t = addToggle (mb::builtInPluginName (6), p.limitOn);
            auto* s = addFader (-12.0, 0.0, 0.5, p.limitCeilDb, mb::S(" dB"), -1.0);
            auto commit = [this, t, s]
            {
                auto q = read();
                q.limitOn = t->getToggleState();
                q.limitCeilDb = static_cast<float> (s->getValue());
                write (q);
            };
            t->onStateChange = commit;
            s->onValueChange = commit;
            limToggle_ = t; limCeil_ = s;
            mods_.push_back ({ 6, t, nullptr, { { s, addLocalisedLabel ("上限", "Ceiling") } } });
        }

        // ---- 侧链闪避 -------------------------------------------------------
        if (all || fxSlot_ == 10)
        {
            // 源通道循环按钮（放左侧操作栏）：点击在 -1..15 间循环，一圈回到「未指定」
            duckSrcRefresh_ = [this]
            {
                if (duckSrcBtn_ == nullptr) return;
                const int s = engine_.fxParams (ch_).duckSrcCh;
                duckSrcBtn_->setButtonText (mb::tr ("源: ", "Src: ")
                    + (s < 0 ? mb::S("--") : mb::channelDisplayName (s)));
            };
            auto* src = new juce::TextButton();
            duckSrcBtn_ = src;
            src->setColour (juce::TextButton::buttonColourId, mb::ui::slotBg);
            src->setColour (juce::TextButton::textColourOffId, mb::ui::subText);
            src->setTooltip (mb::tr ("点击切换闪避源通道（源出声时压低本通道）",
                                     "Click to cycle the ducking source channel"));
            src->onClick = [this]
            {
                auto q = read();
                int s = q.duckSrcCh < 0 ? 0 : q.duckSrcCh + 1;
                if (s >= 16) s = -1;                    // 一圈回到「未指定」
                q.duckSrcCh = s;
                write (q);
                if (duckSrcRefresh_) duckSrcRefresh_();
            };
            addAndMakeVisible (src);
            owned_.add (src);
            src->setVisible (fxSlot_ >= 0);   // 整链模式无左栏，源通道在单插件面板里选
            textRefreshers_.push_back ([this] { if (duckSrcRefresh_) duckSrcRefresh_(); });

            auto* t  = addToggle (mb::builtInPluginName (10), p.duckOn);
            auto* sT = addFader (-60.0, 0.0, 0.5, p.duckThreshDb, mb::S(" dB"), -28.0);
            auto* sD = addFader (-24.0, 0.0, 0.5, p.duckDepthDb, mb::S(" dB"), -12.0);
            auto* sA = addFader (1.0, 100.0, 1.0, p.duckAtkMs, mb::S(" ms"), 15.0, 15.0);
            auto* sR = addFader (50.0, 1500.0, 5.0, p.duckRelMs, mb::S(" ms"), 350.0, 350.0);
            auto commit = [this, t, sT, sD, sA, sR]
            {
                auto q = read();
                q.duckOn = t->getToggleState();
                // 开启时未指定源或源是自身 → 自动指向下一个通道
                if (q.duckOn && (q.duckSrcCh < 0 || q.duckSrcCh == ch_))
                    q.duckSrcCh = (ch_ + 1) % 16;
                q.duckThreshDb = static_cast<float> (sT->getValue());
                q.duckDepthDb  = static_cast<float> (sD->getValue());
                q.duckAtkMs    = static_cast<float> (sA->getValue());
                q.duckRelMs    = static_cast<float> (sR->getValue());
                write (q);
            };
            t->onStateChange = commit;
            sT->onValueChange = commit; sD->onValueChange = commit;
            sA->onValueChange = commit; sR->onValueChange = commit;
            duckToggle_ = t; duckThresh_ = sT; duckDepth_ = sD;
            duckAtk_ = sA; duckRel_ = sR;
            mods_.push_back ({ 10, t, nullptr,
                { { sT, addLocalisedLabel ("阈值", "Threshold") },
                  { sD, addLocalisedLabel ("压低量", "Depth") },
                  { sA, addLocalisedLabel ("启动", "Attack") },
                  { sR, addLocalisedLabel ("释放", "Release") } } });
        }

        // 单效果模式：内置插件已拆成独立面板，模块勾选框 + 插件标题不再需要
        // （开关由左上角「旁通」接管）。整链模式仍保留模块标题区分 8 个模块。
        if (fxSlot_ >= 0)
            for (auto& m : mods_)
            {
                if (m.toggle != nullptr) m.toggle->setVisible (false);
                if (m.header != nullptr) m.header->setVisible (false);
            }

        // ---- 左上角「旁通」-------------------------------------------------
        addAndMakeVisible (bypassBtn_);
        bypassBtn_.setButtonText (mb::tr ("旁通", "Bypass"));
        bypassBtn_.setClickingTogglesState (true);
        bypassBtn_.setToggleState (currentBypass(), juce::dontSendNotification);
        bypassBtn_.setTooltip (mb::tr ("旁通本插件（与通道条插件圆点同步）",
                                      "Bypass this plugin (synced with the strip dot)"));
        bypassBtn_.onClick = [this] { setBypass (bypassBtn_.getToggleState()); };
        bypassBtn_.setVisible (fxSlot_ >= 0);

        // ---- 底部按钮 ------------------------------------------------------
        addAndMakeVisible (resetBtn_);
        resetBtn_.setButtonText (fxSlot_ < 0 ? mb::tr ("全部重置", "Reset all")
                                             : mb::tr ("重置", "Reset"));
        resetBtn_.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
        resetBtn_.setColour (juce::TextButton::textColourOffId, mb::ui::subText);
        resetBtn_.onClick = [this]
        {
            write (fxSlot_ < 0 ? mb::FxParams {} : resetFocusedParams());
            refreshControls();
        };

        syncThemeColours();

        // 窗口按钮（最小化/最大化/关闭）由基类 FloatingCardPanel 托管
    }

    mb::AudioEngine& engine_;
    int ch_;
    int fxSlot_;                                 // -1 = 整条链，1..8 = 单效果
    juce::Image logo_;

    /** 单效果重置：只还原当前效果参数，其余不动（返回修改后的完整参数）。
        勾选框移除后由「旁通」统一管开关，因此重置不能再把模块悄悄关掉：
        各模块 on 标志（= 未旁通）原样保留。 */
    mb::FxParams resetFocusedParams() const
    {
        auto q = read();
        const mb::FxParams d {};
        switch (fxSlot_)
        {
            case 1: q.gateThreshDb = d.gateThreshDb; break;
            case 2: q.hpfHz        = d.hpfHz;        break;
            case 3: q.eqLowDb = d.eqLowDb; q.eqMidDb = d.eqMidDb;
                    q.eqMidHz = d.eqMidHz; q.eqHighDb = d.eqHighDb;
                    // EQ 无 on 标志：未旁通时给回一个可听的初始量（与「添加」时一致）
                    if (! currentBypass()) mb::applyBuiltInToParams (q, 3, true);
                    break;
            case 4: q.compThreshDb = d.compThreshDb; q.compRatio = d.compRatio;
                    q.compAtkMs = d.compAtkMs;       q.compRelMs = d.compRelMs; break;
            case 5: q.reverbMix = d.reverbMix;   q.reverbSize = d.reverbSize; break;
            case 6: q.limitCeilDb = d.limitCeilDb; break;
            case 7: q.delayMs = d.delayMs; q.delayFb = d.delayFb;
                    q.delayMix = d.delayMix; break;
            case 8: q.driveAmt = d.driveAmt; q.driveMix = d.driveMix; break;
            case 10: q.duckThreshDb = d.duckThreshDb; q.duckDepthDb = d.duckDepthDb;
                     q.duckAtkMs = d.duckAtkMs;       q.duckRelMs = d.duckRelMs;
                     break;                            // 源通道是接线意图，重置不动它
            default: break;
        }
        return q;
    }

    std::vector<Module> mods_;
    std::vector<FaderMeta> faderMetas_;
    mb::ui::FaderSkin faderLf_;            // 声明在 owned_ 之前 → 先于 Slider 销毁
    bool themeDark_ = mb::ui::isDarkTheme();
    juce::OwnedArray<juce::Component> owned_;
    juce::TextButton resetBtn_, bypassBtn_;

    juce::ToggleButton* gateToggle_ = nullptr;  juce::Slider* gateSlider_ = nullptr;
    juce::ToggleButton* hpfToggle_  = nullptr;  juce::Slider* hpfSlider_  = nullptr;
    juce::Slider* eqLo_ = nullptr, *eqMid_ = nullptr, *eqHi_ = nullptr, *eqMidHz_ = nullptr;
    juce::ToggleButton* compToggle_ = nullptr;
    juce::Slider* compThresh_ = nullptr, *compRatio_ = nullptr;
    juce::Slider* compAtk_ = nullptr, *compRel_ = nullptr;
    juce::ToggleButton* drvToggle_ = nullptr;
    juce::Slider* drvAmt_ = nullptr, *drvMix_ = nullptr;
    juce::ToggleButton* dlyToggle_ = nullptr;
    juce::Slider* dlyMs_ = nullptr, *dlyFb_ = nullptr, *dlyMix_ = nullptr;
    juce::ToggleButton* revToggle_ = nullptr;
    juce::Slider* revMix_ = nullptr, *revSize_ = nullptr;
    juce::ToggleButton* limToggle_ = nullptr;   juce::Slider* limCeil_ = nullptr;
    juce::ToggleButton* duckToggle_ = nullptr;
    juce::Slider* duckThresh_ = nullptr, *duckDepth_ = nullptr;
    juce::Slider* duckAtk_ = nullptr, *duckRel_ = nullptr;
    juce::TextButton* duckSrcBtn_ = nullptr;    // 闪避源通道循环按钮（左栏）
    std::function<void()> duckSrcRefresh_;      // 文本随参数/语言刷新

    JUCE_DECLARE_NON_COPYABLE (FxPanel)
};
