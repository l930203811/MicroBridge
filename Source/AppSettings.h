#pragma once

#include <JuceHeader.h>

#include "I18n.h"
#include "SessionStore.h"

//==============================================================================
// 应用设置（exe 同级 MicroBridgeData/settings.json，便携化存储）
//
// · 与 session.json 分开：session 每次改动自动落盘用于崩溃恢复，
//   settings 只在设置页改动时写入，存用户偏好与「当前预设」路径。
// · 开机自启写 HKCU\...\Run，是唯一会触碰注册表的地方；关闭选项即删值。
//==============================================================================
namespace mb
{
    /** 内置插件浮层窗口状态：尺寸 / 位置 / 折叠 / 最大化，重开与重启都恢复。 */
    struct CardWindowState
    {
        bool hasSize = false, hasPos = false;
        double w = 0.0, h = 0.0, x = 0.0, y = 0.0;
        bool minimised = false, maximised = false;
    };

    struct AppSettingsData
    {
        bool autoStart = false;              // 开机自动启动
        bool startMinimizedToTray = false;   // 启动后隐藏到托盘
        bool closeToTray = false;            // 点关闭时隐藏到托盘（不退出）
        int  channelBorder = 0;              // 通道边框：0=无颜色边框(默认) 1=左侧颜色边框 2=全边框
        bool minimizeFolds = false;          // 最小化时卷起窗口（只留通道底部控制带），默认不勾选
        bool chinese = true;                 // 当前界面语言（由 languageMode 解析，切换时回写）
        int  languageMode = 0;               // 语言模式：0=跟随系统 1=中文 2=English
        bool lightTheme = true;              // true = 浅色（默认）/ false = 深色
        bool nodeView = false;               // 主区显示：false = 调音台 / true = 节点连接视图
        juce::String currentPreset;          // 「保存预设」覆盖的目标文件
        juce::String recordMode = "sum";     // 录音模式："sum"=合成单文件 / "multi"=分轨多文件
        juce::String recordFormat = "WAV";   // 录音格式："WAV" / "FLAC" / "AIFF"
        int recordBitDepth = 24;             // 位深：16 / 24
        juce::String recordDir;              // 录音目录（空 = 默认 音乐\MicroBridge）
        double uiScale = 1.1;                // 界面缩放：1.0 / 1.1 / 1.25 / 1.5（默认 110%）
        bool drawerCollapsed = false;        // 左侧抽屉收纳状态（重启恢复）
        bool recorderVisible = true;         // 顶部录音区显隐（重启恢复）
        int  hideHotkeyKey = 0;              // 全局「隐藏/恢复窗口」快捷键（JUCE keyCode，0 = 未设置）
        int  hideHotkeyMods = 0;             // 快捷键修饰键（JUCE raw flags）
        int  hideMode = 0;                   // 快捷键隐藏方式：0=隐藏到托盘（默认）/ 1=完全隐藏（托盘也不显示）
        bool alwaysOnTop = false;            // 主窗口始终置顶（显示在其他应用上层）
        bool desktopLyricsOnTop = true;      // 桌面歌词始终置顶（主窗口置顶时也自动置顶）
        bool desktopLyricsLocked = false;    // 桌面歌词锁定：鼠标穿透 + 位置固定，靠解锁快捷键/托盘解除
        // 全局「锁定 / 解锁桌面歌词」切换快捷键（JUCE keyCode；默认 Ctrl+Alt+L，0 = 未设置）
        int  unlockLyricsHotkeyKey  = 'L';
        int  unlockLyricsHotkeyMods = juce::ModifierKeys::ctrlModifier
                                    | juce::ModifierKeys::altModifier;
        bool desktopLyricsHasBounds = false; // 桌面歌词窗口位置/尺寸是否已保存
        double desktopLyricsX = 0, desktopLyricsY = 0, desktopLyricsW = 0, desktopLyricsH = 0;
        CardWindowState fxCard;              // 效果器浮层窗口状态
        CardWindowState mediaCard;           // 音频播放浮层窗口状态
    };

    class AppSettings
    {
    public:
        static juce::File file()
        {
            return SessionStore::dataRoot().getChildFile ("settings.json");
        }

        static juce::String runKey()
        {
            return mb::S("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\MicroBridge");
        }

        static AppSettingsData load()
        {
            AppSettingsData d;
            const auto text = file().loadFileAsString();
            if (text.isEmpty()) return d;

            const auto v = juce::JSON::parse (text);
            if (! v.isObject()) return d;
            auto* o = v.getDynamicObject();
            if (o == nullptr) return d;

            auto flag = [o] (const juce::Identifier& id, bool def)
            {
                const auto x = o->getProperty (id);
                return x.isVoid() ? def : (static_cast<int> (x) != 0);
            };
            d.autoStart            = flag ("autoStart", false);
            d.startMinimizedToTray = flag ("startMinimizedToTray", false);
            d.closeToTray          = flag ("closeToTray", false);
            // 通道边框：新字段 channelBorder 优先；旧配置只有 stripBorder 布尔时换算
            // （true=全边框 2，false=左侧颜色边框 1）；两者都没有 = 新默认 0（无颜色边框）
            const auto cb = o->getProperty ("channelBorder");
            if (cb.isVoid())
            {
                const auto sb = o->getProperty ("stripBorder");
                d.channelBorder = sb.isVoid() ? 0 : (static_cast<bool> (sb) ? 2 : 1);
            }
            else
                d.channelBorder = juce::jlimit (0, 2, static_cast<int> (cb));
            d.minimizeFolds       = flag ("minimizeFolds", false);
            d.chinese              = flag ("chinese", true);
            d.nodeView             = flag ("nodeView", false);
            d.lightTheme           = flag ("lightTheme", true);
            // 语言模式：新 key 优先；旧配置只有 chinese 布尔时按它换算
            const auto lm = o->getProperty ("languageMode");
            if (lm.isVoid())
                d.languageMode = d.chinese ? 1 : 2;      // 旧版：chinese=false → English
            else
                d.languageMode = juce::jlimit (0, 2, static_cast<int> (lm));
            if (d.languageMode == 0) d.chinese = true;   // 恢复时由 setLanguageMode 重新解析
            d.currentPreset        = o->getProperty ("currentPreset").toString();
            d.recordMode           = o->getProperty ("recordMode").toString();
            d.recordFormat         = o->getProperty ("recordFormat").toString();
            d.recordBitDepth       = static_cast<int> (o->getProperty ("recordBitDepth"));
            if (d.recordBitDepth != 16 && d.recordBitDepth != 24) d.recordBitDepth = 24;
            d.recordDir            = o->getProperty ("recordDir").toString();
            d.uiScale              = static_cast<double> (o->getProperty ("uiScale"));
            if (d.uiScale < 1.0 || d.uiScale > 2.0) d.uiScale = 1.1;
            d.drawerCollapsed      = flag ("drawerCollapsed", false);
            d.recorderVisible       = flag ("recorderVisible", true);
            const auto hk = o->getProperty ("hideHotkeyKey");
            d.hideHotkeyKey   = hk.isVoid() ? 0 : static_cast<int> (hk);
            const auto hm = o->getProperty ("hideHotkeyMods");
            d.hideHotkeyMods  = hm.isVoid() ? 0 : static_cast<int> (hm);
            const auto hmode = o->getProperty ("hideMode");
            d.hideMode        = hmode.isVoid() ? 0 : juce::jlimit (0, 1, static_cast<int> (hmode));
            d.alwaysOnTop     = flag ("alwaysOnTop", false);
            d.desktopLyricsOnTop = flag ("desktopLyricsOnTop", true);
            d.desktopLyricsLocked = flag ("desktopLyricsLocked", false);
            const auto uk = o->getProperty ("unlockLyricsHotkeyKey");
            d.unlockLyricsHotkeyKey  = uk.isVoid() ? 'L' : static_cast<int> (uk);
            const auto um = o->getProperty ("unlockLyricsHotkeyMods");
            d.unlockLyricsHotkeyMods = um.isVoid()
                ? static_cast<int> (juce::ModifierKeys::ctrlModifier
                                    | juce::ModifierKeys::altModifier)
                : static_cast<int> (um);
            d.desktopLyricsHasBounds = flag ("desktopLyricsHasBounds", false);
            auto lyricNum = [o] (const char* k)
            {
                return static_cast<double> (o->getProperty (juce::Identifier (k)));
            };
            d.desktopLyricsX = lyricNum ("desktopLyricsX");
            d.desktopLyricsY = lyricNum ("desktopLyricsY");
            d.desktopLyricsW = lyricNum ("desktopLyricsW");
            d.desktopLyricsH = lyricNum ("desktopLyricsH");
            if (! (d.desktopLyricsW >= 200.0 && d.desktopLyricsW <= 4000.0
                   && d.desktopLyricsH >= 30.0 && d.desktopLyricsH <= 400.0))
                d.desktopLyricsHasBounds = false;
            auto readCard = [o] (const char* key)
            {
                CardWindowState c;
                if (auto* co = o->getProperty (juce::Identifier (key)).getDynamicObject())
                {
                    auto num = [co] (const char* k)
                    {
                        return static_cast<double> (co->getProperty (juce::Identifier (k)));
                    };
                    c.hasSize   = static_cast<int> (co->getProperty ("hasSize"))  != 0;
                    c.hasPos    = static_cast<int> (co->getProperty ("hasPos"))   != 0;
                    c.w = num ("w"); c.h = num ("h");
                    c.x = num ("x"); c.y = num ("y");
                    c.minimised = static_cast<int> (co->getProperty ("minimised")) != 0;
                    c.maximised = static_cast<int> (co->getProperty ("maximised")) != 0;
                    if (! (c.w >= 100.0 && c.w <= 20000.0 && c.h >= 100.0 && c.h <= 20000.0))
                        c.hasSize = false;
                    if (c.x < -20000.0 || c.x > 20000.0 || c.y < -20000.0 || c.y > 20000.0)
                        c.hasPos = false;
                    if (c.minimised && c.maximised) c.maximised = false;
                }
                return c;
            };
            d.fxCard    = readCard ("fxCard");
            d.mediaCard = readCard ("mediaCard");
            return d;
        }

        static void save (const AppSettingsData& d)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("autoStart", d.autoStart);
            o->setProperty ("startMinimizedToTray", d.startMinimizedToTray);
            o->setProperty ("closeToTray", d.closeToTray);
            o->setProperty ("channelBorder", d.channelBorder);
            o->setProperty ("stripBorder", d.channelBorder == 2);   // 旧字段兼容
            o->setProperty ("minimizeFolds", d.minimizeFolds);
            o->setProperty ("chinese", d.chinese);
            o->setProperty ("languageMode", d.languageMode);
            o->setProperty ("lightTheme", d.lightTheme);
            o->setProperty ("nodeView", d.nodeView);
            o->setProperty ("currentPreset", d.currentPreset);
            o->setProperty ("recordMode", d.recordMode);
            o->setProperty ("recordFormat", d.recordFormat);
            o->setProperty ("recordBitDepth", d.recordBitDepth);
            o->setProperty ("recordDir", d.recordDir);
            o->setProperty ("uiScale", d.uiScale);
            o->setProperty ("drawerCollapsed", d.drawerCollapsed);
            o->setProperty ("recorderVisible", d.recorderVisible);
            o->setProperty ("hideHotkeyKey", d.hideHotkeyKey);
            o->setProperty ("hideHotkeyMods", d.hideHotkeyMods);
            o->setProperty ("hideMode", d.hideMode);
            o->setProperty ("alwaysOnTop", d.alwaysOnTop);
            o->setProperty ("desktopLyricsOnTop", d.desktopLyricsOnTop);
            o->setProperty ("desktopLyricsLocked", d.desktopLyricsLocked);
            o->setProperty ("unlockLyricsHotkeyKey", d.unlockLyricsHotkeyKey);
            o->setProperty ("unlockLyricsHotkeyMods", d.unlockLyricsHotkeyMods);
            o->setProperty ("desktopLyricsHasBounds", d.desktopLyricsHasBounds);
            o->setProperty ("desktopLyricsX", d.desktopLyricsX);
            o->setProperty ("desktopLyricsY", d.desktopLyricsY);
            o->setProperty ("desktopLyricsW", d.desktopLyricsW);
            o->setProperty ("desktopLyricsH", d.desktopLyricsH);
            auto writeCard = [] (const CardWindowState& c)
            {
                auto* co = new juce::DynamicObject();
                co->setProperty ("hasSize",   c.hasSize);
                co->setProperty ("hasPos",    c.hasPos);
                co->setProperty ("w", c.w);  co->setProperty ("h", c.h);
                co->setProperty ("x", c.x);  co->setProperty ("y", c.y);
                co->setProperty ("minimised", c.minimised);
                co->setProperty ("maximised", c.maximised);
                return juce::var (co);
            };
            o->setProperty ("fxCard",    writeCard (d.fxCard));
            o->setProperty ("mediaCard", writeCard (d.mediaCard));
            file().getParentDirectory().createDirectory();
            file().replaceWithText (juce::JSON::toString (juce::var (o), true));
        }

        //======================================================================
        static bool isAutoStartEnabled()
        {
           #if JUCE_WINDOWS
            return juce::WindowsRegistry::valueExists (runKey());
           #else
            return false;
           #endif
        }

        /** 打开：把当前 exe 全路径写入 HKCU Run；关闭：删除该值。 */
        static void applyAutoStart (bool on)
        {
           #if JUCE_WINDOWS
            if (on)
            {
                const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                    .getFullPathName();
                juce::WindowsRegistry::setValue (runKey(), mb::S("\"") + exe + mb::S("\""));
            }
            else
            {
                juce::WindowsRegistry::deleteValue (runKey());
            }
           #else
            juce::ignoreUnused (on);
           #endif
        }
    };
}
