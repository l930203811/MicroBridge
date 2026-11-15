#pragma once

#include <JuceHeader.h>

#include <vector>
#include <utility>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

//==============================================================================
// 全局快捷键引擎（Windows）：低级键盘钩子 WH_KEYBOARD_LL，支持多条绑定。
//   · 「设置 → 快捷键」的隐藏/恢复窗口快捷键
//   · 「快捷键与链接」里每条通道录入的快捷键
// 统一由这一个钩子派发，避免多个钩子互相吞键 / 抢优先级。
//
// 系统级生效：本应用不在前台时按下也会触发。相比 RegisterHotKey 不受
// 「组合键被其它程序占用」限制，也不依赖 message-only 窗口收 WM_HOTKEY。
//
// 触发回调经 MessageManager::callAsync 派发到消息线程，钩子回调本身立即返回，
// 避免超过系统 LowLevelHooksTimeout 被静默卸载。
//
// 无修饰键的普通按键命中时只触发、不吞键，避免劫持用户在其它程序里的
// 正常打字；带 Ctrl/Alt/Shift 或 F 功能键的组合命中即吞掉。
//==============================================================================
namespace mb
{
class GlobalHotkey
{
public:
    struct Binding
    {
        int key = 0;                       // JUCE keyCode（0 = 未设置）
        int mods = 0;                      // JUCE ModifierKeys 原始 flags
        std::function<void()> callback;    // 命中后在消息线程执行
    };

    /** 兼容入口：隐藏/恢复窗口快捷键的回调（宿主在构造时接线）。 */
    std::function<void()> onTrigger;

    GlobalHotkey() = default;

    ~GlobalHotkey()
    {
       #if JUCE_WINDOWS
        uninstall();
       #endif
    }

    /** 用整组绑定替换当前注册。返回 false 表示有按键不支持或钩子安装失败。 */
    bool setBindings (std::vector<Binding> list)
    {
       #if JUCE_WINDOWS
        uninstall();
        entries_.clear();

        bool ok = true;
        for (auto& b : list)
        {
            if (b.key == 0) continue;
            const UINT vk = vkFromJuce (b.key);
            if (vk == 0) { ok = false; continue; }   // 不支持的键：跳过并整体报失败

            Entry e;
            e.vk        = vk;
            e.key       = b.key;
            e.mods      = b.mods;
            e.callback  = std::move (b.callback);
            e.wantCtrl  = (b.mods & juce::ModifierKeys::ctrlModifier)    != 0;
            e.wantAlt   = (b.mods & juce::ModifierKeys::altModifier)     != 0;
            e.wantShift = (b.mods & juce::ModifierKeys::shiftModifier)   != 0;
            // Windows 上 commandModifier 等同 ctrlModifier（非 Win 键），
            // 因此只精确匹配 Ctrl/Alt/Shift，与 mb::links::matchesHotkey 一致。
            e.swallow   = e.wantCtrl || e.wantAlt || e.wantShift
                          || (vk >= VK_F1 && vk <= VK_F24);
            entries_.push_back (std::move (e));
        }

        if (entries_.empty()) return ok;              // 无绑定 = 不需要钩子
        if (! install()) return false;
        return ok;
       #else
        juce::ignoreUnused (list);
        return true;
       #endif
    }

    /** 钩子是否已安装（无任何绑定时为 false）。 */
    bool isActive() const noexcept
    {
       #if JUCE_WINDOWS
        return hook_ != nullptr;
       #else
        return false;
       #endif
    }

    /** 该组合键是否已由全局钩子接管（用于抑制 JUCE keyPressed 重复触发）。 */
    bool isBound (int key, int mods) const noexcept
    {
       #if JUCE_WINDOWS
        if (hook_ == nullptr || key == 0) return false;
        const UINT vk = vkFromJuce (key);
        if (vk == 0) return false;
        for (const auto& e : entries_)
            if (e.vk == vk
                && e.wantCtrl  == ((mods & juce::ModifierKeys::ctrlModifier)    != 0)
                && e.wantAlt   == ((mods & juce::ModifierKeys::altModifier)     != 0)
                && e.wantShift == ((mods & juce::ModifierKeys::shiftModifier)   != 0))
                return true;
        return false;
       #else
        juce::ignoreUnused (key, mods);
        return false;
       #endif
    }

    bool isBound (const juce::KeyPress& k) const noexcept
    {
        return isBound (k.getKeyCode(), k.getModifiers().getRawFlags());
    }

    /** 录入快捷键时临时抑制钩子：既不触发，也不吞掉按键。 */
    void setSuppressed (bool s) noexcept
    {
       #if JUCE_WINDOWS
        suppressed_ = s;
        if (s) for (auto& e : entries_) e.pressed = false;
       #else
        juce::ignoreUnused (s);
       #endif
    }

private:
   #if JUCE_WINDOWS
    struct Entry
    {
        UINT vk = 0;
        int key = 0, mods = 0;
        std::function<void()> callback;
        bool wantCtrl = false, wantAlt = false, wantShift = false;
        bool swallow = true;
        bool pressed = false;
    };

    /** JUCE keyCode → Win32 虚拟键码。Windows 上 JUCE 的特殊键带
        extendedKeyModifier(0x10000)，低字节即 VK；字母做大小写容错。 */
    static UINT vkFromJuce (int k)
    {
        using K = juce::KeyPress;
        if (k >= 'a' && k <= 'z') k -= 'a' - 'A';
        if (k >= 'A' && k <= 'Z') return (UINT) k;
        if (k >= '0' && k <= '9') return (UINT) k;
        if (k >= K::F1Key  && k <= K::F24Key) return VK_F1 + (UINT) (k - K::F1Key);
        if (k == K::spaceKey)     return VK_SPACE;
        if (k == K::returnKey)    return VK_RETURN;
        if (k == K::tabKey)       return VK_TAB;
        if (k == K::homeKey)      return VK_HOME;
        if (k == K::endKey)       return VK_END;
        if (k == K::pageUpKey)    return VK_PRIOR;
        if (k == K::pageDownKey)  return VK_NEXT;
        if (k == K::insertKey)    return VK_INSERT;
        if (k == K::deleteKey)    return VK_DELETE;
        if (k == K::upKey)        return VK_UP;
        if (k == K::downKey)      return VK_DOWN;
        if (k == K::leftKey)      return VK_LEFT;
        if (k == K::rightKey)     return VK_RIGHT;
        if (k == K::escapeKey)    return VK_ESCAPE;
        if (k == K::backspaceKey) return VK_BACK;
        // 兜底：任何带扩展位的 JUCE 键，低字节就是 VK
        if ((k & 0x10000) != 0 && (k & 0xff) != 0) return (UINT) (k & 0xff);
        return 0;
    }

    bool install()
    {
        instance_ = this;
        hook_ = SetWindowsHookExW (WH_KEYBOARD_LL, keyboardProc,
                                   GetModuleHandleW (nullptr), 0);
        if (hook_ == nullptr)
        {
            if (instance_ == this) instance_ = nullptr;
            return false;
        }
        return true;
    }

    void uninstall()
    {
        if (hook_ != nullptr)
        {
            UnhookWindowsHookEx (hook_);
            hook_ = nullptr;
        }
        if (instance_ == this) instance_ = nullptr;
        for (auto& e : entries_) e.pressed = false;
    }

    static bool down (int vk) noexcept { return (GetAsyncKeyState (vk) & 0x8000) != 0; }

    static LRESULT CALLBACK keyboardProc (int nCode, WPARAM w, LPARAM l)
    {
        auto* self = instance_;
        if (nCode == HC_ACTION && self != nullptr && ! self->suppressed_)
        {
            const auto* p = reinterpret_cast<const KBDLLHOOKSTRUCT*> (l);
            const bool isDown = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
            const bool isUp   = (w == WM_KEYUP   || w == WM_SYSKEYUP);

            if (p != nullptr && (isDown || isUp))
            {
                const bool ctrl  = down (VK_CONTROL);
                const bool alt   = down (VK_MENU);
                const bool shift = down (VK_SHIFT);
                bool swallow = false;

                for (auto& e : self->entries_)
                {
                    if (e.vk != p->vkCode) continue;
                    if (isUp) { e.pressed = false; continue; }
                    if (e.pressed) continue;                  // 按住不重复触发
                    if (ctrl != e.wantCtrl || alt != e.wantAlt
                        || shift != e.wantShift) continue;

                    e.pressed = true;
                    self->fire (e.callback);
                    if (e.swallow) swallow = true;
                }
                if (swallow) return 1;                        // 吞键：其它程序不再响应
            }
        }
        return CallNextHookEx (nullptr, nCode, w, l);
    }

    /** 钩子回调里不做重活：异步派发到 JUCE 消息线程，避免超时被系统卸载钩子。 */
    void fire (const std::function<void()>& cb)
    {
        if (! cb) return;
        auto copy = cb;
        juce::MessageManager::callAsync ([copy] { copy(); });
    }

    HHOOK hook_ = nullptr;
    std::vector<Entry> entries_;
    bool suppressed_ = false;
    inline static GlobalHotkey* instance_ = nullptr;
   #endif
};
}
