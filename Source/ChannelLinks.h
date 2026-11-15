#pragma once

#include <JuceHeader.h>

#include <vector>

#include "I18n.h"

//==============================================================================
// 通道「快捷键与链接」：跨通道的状态绑定 + 可选的全局快捷键。
//
// 每条通道可绑定一个「链接码」= 动作 × 类型字母(A~E) × 序号(1~5)：
//   · 同码（如两条通道都是 A1）            = 同步组：一起开 / 一起关
//   · 同字母不同序号（A1 与 A2）           = 互斥组：任一个激活，其他码全部关闭；允许全关
//   · 不绑定                               = 不参与任何联动
// 手动点击与快捷键走同一套联动逻辑；快捷键只是另一个触发入口。
//
// 快捷键与链接互相独立：
//   · 可以只链接不设快捷键（letter+index 参与同步/互斥，无快捷键触发）
//   · 快捷键由用户在弹窗里现场录入（任意键组合），不再按字母组自动生成；
//     录入相同快捷键的通道构成快捷键触发组（按动作整组翻转）。
//   · 录入的快捷键由 MainComponent 注册为系统级全局热键（其它程序前台按下
//     也生效）；同码共享组共用一个键，只注册一次，命中时整组翻转。
//
// 每个动作（开关/音量/效果/显示/独奏）各有一条独立配置：链接码与快捷键
// 都随动作分开设置 —— 开关可绑 A1+Ctrl+F1，效果同时绑 B2+Alt+E，互不影响。
//==============================================================================
namespace mb::links
{
    enum Action : int
    {
        kPower  = 0,   // 开关：SPK 通道激活开关
        kVolume = 1,   // 音量：同码推子联动（等值跟随，无互斥 / 无快捷键翻转）
        kFx     = 2,   // 效果：内置效果链全开 / 全关
        kSlot   = 3,   // 显示：通道条显示 / 隐藏（同右键「隐藏通道」联动）
        kSolo   = 4    // 独奏：S 按钮
    };
    inline constexpr int kActionCount = 5;
    inline constexpr int kLetterCount = 5;    // A..E
    inline constexpr int kIndexCount  = 5;    // 1..5

    struct LinkSpec
    {
        int action = kPower;   // 动作类型
        int letter = 0;        // 0=A .. 4=E
        int index  = 0;        // 0=未链接，1..5
        int key    = 0;        // 录入的全局快捷键主键码（0=未设置）
        int mods   = 0;        // 快捷键修饰键（ModifierKeys 原始 flags）

        bool operator== (const LinkSpec& o) const noexcept
        {
            return action == o.action && letter == o.letter && index == o.index
                && key == o.key && mods == o.mods;
        }
        bool operator!= (const LinkSpec& o) const noexcept { return ! operator== (o); }
        bool bound() const noexcept { return index > 0; }
        bool hasHotkey() const noexcept { return key != 0; }
    };

    /** 每通道一条的链接表（下标 = 通道序号）：每个动作各一条独立配置。 */
    struct LinkSet
    {
        LinkSpec per[kActionCount];              // 下标 = 动作（Action 枚举值）
        const LinkSpec& operator[] (int a) const noexcept { return per[a]; }
        LinkSpec&       operator[] (int a)       noexcept { return per[a]; }
        bool empty() const noexcept
        {
            for (const auto& s : per) if (s.bound() || s.hasHotkey()) return false;
            return true;
        }
    };

    inline std::vector<LinkSet>& table() noexcept
    {
        static std::vector<LinkSet> v;
        return v;
    }

    inline void resize (int channels) { table().resize (static_cast<size_t> (juce::jmax (0, channels))); }

    /** 读取通道某动作的配置（越界返回默认值）。 */
    inline LinkSpec linkOf (int ch, int action)
    {
        const auto& t = table();
        if (ch < 0 || ch >= static_cast<int> (t.size())) return LinkSpec{};
        const int a = juce::jlimit (0, kActionCount - 1, action);
        return t[static_cast<size_t> (ch)].per[a];
    }

    /** 写入通道某动作的配置（action 字段与存储槽位保持一致）。 */
    inline void setLink (int ch, int action, const LinkSpec& s)
    {
        if (ch < 0) return;
        auto& t = table();
        if (static_cast<int> (t.size()) <= ch) t.resize (static_cast<size_t> (ch) + 1);
        const int a = juce::jlimit (0, kActionCount - 1, action);
        auto out = s;
        out.action = a;
        t[static_cast<size_t> (ch)].per[a] = out;
    }

    /** 整组读取（通道重排 / 复制用）。 */
    inline LinkSet linkSetOf (int ch)
    {
        const auto& t = table();
        return (ch >= 0 && ch < static_cast<int> (t.size())) ? t[static_cast<size_t> (ch)] : LinkSet{};
    }

    /** 整组写入。 */
    inline void setLinkSet (int ch, const LinkSet& s)
    {
        if (ch < 0) return;
        auto& t = table();
        if (static_cast<int> (t.size()) <= ch) t.resize (static_cast<size_t> (ch) + 1);
        t[static_cast<size_t> (ch)] = s;
    }

    /** 聚合显示用：第一条已绑定或已设快捷键的配置（按动作序优先）。 */
    inline LinkSpec primaryOf (int ch)
    {
        const auto& t = table();
        if (ch < 0 || ch >= static_cast<int> (t.size())) return LinkSpec{};
        for (int a = 0; a < kActionCount; ++a)
        {
            const auto& s = t[static_cast<size_t> (ch)].per[a];
            if (s.bound() || s.hasHotkey()) return s;
        }
        return LinkSpec{};
    }

    /** 通道是否有任何动作绑定了链接码或快捷键。 */
    inline bool anyBound (int ch)
    {
        const auto& t = table();
        if (ch < 0 || ch >= static_cast<int> (t.size())) return false;
        return ! t[static_cast<size_t> (ch)].empty();
    }

    inline juce::String letterName (int letter)
    {
        return juce::String::charToString (static_cast<juce::juce_wchar> ('A' + juce::jlimit (0, kLetterCount - 1, letter)));
    }

    /** 链接码显示文本，如 "A1"；未绑定为空。 */
    inline juce::String codeString (const LinkSpec& s)
    {
        return s.bound() ? letterName (s.letter) + juce::String (s.index) : juce::String();
    }

    /** 动作显示名（菜单 / 弹窗用）。 */
    inline juce::String actionName (int action)
    {
        switch (action)
        {
            case kPower:  return mb::tr ("开关", "Power");
            case kVolume: return mb::tr ("音量", "Volume");
            case kFx:     return mb::tr ("效果", "FX");
            case kSlot:   return mb::tr ("显示", "Show");
            case kSolo:   return mb::tr ("独奏", "Solo");
            default: break;
        }
        return {};
    }

    /** 动作说明（弹窗动作行下方随选随变）。
        除「音量」外，各动作均可「仅链接」（同码同步 / 同字母互斥）
        或「仅设快捷键」（整组翻转），两者也可同时设置。 */
    inline juce::String actionDesc (int action)
    {
        switch (action)
        {
            case kPower:
                return mb::tr ("切换通道喇叭开关 · 可仅链接（同步/互斥），也可仅设快捷键",
                               "Speaker on/off · link-only (sync/mutex) or hotkey-only");
            case kVolume:
                return mb::tr ("同码推子等值跟随 · 无互斥，快捷键不翻转（仅链接生效）",
                               "Linked faders follow · no mutex, hotkey won't toggle (link only)");
            case kFx:
                return mb::tr ("整条效果链全开/全关（内置效果 + 插件）· 可仅链接，也可仅设快捷键",
                               "Toggle whole FX chain (built-in + plugins) · link-only or hotkey-only");
            case kSlot:
                return mb::tr ("显示 / 隐藏通道条 · 可仅链接，也可仅设快捷键",
                               "Show / hide channel strip · link-only or hotkey-only");
            case kSolo:
                return mb::tr ("切换 S 独奏 · 可仅链接，也可仅设快捷键",
                               "Toggle S solo · link-only or hotkey-only");
            default: break;
        }
        return {};
    }

    /** 主键码显示名：F1~F12 / 字母数字直出，其余用 JUCE 描述。 */
    inline juce::String keyDisplayName (int keyCode)
    {
        if (keyCode >= juce::KeyPress::F1Key && keyCode <= juce::KeyPress::F12Key)
            return "F" + juce::String (keyCode - juce::KeyPress::F1Key + 1);
        if ((keyCode >= 'A' && keyCode <= 'Z') || (keyCode >= '0' && keyCode <= '9'))
            return juce::String::charToString (static_cast<juce::juce_wchar> (keyCode));
        return juce::KeyPress (keyCode, juce::ModifierKeys(), 0).getTextDescription();
    }

    /** 录入快捷键的显示文本，如 "Ctrl+Alt+F1"；未设置为空。 */
    inline juce::String hotkeyText (const LinkSpec& s)
    {
        if (! s.hasHotkey()) return {};
        const auto m = juce::ModifierKeys (s.mods);
        juce::String t;
        if (m.isCtrlDown())  t << "Ctrl+";
        if (m.isAltDown())   t << "Alt+";
        if (m.isShiftDown()) t << "Shift+";
        return t + keyDisplayName (s.key);
    }

    /** 按键是否命中某通道录入的快捷键（仅比较 Ctrl/Alt/Shift 三个修饰键）。 */
    inline bool matchesHotkey (const LinkSpec& s, const juce::KeyPress& key)
    {
        if (! s.hasHotkey() || s.key != key.getKeyCode()) return false;
        const auto a = juce::ModifierKeys (s.mods);
        const auto b = key.getModifiers();
        return a.isCtrlDown()   == b.isCtrlDown()
            && a.isAltDown()    == b.isAltDown()
            && a.isShiftDown()  == b.isShiftDown();
    }
} // namespace mb::links
