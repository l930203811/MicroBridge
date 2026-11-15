#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <string>

#include "RouteGraph.h"

//==============================================================================
// 编码安全 + 中英双语层
//
// 乱码根因：JUCE 8 的 juce::String (const char*) 按「ASCII 逐字节」构造，
// 源码里的中文 UTF-8 字节（E5 BE AE ...）被当成 Latin-1 码点，于是显示成
// "å¾®éŸ³æ¡¥"。所有界面字面量必须经 S() / tr() 包装，禁止直接传裸字符串。
//==============================================================================
namespace mb
{
    /** UTF-8 字面量 -> juce::String（唯一允许的入口）。 */
    inline juce::String S (const char* utf8)
    {
        return juce::String (juce::CharPointer_UTF8 (utf8));
    }

    inline juce::String S (const std::string& utf8) { return S (utf8.c_str()); }

    // ---- 语言状态（默认跟随系统；系统非中文一律英文）------------------------
    inline bool& languageFlag() noexcept
    {
        static bool chinese = true;
        return chinese;
    }
    inline bool isZh() noexcept { return languageFlag(); }
    inline void setChinese (bool on) noexcept { languageFlag() = on; }

    /** 语言模式：0 = 跟随系统 / 1 = 简体中文 / 2 = English。 */
    inline int& languageMode() noexcept
    {
        static int m = 0;
        return m;
    }

    /** 系统首选语言是否中文（非中文环境——含其他语种——一律回退英文）。 */
    inline bool systemPrefersChinese() noexcept
    {
        return juce::SystemStats::getUserLanguage().startsWith ("zh");
    }

    /** 设置语言模式并解析出实际界面语言；语言切换的唯一入口。 */
    inline void setLanguageMode (int mode) noexcept
    {
        languageMode() = juce::jlimit (0, 2, mode);
        languageFlag() = mode == 1
                      || (mode == 0 && systemPrefersChinese());
    }

    /** 「跟随系统」实际会落到哪种语言的展示名（欢迎页按钮副标题用）。 */
    inline juce::String systemLanguageName()
    {
        return systemPrefersChinese() ? S("简体中文") : S("English");
    }

    /** 中英取词。仅用于界面文案；设备名 / 驱动名 / 文件名一律不翻译。 */
    inline juce::String tr (const char* zh, const char* en)
    {
        return S (isZh() ? zh : en);
    }

    /** 应用名：中文界面只显示「微音桥」，英文界面只显示 MicroBridge。 */
    inline juce::String appName() { return tr ("微音桥", "MicroBridge"); }

    struct NamedItem { const char* zh; const char* en; };

    inline constexpr int kMaxNamedChannels = 12;

    inline juce::String channelName (int index)
    {
        static const NamedItem names[kMaxNamedChannels] =
        {
            { "系统",   "System"   }, { "伴奏",   "Backing"  },
            { "麦克风", "Mic"      }, { "聊天",   "Chat"     },
            { "唱歌",   "Vocal"    }, { "混响",   "Reverb"   },
            // 6/7/8 与「重置路由」的直播默认配线配套：直播总线 / 语音总线 / 监听
            { "总线",   "Bus"      }, { "语音",   "Voice"    },
            { "监听",   "Monitor"  }, { "线路",   "Line In"  },
            { "游戏",   "Game"     }, { "浏览器", "Browser"  }
        };

        if (index >= 0 && index < kMaxNamedChannels)
            return tr (names[index].zh, names[index].en);

        return S("CH") + juce::String (index + 1);
    }

    // ---- 用户自定义通道名（重命名后优先显示；空串回退到默认名）------------
    inline juce::StringArray& customChannelNames()
    {
        static juce::StringArray a;
        return a;
    }

    inline juce::String channelDisplayName (int index)
    {
        const auto& a = customChannelNames();
        if (index >= 0 && index < a.size() && a[index].isNotEmpty())
            return a[index];
        return channelName (index);
    }

    // ---- 用户自定义通道图标（空串 = 默认无图标；与名字 / 颜色同路径持久化）----
    inline juce::StringArray& customChannelIcons()
    {
        static juce::StringArray a;
        return a;
    }

    /** 取某通道当前图标 id；越界 / 空串一律回退为空（默认无图标）。 */
    inline juce::String channelIcon (int index)
    {
        const auto& a = customChannelIcons();
        if (index >= 0 && index < a.size()) return a[index];
        return {};
    }

    inline juce::String edgeKindLabel (EdgeKind kind)
    {
        switch (kind)
        {
            case EdgeKind::InputToChannel:   return tr ("输入源", "INPUT SOURCE");
            case EdgeKind::ChannelToChannel: return tr ("通道互送", "CHANNEL SEND");
            case EdgeKind::ChannelToOutput:  return tr ("输出目标", "OUTPUT TARGET");
        }
        return {};
    }

    /** 路由条目的短标签，供通道条绘制使用。 */
    inline juce::String edgeTag (EdgeKind kind, int id, const juce::String& label)
    {
        juce::String prefix;
        switch (kind)
        {
            case EdgeKind::InputToChannel:   prefix = S("IN")  + juce::String (id + 1); break;
            case EdgeKind::ChannelToChannel: prefix = S("CH")  + juce::String (id + 1); break;
            case EdgeKind::ChannelToOutput:  prefix = S("OUT") + juce::String (id + 1); break;
        }
        return prefix + S("  ") + label;
    }

    inline juce::String gainLabel (float gain)
    {
        if (gain <= 0.00001f) return tr ("静音", "-inf");
        const float db = 20.0f * std::log10 (gain);
        return juce::String (db, 1) + S(" dB");
    }
}
