#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>

#include "AppSettings.h"
#include "DeviceSetup.h"
#include "I18n.h"
#include "PluginPanel.h"
#include "Theme.h"

//==============================================================================
// 设置页（全屏遮罩浮层）
//
// 从左侧抽屉迁入：设备设置 / 中英切换 / 诊断 / 插件库。
// 「设备设置」「插件库」为内嵌 Tab：直接承载页面本体，不再额外弹窗。
// 新增常用选项：开机自启、启动后隐藏到托盘、关闭时隐藏到托盘、通道边框样式。
// 所有改动即时写盘，由 MainComponent 负责应用到运行时（注册表 / 托盘 / 重绘）。
//==============================================================================
class SettingsPanel : public juce::Component
{
public:
    std::function<void()> onClose, onDiagnostics, onLanguageToggle;
    std::function<void()> onLaunchNewInstance, onRestartApp;   // 另起 / 重启微音桥
    std::function<void (bool)> onLightTheme;                   // 浅色模式开关（true = 浅色）
    std::function<void (double)> onRecordSampleRate;   // 0 = 跟随设备
    std::function<void (double, double)> onUiScaleChanged;   // (新缩放, 旧缩放)
    std::function<void (mb::AppSettingsData)> onSettingsChanged;
    std::function<void (bool)> onHotkeyCapture;              // 录入期间抑制全局钩子

    DeviceSetupOverlay* devicePage() { return devicePage_.get(); }
    PluginPanel* pluginPage()        { return pluginPage_.get(); }

    explicit SettingsPanel (mb::AppSettingsData data, double deviceSr, mb::AudioEngine& engine)
        : data_ (data), deviceSr_ (deviceSr)
    {
        // 内嵌页：设备设置 / 插件库（plainMode = 无遮罩，铺满 Tab 内容区）
        devicePage_ = std::make_unique<DeviceSetupOverlay> (engine, engine.savedChoice(), false);
        devicePage_->setPlainMode (true);
        addAndMakeVisible (devicePage_.get());
        devicePage_->setVisible (false);

        pluginPage_ = std::make_unique<PluginPanel> (engine.pluginCatalog());
        pluginPage_->setPlainMode (true);
        addAndMakeVisible (pluginPage_.get());
        pluginPage_->setVisible (false);

        auto styleToggle = [this] (juce::ToggleButton& b)
        {
            styleToggleColour (b);
            addAndMakeVisible (b);
        };
        styleToggle (autoStart_);
        styleToggle (startTray_);
        styleToggle (closeTray_);
        styleToggle (minimizeFolds_);
        styleToggle (alwaysOnTop_);
        styleToggle (desktopLyricsOnTop_);
        styleToggle (borderNone_);
        styleToggle (borderLeft_);
        styleToggle (borderFull_);
        styleToggle (hideTrayMode_);
        styleToggle (hideFullMode_);
        borderNone_.setRadioGroupId (101);   // 三态互斥：无颜色边框 / 左侧 / 全边框
        borderLeft_.setRadioGroupId (101);
        borderFull_.setRadioGroupId (101);
        hideTrayMode_.setRadioGroupId (102); // 隐藏方式二选一：托盘 / 完全隐藏
        hideFullMode_.setRadioGroupId (102);

        auto styleButton = [this] (juce::TextButton& b)
        {
            styleButtonColour (b);
            addAndMakeVisible (b);
        };
        styleButton (langBtn_);
        styleButton (diagBtn_);
        styleButton (dirBtn_);
        styleButton (openDirBtn_);
        styleButton (closeBtn_);
        styleButton (applyDeviceBtn_);
        styleButton (newInstanceBtn_);
        styleButton (restartBtn_);
        styleButton (hotkeyBtn_);
        styleButton (unlockHotkeyBtn_);
        styleButton (lightTheme_);          // 深浅模式：切换按钮（与抽屉入口同步）
        for (int i = 0; i < kTabCount; ++i)  // 分类 Tab：常规 / 界面 / 录音 / 设备设置 / 插件库 / 其他 / 快捷键
        {
            styleButton (tabBtns_[i]);
            tabBtns_[i].onClick = [this, i] { setTab (i); };
        }
        refreshTabStyles();
        setWantsKeyboardFocus (true);       // 快捷键录入需要键盘焦点

        // 两条系统级快捷键共用一套录入态：captureTarget_ 1=隐藏窗口 / 2=解锁桌面歌词
        hotkeyBtn_.setWantsKeyboardFocus (false);
        unlockHotkeyBtn_.setWantsKeyboardFocus (false);
        hotkeyBtn_.onClick        = [this] { startCapture (1); };
        unlockHotkeyBtn_.onClick  = [this] { startCapture (2); };

        auto styleCombo = [this] (juce::ComboBox& c)
        {
            styleComboColour (c);
            addAndMakeVisible (c);
        };
        styleCombo (fmtCombo_);
        styleCombo (bitsCombo_);
        styleCombo (srCombo_);
        styleCombo (modeCombo_);
        styleCombo (scaleCombo_);

        autoStart_.setToggleState (data_.autoStart, juce::dontSendNotification);
        startTray_.setToggleState (data_.startMinimizedToTray, juce::dontSendNotification);
        closeTray_.setToggleState (data_.closeToTray, juce::dontSendNotification);
        minimizeFolds_.setToggleState (data_.minimizeFolds, juce::dontSendNotification);
        alwaysOnTop_.setToggleState (data_.alwaysOnTop, juce::dontSendNotification);
        desktopLyricsOnTop_.setToggleState (data_.desktopLyricsOnTop, juce::dontSendNotification);
        borderNone_.setToggleState (data_.channelBorder == 0, juce::dontSendNotification);
        borderLeft_.setToggleState (data_.channelBorder == 1, juce::dontSendNotification);
        borderFull_.setToggleState (data_.channelBorder == 2, juce::dontSendNotification);
        lightTheme_.setToggleState (data_.lightTheme, juce::dontSendNotification);
        hideTrayMode_.setToggleState (data_.hideMode == 0, juce::dontSendNotification);
        hideFullMode_.setToggleState (data_.hideMode == 1, juce::dontSendNotification);
        refreshHotkeyText();

        // 录音设置初值
        fmtCombo_.addItem (mb::S("WAV"), 1);
        fmtCombo_.addItem (mb::S("FLAC"), 2);
        fmtCombo_.addItem (mb::S("AIFF"), 3);
        bitsCombo_.addItem (mb::S("16 bit"), 1);
        bitsCombo_.addItem (mb::S("24 bit"), 2);
        fillSrItems();
        modeCombo_.addItem (mb::tr ("多轨合成单文件", "Mix to one file"), 1);
        modeCombo_.addItem (mb::tr ("分轨多文件", "Multi-track files"), 2);
        fmtCombo_.setSelectedId (data_.recordFormat == "FLAC" ? 2 : data_.recordFormat == "AIFF" ? 3 : 1,
                                 juce::dontSendNotification);
        bitsCombo_.setSelectedId (data_.recordBitDepth == 16 ? 1 : 2, juce::dontSendNotification);
        modeCombo_.setSelectedId (data_.recordMode == "multi" ? 2 : 1, juce::dontSendNotification);
        dirBtn_.setButtonText (mb::tr ("更改目录…", "Change folder…"));
        openDirBtn_.setButtonText (mb::tr ("打开目录", "Open folder"));

        auto changed = [this]
        {
            data_.autoStart            = autoStart_.getToggleState();
            data_.startMinimizedToTray = startTray_.getToggleState();
            data_.closeToTray          = closeTray_.getToggleState();
            data_.minimizeFolds        = minimizeFolds_.getToggleState();
            data_.alwaysOnTop          = alwaysOnTop_.getToggleState();
            data_.desktopLyricsOnTop   = desktopLyricsOnTop_.getToggleState();
            data_.channelBorder       = borderFull_.getToggleState() ? 2
                                       : borderLeft_.getToggleState() ? 1 : 0;
            if (onSettingsChanged) onSettingsChanged (data_);
        };
        auto recChanged = [this]
        {
            data_.recordMode     = modeCombo_.getSelectedId() == 2 ? "multi" : "sum";
            data_.recordFormat   = fmtCombo_.getText().toUpperCase();
            data_.recordBitDepth = bitsCombo_.getSelectedId() == 1 ? 16 : 24;
            if (onSettingsChanged) onSettingsChanged (data_);
        };
        autoStart_.onClick = changed;
        startTray_.onClick = changed;
        closeTray_.onClick = changed;
        minimizeFolds_.onClick = changed;
        alwaysOnTop_.onClick = changed;
        desktopLyricsOnTop_.onClick = changed;
        borderNone_.onClick = changed;
        borderLeft_.onClick = changed;
        borderFull_.onClick = changed;
        auto hideModeChanged = [this]
        {
            data_.hideMode = hideFullMode_.getToggleState() ? 1 : 0;
            if (onSettingsChanged) onSettingsChanged (data_);   // MainComponent 同步托盘与热键
        };
        hideTrayMode_.onClick = hideModeChanged;
        hideFullMode_.onClick = hideModeChanged;
        lightTheme_.onClick = [this]
        {
            data_.lightTheme = ! data_.lightTheme;
            if (onLightTheme) onLightTheme (data_.lightTheme);
        };
        fmtCombo_.onChange  = recChanged;
        bitsCombo_.onChange = recChanged;
        modeCombo_.onChange = recChanged;
        srCombo_.onChange   = [this]
        {
            const double sr = srCombo_.getSelectedId() == 2 ? 44100.0
                            : srCombo_.getSelectedId() == 3 ? 48000.0
                            : srCombo_.getSelectedId() == 4 ? 96000.0 : 0.0;
            if (onRecordSampleRate) onRecordSampleRate (sr);
        };
        scaleCombo_.onChange = [this]
        {
            static const double vals[] = { 1.0, 1.1, 1.25, 1.5 };
            const int id = scaleCombo_.getSelectedId();
            if (id < 1 || id > 4) return;
            const double prev = juce::Desktop::getInstance().getGlobalScaleFactor();
            data_.uiScale = vals[id - 1];
            juce::Desktop::getInstance().setGlobalScaleFactor (data_.uiScale);
            if (onUiScaleChanged) onUiScaleChanged (data_.uiScale, prev);
            if (onSettingsChanged) onSettingsChanged (data_);
        };

        langBtn_.onClick   = [this] { if (onLanguageToggle) onLanguageToggle(); };
        diagBtn_.onClick    = [this] { if (onDiagnostics) onDiagnostics(); };
        closeBtn_.onClick   = [this] { if (onClose) onClose(); };
        applyDeviceBtn_.onClick = [this]
        {
            if (devicePage_ != nullptr) devicePage_->tryApply();   // 内嵌设备页统一应用入口
        };
        newInstanceBtn_.onClick = [this] { if (onLaunchNewInstance) onLaunchNewInstance(); };
        restartBtn_.onClick     = [this] { if (onRestartApp) onRestartApp(); };
        openDirBtn_.onClick = [this]
        {
            const auto d = recordDirectory();
            d.createDirectory();
            d.startAsProcess();
        };
        dirBtn_.onClick = [this]
        {
            chooser_ = std::make_unique<juce::FileChooser> (
                mb::tr ("选择录音保存目录", "Choose recording folder"), recordDirectory());
            chooser_->launchAsync (juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::FileChooser& fc)
                {
                    const auto d = fc.getResult();
                    if (! d.isDirectory()) return;
                    data_.recordDir = d.getFullPathName();
                    if (onSettingsChanged) onSettingsChanged (data_);
                });
        };

        applyLanguage();
    }

    void applyLanguage()
    {
        tabBtns_[0].setButtonText (mb::tr ("常规", "General"));
        tabBtns_[1].setButtonText (mb::tr ("界面", "Appearance"));
        tabBtns_[2].setButtonText (mb::tr ("录音", "Recording"));
        tabBtns_[3].setButtonText (mb::tr ("设备设置", "Devices"));
        tabBtns_[4].setButtonText (mb::tr ("插件库", "Plugins"));
        tabBtns_[5].setButtonText (mb::tr ("快捷键", "Hotkeys"));
        tabBtns_[6].setButtonText (mb::tr ("其他", "Other"));
        refreshHotkeyText();
        autoStart_.setButtonText (mb::tr ("开机自动启动", "Launch at Windows startup"));
        startTray_.setButtonText (mb::tr ("启动后隐藏到托盘", "Start minimized to tray"));
        closeTray_.setButtonText (mb::tr ("关闭窗口时隐藏到托盘（不退出）",
                                          "Hide to tray instead of quitting"));
        minimizeFolds_.setButtonText (mb::tr ("最小化时折叠窗口（卷起·只留底部）",
                                              "Fold window on minimise (roll-up)"));
        alwaysOnTop_.setButtonText (mb::tr ("窗口始终置顶（显示在其他应用上层）",
                                            "Keep window always on top of other apps"));
        desktopLyricsOnTop_.setButtonText (mb::tr ("桌面歌词始终置顶",
                                                   "Keep desktop lyrics always on top"));
        borderNone_.setButtonText (mb::tr ("无颜色边框", "No colour outline"));
        borderLeft_.setButtonText (mb::tr ("左侧颜色边框", "Left colour outline"));
        borderFull_.setButtonText (mb::tr ("全边框（覆盖顶部与右侧）", "Full outline (top & right)"));
        hideTrayMode_.setButtonText (mb::tr ("隐藏到托盘（托盘保留图标，可点击恢复）",
                                             "Hide to tray (icon kept, click to restore)"));
        hideFullMode_.setButtonText (mb::tr ("完全隐藏（托盘也不显示，仅快捷键找回）",
                                             "Hide completely (no tray icon — hotkey only)"));
        lightTheme_.setButtonText (data_.lightTheme
                                       ? mb::tr ("切换到深色模式", "Switch to dark theme")
                                       : mb::tr ("切换到浅色模式", "Switch to light theme"));
        langBtn_.setButtonText (mb::isZh() ? mb::S("Switch to English")
                                           : mb::S("切换到中文"));
        diagBtn_.setButtonText    (mb::tr ("诊断", "Diagnostics"));
        dirBtn_.setButtonText     (mb::tr ("更改目录…", "Change folder…"));
        openDirBtn_.setButtonText (mb::tr ("打开目录", "Open folder"));
        newInstanceBtn_.setButtonText (mb::tr ("启动新实例", "New instance"));
        restartBtn_.setButtonText     (mb::tr ("重启应用", "Restart app"));
        restartBtn_.setTooltip (mb::tr ("保存会话后关闭当前实例并重新打开",
                                        "Save session, quit and relaunch"));
        newInstanceBtn_.setTooltip (mb::tr ("以新进程再启动一份应用（互斥保护不受影响）",
                                            "Start another app process"));
        scaleCombo_.setTooltip (mb::tr ("界面整体缩放，字体与控件同步放大",
                                        "Global UI scale, fonts and controls enlarge together"));
        fillScaleItems();
        closeBtn_.setButtonText   (mb::tr ("关闭", "Close"));
        applyDeviceBtn_.setButtonText (mb::tr ("应用设备设置", "Apply device"));
        fillModeItems();
        fillSrItems();
        if (devicePage_ != nullptr) devicePage_->applyLanguage();   // 内嵌设备页同步语言
        repaint();
    }

    /** 界面缩放下拉：100% / 110% / 125% / 150%。 */
    void fillScaleItems()
    {
        static const double vals[] = { 1.0, 1.1, 1.25, 1.5 };
        scaleCombo_.clear (juce::dontSendNotification);
        for (int i = 0; i < 4; ++i)
            scaleCombo_.addItem (juce::String (juce::roundToInt (vals[i] * 100.0)) + "%", i + 1);
        int sel = 1;
        for (int i = 0; i < 4; ++i)
            if (std::abs (data_.uiScale - vals[i]) < 0.01) sel = i + 1;
        scaleCombo_.setSelectedId (sel, juce::dontSendNotification);
    }

    /** 录音模式下拉：两项是翻译文案，切换语言时要重建并保留选中项。 */
    void fillModeItems()
    {
        const int sel = modeCombo_.getSelectedId();
        modeCombo_.clear (juce::dontSendNotification);
        modeCombo_.addItem (mb::tr ("多轨合成单文件", "Mix to one file"), 1);
        modeCombo_.addItem (mb::tr ("分轨多文件", "Multi-track files"), 2);
        modeCombo_.setSelectedId (sel > 0 ? sel : 1, juce::dontSendNotification);
    }

    /** 采样率下拉：跟随设备（显示设备实际值）/ 44.1 / 48 / 96 kHz。 */
    void fillSrItems()
    {
        srCombo_.clear (juce::dontSendNotification);
        srCombo_.addItem (mb::tr ("跟随设备", "Follow device")
                          + (deviceSr_ > 8000.0
                             ? mb::S(" (") + juce::String (deviceSr_ / 1000.0, 1) + mb::S(" kHz)") : juce::String()),
                          1);
        srCombo_.addItem (mb::S("44.1 kHz"), 2);
        srCombo_.addItem (mb::S("48 kHz"), 3);
        srCombo_.addItem (mb::S("96 kHz"), 4);
        srCombo_.setSelectedId (1, juce::dontSendNotification);
    }

    juce::File recordDirectory() const
    {
        if (data_.recordDir.isNotEmpty()) return juce::File (data_.recordDir);
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                .getChildFile ("MicroBridge");
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.42f));

        const auto card = cardBounds();
        g.setColour (mb::ui::card);
        g.fillRoundedRectangle (card, 10.0f);
        g.setColour (mb::ui::border);
        g.drawRoundedRectangle (card, 10.0f, 1.2f);

        const auto c = card.toNearestInt();
        g.setFont (mb::ui::font (15.0f, true));
        g.setColour (mb::ui::text);
        g.drawText (mb::tr ("设置", "Settings") + mb::S("   ·   MicroBridge"),
                    c.withHeight (34).withTrimmedLeft (18),
                    juce::Justification::centredLeft, false);

        g.setFont (mb::ui::font (8.5f));
        g.setColour (mb::ui::subText);
        g.drawText (mb::tr ("改动即时保存 · 关闭窗口不影响音频运行",
                            "Changes save instantly · closing won't stop audio"),
                    c.withY (c.getY() + 32).withHeight (16).withTrimmedLeft (18),
                    juce::Justification::centredLeft, false);

        // Tab 栏底部分隔线 + 纵向 Tab 栏与内容区之间的分隔线
        g.setColour (mb::ui::divider);
        g.fillRect (c.getX() + 18, c.getY() + 80, c.getWidth() - 36, 1);
        g.fillRect (contentX (c) - 12, c.getY() + 90, 1, c.getHeight() - 102);

        const int py = c.getY() + tabPageY();   // 与 resized() 同基准（此前少加卡片偏移导致小字被按钮盖住）
        const int cx = contentX (c);            // 内容区起点（纵向 Tab 栏右侧）

        // 普通页内容面板：与内嵌页（插件库 / 设备设置）一致的「面板底色 + 描边线条」
        if (activeTab_ == 0 || activeTab_ == 1 || activeTab_ == 2
            || activeTab_ == 5 || activeTab_ == 6)
        {
            const auto panel = juce::Rectangle<float> (static_cast<float> (c.getX() + 100),
                                                       static_cast<float> (c.getY() + 88),
                                                       static_cast<float> (c.getRight() - 16 - c.getX() - 100),
                                                       static_cast<float> (c.getBottom() - 12 - c.getY() - 88));
            g.setColour (mb::ui::sectionBg);
            g.fillRoundedRectangle (panel, 10.0f);
            g.setColour (mb::ui::border);
            g.drawRoundedRectangle (panel.reduced (0.5f), 10.0f, 1.0f);
        }

        // 各 Tab 内容标签（分组大标题已去掉，与内嵌 Tab 保持一致）
        switch (activeTab_)
        {
        case 0:                             // 常规：置顶说明
        {
            const int cw = c.getRight() - 16 - cx;
            g.setFont (mb::ui::font (9.0f, true));
            g.setColour (mb::ui::subText);
            g.drawText (mb::tr ("窗口置顶", "Always on top"),
                        cx, py + 190, cw, 16, juce::Justification::centredLeft, false);
            g.setFont (mb::ui::font (9.0f));
            g.drawText (mb::tr ("主窗口置顶后，桌面歌词也会一并置顶；也可单独勾选桌面歌词置顶。"
                                "桌面歌词由播放器「歌词」左侧按钮打开，拖动窗口改位置，拖右下角改大小。",
                                "Top-most main window also pins desktop lyrics; you can pin lyrics separately. "
                                "Open desktop lyrics from the button left of 'Lyrics' in the player, drag to move, "
                                "drag the bottom-right corner to resize."),
                        cx, py + 210, cw, 46, juce::Justification::topLeft, true);
            break;
        }
        case 1:
        {
            const int cw = c.getRight() - 16 - cx;
            const int colW = (cw - 24) / 2;
            const int x2 = cx + colW + 24;
            auto fLabel = [&] (int fx, int fy, int fw, const char* zh, const char* en)
            {
                g.setFont (mb::ui::font (9.0f, true));
                g.setColour (mb::ui::subText);
                g.drawText (mb::tr (zh, en), fx, fy, fw, 16, juce::Justification::centredLeft, false);
            };
            auto fNote = [&] (int fx, int fy, int fw, const char* zh, const char* en)
            {
                g.setFont (mb::ui::font (9.0f));
                g.setColour (mb::ui::subText);
                g.drawText (mb::tr (zh, en), fx, fy, fw, 16, juce::Justification::centredLeft, true);
            };
            // 行1：界面语言 | 界面缩放
            fLabel (cx, py + 24, colW, "界面语言", "Language");
            fLabel (x2, py + 24, colW, "界面缩放", "Scale");
            fNote  (cx, py + 78, colW, "在中文与英文界面之间切换", "Switch between Chinese and English");
            fNote  (x2, py + 78, colW, "字体与控件同步缩放，窗口自动适配", "Fonts and controls scale together");
            // 行2：通道边框样式（三态单选）
            fLabel (cx, py + 130, cw, "通道边框样式", "Channel outline style");
            fNote  (cx, py + 216, cw,
                    "无颜色边框（默认）：隐藏左侧纵线；左侧颜色边框：仅左侧细线；"
                    "全边框：色线覆盖顶部与右侧。顶部圆点已全面移除。",
                    "No outline (default): hide left line; Left: left line only; "
                    "Full: colour line on top and right. Top dot removed everywhere.");
            // 行3：主题模式
            fLabel (cx, py + 250, colW, "主题模式", "Theme");
            fNote  (cx, py + 322, colW, "深浅色即时生效，与抽屉入口同步", "Applies instantly; synced with drawer toggle");
            break;
        }
        case 2:
        {
            const int cw = c.getRight() - 16 - cx;
            const int colW = (cw - 48) / 3;
            const int x2 = cx + colW + 24, x3 = x2 + colW + 24;
            auto fLabel = [&] (int fx, int fy, int fw, const char* zh, const char* en)
            {
                g.setFont (mb::ui::font (9.0f, true));
                g.setColour (mb::ui::subText);
                g.drawText (mb::tr (zh, en), fx, fy, fw, 16, juce::Justification::centredLeft, false);
            };
            auto fNote = [&] (int fx, int fy, int fw, const juce::String& text)
            {
                g.setFont (mb::ui::font (9.0f));
                g.setColour (mb::ui::subText);
                g.drawText (text, fx, fy, fw, 30, juce::Justification::topLeft, true);
            };
            // 行1：格式 | 位深 | 采样率
            fLabel (cx, py + 24, colW, "格式", "Format");
            fLabel (x2, py + 24, colW, "位深", "Depth");
            fLabel (x3, py + 24, colW, "采样率", "Sample rate");
            // 行2：模式 | 保存目录
            fLabel (cx, py + 130, colW, "模式", "Mode");
            fLabel (x2, py + 130, cw - (x2 - cx), "保存目录", "Save folder");
            fNote (cx, py + 190, colW * 2 + 24,
                   mb::tr ("「合成单文件」把所有布防通道混合为一个文件；「分轨多文件」每个布防通道单独保存。",
                           "'Mix to one file' merges all armed channels; 'Multi-track' saves each channel separately."));
            fNote (x2, py + 190, cw - (x2 - cx),
                   mb::tr ("当前目录：", "Current folder: ") + recordDirectory().getFullPathName());
            fNote (cx, py + 240, cw,
                   mb::tr ("采样率由音频设备决定，选择固定值会请求设备切换（设备不支持时自动回退）。",
                           "Sample rate follows the audio device; a fixed value requests a device switch when unsupported."));
            break;
        }
        case 5:                             // 快捷键（排在「其他」上方）
        {
            const int cw = c.getRight() - 16 - cx;
            const int colW2 = (cw - 24) / 2;
            const int x2 = cx + colW2 + 24;
            const int noteW = cw - 20;      // 右侧留白：说明文字不再贴边 / 越过卡片边框

            // 标题：主文字色（比原来的 subText 亮）+ 更大字号加粗
            auto fLabel = [&] (int fx, int fy, int fw, const char* zh, const char* en)
            {
                g.setFont (mb::ui::font (11.5f, true));
                g.setColour (mb::ui::text);
                g.drawText (mb::tr (zh, en), fx, fy, fw, 20,
                            juce::Justification::centredLeft, false);
            };
            // 说明：按宽度自动换行（中文按字、英文按词），并裁剪在矩形内，绝不越出右边框
            auto fNote = [&] (int fx, int fy, int fw, int fh,
                              const char* zh, const char* en)
            {
                juce::AttributedString s (mb::tr (zh, en));
                s.setFont (mb::ui::font (10.5f));
                s.setColour (mb::ui::text.withAlpha (0.86f));
                s.setWordWrap (mb::isZh() ? juce::AttributedString::byChar
                                          : juce::AttributedString::byWord);
                s.setJustification (juce::Justification::topLeft);
                const auto area = juce::Rectangle<int> (fx, fy, fw, fh);
                g.saveState();
                g.reduceClipRegion (area);
                s.draw (g, area.toFloat());
                g.restoreState();
            };

            // 行1：左「隐藏 / 恢复窗口快捷键」，右「隐藏方式」（两个单选项）
            fLabel (cx, py + 20, colW2, "隐藏 / 恢复窗口快捷键", "Hide / restore window hotkey");
            fLabel (x2, py + 20, colW2, "隐藏方式", "Hide mode");
            fNote  (x2, py + 98, colW2, 50,
                    "托盘：托盘保留图标，点击即可恢复；完全隐藏：托盘也不显示，只能靠快捷键找回窗口。",
                    "Tray: the tray icon stays to restore the window; Full: no tray icon at all — only the hotkey brings it back.");
            // 行2：左「锁定 / 解锁桌面歌词快捷键」——放在隐藏窗口快捷键的正下方
            fLabel (cx, py + 86, colW2, "锁定 / 解锁桌面歌词快捷键", "Lock / unlock desktop lyrics hotkey");
            fNote  (cx, py + 146, colW2, 40,
                    "两条快捷键在任意程序前台按下均生效；点击按钮后输入新组合键（Esc 取消 / Del 清除）。",
                    "Both hotkeys work while any app is in front; click a button then press a combo (Esc cancel / Del clear).");

            // 行3：调音台通道「快捷键与链接」说明
            fLabel (cx, py + 196, noteW, "调音台通道「快捷键与链接」说明",
                    "About channel hotkeys & links (mixer)");
            fNote  (cx, py + 220, noteW, 50,
                    "通道条「链接」列打开「快捷键与链接」弹窗，动作×字母×序号 组成链接码。"
                    "同码（同字母同序号）为同步组：整组一起开/关，快捷键全组共用，任意通道修改即全组同步；"
                    "同字母不同序号为互斥组：任一激活，其余自动关闭。",
                    "Open the popup from a channel's 'Link' column; action × letter × index forms a code. "
                    "Same code = sync group (toggle together, hotkey shared and synced); "
                    "same letter, different index = mutex group (one on, others off).");
            fNote  (cx, py + 276, noteW, 46,
                    "通道快捷键同为系统级全局热键：其它程序前台按下也可触发，建议用 Ctrl/Alt 组合键或 F 键。"
                    "弹窗内换码实时刷新分组与继承键，与其它通道共用快捷键会红字提示冲突。",
                    "Channel hotkeys are system-wide too (they fire while other apps are in front); prefer Ctrl/Alt combos or F-keys. "
                    "Switching codes refreshes groups live; shared keys show a red conflict warning.");
            // 行4：桌面歌词锁定 / 解锁（锁定后鼠标穿透，靠快捷键或托盘切回）
            fLabel (cx, py + 330, noteW, "桌面歌词锁定 / 解锁", "Desktop lyrics lock / unlock");
            fNote  (cx, py + 354, noteW, 46,
                    "该快捷键为切换开关：未锁定时按下即锁定，锁定时按下即解锁（默认 Ctrl+Alt+L）。也可点歌词窗口左上角的锁按钮；"
                    "锁定后鼠标穿透，只能用快捷键或托盘解锁。与隐藏/通道快捷键撞键时不注册，按钮显示未生效。",
                    "This hotkey is a toggle: press to lock when unlocked and to unlock when locked (default Ctrl+Alt+L). "
                    "You can also click the lock at the top-left of the lyrics window; once locked the mouse passes through, "
                    "so only this hotkey or the tray menu reverts it. Clashing keys are not registered and show as inactive.");
            break;
        }
        case 3:
        case 4:                            // 设备设置 / 插件库：内嵌页自带标题，不画分组头
            break;
        case 6:                             // 其他
        {
            const int cw = c.getRight() - 16 - cx;
            const int colW = (cw - 48) / 3;
            const int x2 = cx + colW + 24, x3 = x2 + colW + 24;
            auto fLabel = [&] (int fx, int fy, int fw, const char* zh, const char* en)
            {
                g.setFont (mb::ui::font (9.0f, true));
                g.setColour (mb::ui::subText);
                g.drawText (mb::tr (zh, en), fx, fy, fw, 16, juce::Justification::centredLeft, false);
            };
            auto fNote = [&] (int fx, int fy, int fw, const char* zh, const char* en)
            {
                g.setFont (mb::ui::font (9.0f));
                g.setColour (mb::ui::subText);
                g.drawText (mb::tr (zh, en), fx, fy, fw, 30, juce::Justification::topLeft, true);
            };
            // 三列一行：诊断 | 启动新实例 | 重启应用
            fLabel (cx, py + 24, colW, "诊断", "Diagnostics");
            fLabel (x2, py + 24, colW, "启动新实例", "New instance");
            fLabel (x3, py + 24, colW, "重启应用", "Restart app");
            fNote  (cx, py + 78, colW, "查看设备与录音诊断信息", "Device and recording diagnostics");
            fNote  (x2, py + 78, colW, "以新进程再启动一份应用", "Start another app process");
            fNote  (x3, py + 78, colW, "保存会话后关闭并重新打开", "Save session, quit and relaunch");
            break;
        }
        }

        // 激活 Tab 的强调色侧条（纵向 Tab：条画在按钮左缘）
        const auto tb = tabBtns_[juce::jlimit (0, kTabCount - 1, activeTab_)].getBounds();
        g.setColour (mb::ui::accent[0]);
        g.fillRect (tb.getX(), tb.getY(), 2, tb.getHeight());
    }

    void resized() override
    {
        const auto c = cardBounds().toNearestInt();
        const int x = contentX (c);                       // 内容区起点（Tab 栏右侧）
        const int w = c.getRight() - 16 - x;
        const int py = c.getY() + tabPageY();

        // 纵向 Tab 栏（卡片左侧）
        for (int i = 0; i < kTabCount; ++i)
            tabBtns_[i].setBounds (c.getX() + 12, c.getY() + 92 + i * 30, 84, 24);

        autoStart_.setBounds (x, py + 22, w, 24);
        startTray_.setBounds (x, py + 50, w, 24);
        closeTray_.setBounds (x, py + 78, w, 24);
        minimizeFolds_.setBounds (x, py + 106, w, 24);
        alwaysOnTop_.setBounds (x, py + 134, w, 24);
        desktopLyricsOnTop_.setBounds (x, py + 162, w, 24);

        // 界面 Tab：两列网格（列宽与 paint 的标签列一致）
        const int colW2 = (w - 24) / 2;
        const int gx2 = x + colW2 + 24;
        langBtn_.setBounds    (x,   py + 42,  colW2, 30);
        scaleCombo_.setBounds (gx2, py + 42,  colW2, 30);
        borderNone_.setBounds (x, py + 146, w, 22);
        borderLeft_.setBounds (x, py + 170, w, 22);
        borderFull_.setBounds (x, py + 194, w, 22);
        lightTheme_.setBounds (x,   py + 268, colW2, 30);

        // 内嵌页：设备设置 / 插件库占满 Tab 内容区
        const auto embed = juce::Rectangle<int> (c.getX() + 100, c.getY() + 88,
                                                 c.getRight() - 16 - (c.getX() + 100),
                                                 c.getBottom() - 12 - (c.getY() + 88));
        devicePage_->setBounds (embed);
        pluginPage_->setBounds (embed);

        // 录音 Tab：三列网格（格式 / 位深 / 采样率），第二行模式 + 目录按钮
        const int colW3 = (w - 48) / 3;
        fmtCombo_.setBounds  (x,                   py + 42,  colW3,      30);
        bitsCombo_.setBounds (x + colW3 + 24,      py + 42,  colW3,      30);
        srCombo_.setBounds   (x + 2 * (colW3 + 24),py + 42,  colW3,      30);
        modeCombo_.setBounds (x,                   py + 148, colW3,      30);
        dirBtn_.setBounds    (x + colW3 + 24,      py + 148, colW3 / 2 - 4, 30);
        openDirBtn_.setBounds(x + colW3 + 24 + colW3 / 2 + 4, py + 148, colW3 / 2 - 4, 30);

        // 其他 Tab：诊断 / 启动新实例 / 重启应用 三列一行（列位与 paint 标签一致）
        diagBtn_.setBounds        (x,                   py + 42, colW3, 30);
        newInstanceBtn_.setBounds (x + colW3 + 24,      py + 42, colW3, 30);
        restartBtn_.setBounds     (x + 2 * (colW3 + 24),py + 42, colW3, 30);

        // 快捷键 Tab：左列上下两条录入按钮，右列两个隐藏方式单选
        hotkeyBtn_.setBounds       (x,   py + 44,  colW2, 30);
        unlockHotkeyBtn_.setBounds (x,   py + 110, colW2, 30);
        hideTrayMode_.setBounds (gx2, py + 44,  colW2, 22);
        hideFullMode_.setBounds (gx2, py + 70,  colW2, 22);

        closeBtn_.setBounds (c.getRight() - 110, c.getBottom() - 38, 92, 26);
        applyDeviceBtn_.setBounds (c.getRight() - 110 - 8 - 128, c.getBottom() - 38, 128, 26);
        applyDeviceBtn_.setVisible (activeTab_ == 3);   // 仅「设备设置」Tab 显示页脚应用按钮

        // 只显示当前 Tab 的控件
        const bool t0 = activeTab_ == 0, t1 = activeTab_ == 1,
                   t2 = activeTab_ == 2, t3 = activeTab_ == 3,
                   t4 = activeTab_ == 4, t5 = activeTab_ == 5,
                   t6 = activeTab_ == 6;
        autoStart_.setVisible (t0);
        startTray_.setVisible (t0);
        closeTray_.setVisible (t0);
        minimizeFolds_.setVisible (t0);
        alwaysOnTop_.setVisible (t0);
        desktopLyricsOnTop_.setVisible (t0);
        langBtn_.setVisible (t1);
        scaleCombo_.setVisible (t1);
        borderNone_.setVisible (t1);
        borderLeft_.setVisible (t1);
        borderFull_.setVisible (t1);
        lightTheme_.setVisible (t1);
        fmtCombo_.setVisible (t2);
        bitsCombo_.setVisible (t2);
        srCombo_.setVisible (t2);
        modeCombo_.setVisible (t2);
        dirBtn_.setVisible (t2);
        openDirBtn_.setVisible (t2);
        devicePage_->setVisible (t3);           // 内嵌页随 Tab 显隐
        pluginPage_->setVisible (t4);
        diagBtn_.setVisible (t6);               // 其他：诊断
        newInstanceBtn_.setVisible (t6);
        restartBtn_.setVisible (t6);
        hotkeyBtn_.setVisible (t5);             // 快捷键：两条录入按钮 + 隐藏方式
        unlockHotkeyBtn_.setVisible (t5);
        hideTrayMode_.setVisible (t5);
        hideFullMode_.setVisible (t5);
    }

    void setTab (int i)
    {
        i = juce::jlimit (0, kTabCount - 1, i);
        if (activeTab_ == i) return;
        activeTab_ = i;
        refreshTabStyles();
        resized();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        // 长按触控板被系统翻译成右键时，不应把设置页误关掉
        if (ev.mods.isPopupMenu() || ev.mods.isRightButtonDown()) return;
        if (! cardBounds().contains (ev.position) && onClose) onClose();
    }

    const mb::AppSettingsData& data() const noexcept { return data_; }

    /** MainComponent 回写注册结果：录入按钮上直接显示「已生效 / 未生效」。 */
    void setHotkeyStatus (bool ok)
    {
        hotkeyOk_ = ok;
        refreshHotkeyText();
    }

    /** 锁定/解锁桌面歌词切换热键的注册结果（含撞键被跳过）。 */
    void setUnlockHotkeyStatus (bool ok)
    {
        unlockHotkeyOk_ = ok;
        refreshHotkeyText();
    }

    /** 桌面歌词窗口右上角置顶开关被点：同步设置页复选框与其数据副本。 */
    void syncDesktopLyricsOnTop (bool on)
    {
        data_.desktopLyricsOnTop = on;
        desktopLyricsOnTop_.setToggleState (on, juce::dontSendNotification);
    }

    /** 主题从另一个入口（抽屉按钮）翻转后，同步本地缓存并刷新按钮文字。 */
    void syncLightTheme (bool light)
    {
        data_.lightTheme = light;
        lightTheme_.setButtonText (data_.lightTheme
                                       ? mb::tr ("切换到深色模式", "Switch to dark theme")
                                       : mb::tr ("切换到浅色模式", "Switch to light theme"));
    }

    /** 主题切换后重刷构造期捕获的控件色（MainComponent 在切浅色/深色时调用）。 */
    void refreshThemeColours()
    {
        for (auto* b : { &autoStart_, &startTray_, &closeTray_, &minimizeFolds_,
                          &alwaysOnTop_, &desktopLyricsOnTop_,
                          &borderNone_, &borderLeft_, &borderFull_,
                          &hideTrayMode_, &hideFullMode_ })
            styleToggleColour (*b);
        for (auto* b : { &langBtn_, &diagBtn_, &dirBtn_,
                         &openDirBtn_, &closeBtn_, &applyDeviceBtn_, &newInstanceBtn_, &restartBtn_,
                         &hotkeyBtn_, &unlockHotkeyBtn_, &lightTheme_ })
            styleButtonColour (*b);
        refreshTabStyles();
        for (auto* c : { &fmtCombo_, &bitsCombo_, &srCombo_, &modeCombo_, &scaleCombo_ })
            styleComboColour (*c);
        devicePage_->refreshThemeColours();   // 内嵌设备页按钮/标签/下拉跟随主题
        repaint();
    }

private:
    static constexpr int kTabCount = 7;             // Tab 总数（数组与所有循环共用，避免越界）
    static constexpr int tabPageY() { return 98; }   // Tab 内容统一起始 y（Tab 栏下方）
    static constexpr int kTabW = 84;                 // 纵向 Tab 栏宽

    /** 内容区起点 x：纵向 Tab 栏右侧。 */
    static int contentX (juce::Rectangle<int> card) { return card.getX() + kTabW + 24; }

    // 控件配色（构造与主题刷新共用同一份，避免两处色值漂移）
    void styleToggleColour (juce::ToggleButton& b)
    {
        b.setColour (juce::ToggleButton::textColourId, mb::ui::text);
        b.setColour (juce::ToggleButton::tickColourId, mb::ui::accent[0]);
        b.setColour (juce::ToggleButton::tickDisabledColourId, mb::ui::subText);
    }
    void styleButtonColour (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, mb::ui::card);
        b.setColour (juce::TextButton::textColourOffId, mb::ui::text);
    }
    /** 选中 Tab 高亮：强调色底 + 强调色字；未选中用普通弱化样式。 */
    void refreshTabStyles()
    {
        for (int i = 0; i < kTabCount; ++i)
        {
            auto& b = tabBtns_[i];
            if (i == activeTab_)
            {
                b.setColour (juce::TextButton::buttonColourId, mb::ui::accent[0].withAlpha (0.26f));
                b.setColour (juce::TextButton::textColourOffId, mb::ui::accent[0]);
            }
            else
            {
                b.setColour (juce::TextButton::buttonColourId, mb::ui::sectionBg);
                b.setColour (juce::TextButton::textColourOffId, mb::ui::subText);
            }
        }
    }
    void styleComboColour (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, mb::ui::sectionBg);
        c.setColour (juce::ComboBox::textColourId, mb::ui::text);
        c.setColour (juce::ComboBox::outlineColourId, mb::ui::border);
        c.setColour (juce::ComboBox::arrowColourId, mb::ui::subText);
    }

    juce::Rectangle<float> cardBounds() const
    {
        // 所有 Tab 统一卡片尺寸（保证「设备设置 / 插件库」内嵌页能放下）
        const int cardW = juce::jmin (800, juce::jmax (720, getWidth() - 60));
        const int cardH = juce::jmin (600, juce::jmax (560, getHeight() - 24));
        return { static_cast<float> ((getWidth() - cardW) / 2),
                 static_cast<float> ((getHeight() - cardH) / 2),
                 static_cast<float> (cardW), static_cast<float> (cardH) };
    }

    mb::AppSettingsData data_;
    double deviceSr_ = 48000.0;

    juce::ToggleButton autoStart_, startTray_, closeTray_, minimizeFolds_;
    juce::ToggleButton alwaysOnTop_, desktopLyricsOnTop_;       // 主窗口 / 桌面歌词置顶
    juce::ToggleButton borderNone_, borderLeft_, borderFull_;   // 通道边框三态单选
    juce::TextButton tabBtns_[kTabCount];         // 分类 Tab：常规 / 界面 / 录音 / 设备设置 / 插件库 / 其他 / 快捷键
    int activeTab_ = 0;
    juce::TextButton lightTheme_;
    juce::TextButton langBtn_, diagBtn_, dirBtn_, openDirBtn_, closeBtn_;
    juce::TextButton applyDeviceBtn_;                 // 设备设置 Tab 的页脚「应用」
    juce::TextButton newInstanceBtn_, restartBtn_;   // 「其他」区按钮行
    juce::ComboBox fmtCombo_, bitsCombo_, srCombo_, modeCombo_, scaleCombo_;
    juce::TextButton hotkeyBtn_;                      // 全局隐藏/恢复快捷键录入按钮
    juce::TextButton unlockHotkeyBtn_;                // 锁定/解锁桌面歌词切换快捷键录入按钮
    juce::ToggleButton hideTrayMode_, hideFullMode_;  // 隐藏方式：托盘 / 完全隐藏
    bool capturing_ = false;                          // 正在录入快捷键
    int  captureTarget_ = 0;                         // 1=隐藏/恢复窗口 / 2=解锁桌面歌词
    bool hotkeyOk_ = true, unlockHotkeyOk_ = true;   // 全局钩子是否已生效（MainComponent 回写）

    void startCapture (int target)
    {
        if (capturing_ && captureTarget_ == target) { stopCapture(); return; }
        capturing_ = true;
        captureTarget_ = target;
        if (onHotkeyCapture) onHotkeyCapture (true);   // 录入期间抑制系统级钩子
        refreshHotkeyText();
        grabKeyboardFocus();
    }

    void stopCapture()
    {
        capturing_ = false;
        captureTarget_ = 0;
        if (onHotkeyCapture) onHotkeyCapture (false);
        refreshHotkeyText();
    }

    /** 快捷键录入：仅录入态拦截按键，否则冒泡给全局快捷键钩子。 */
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (! capturing_) return false;

        if (key == juce::KeyPress::escapeKey)         // Esc = 取消录入
        {
            stopCapture();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::deleteKey
            || key.getKeyCode() == juce::KeyPress::backspaceKey)   // Del/退格 = 清除
        {
            if (captureTarget_ == 1)
            {
                data_.hideHotkeyKey = 0;
                data_.hideHotkeyMods = 0;
            }
            else
            {
                data_.unlockLyricsHotkeyKey = 0;
                data_.unlockLyricsHotkeyMods = 0;
            }
            stopCapture();
            if (onSettingsChanged) onSettingsChanged (data_);
            return true;
        }

        // 至少带 Ctrl 或 Alt（或 F 键）：纯字母数字作为全局热键会吞掉正常输入
        auto& btn = captureTarget_ == 1 ? hotkeyBtn_ : unlockHotkeyBtn_;
        const auto mk = key.getModifiers();
        const bool fnKey = key.getKeyCode() >= juce::KeyPress::F1Key
                        && key.getKeyCode() <= juce::KeyPress::F24Key;
        if (! fnKey && ! mk.isCtrlDown() && ! mk.isAltDown())
        {
            btn.setButtonText (mb::tr ("请使用 Ctrl/Alt + 键，或 F 功能键",
                                       "Use Ctrl/Alt + a key, or an F-key"));
            return true;
        }

        if (captureTarget_ == 1)
        {
            data_.hideHotkeyKey  = key.getKeyCode();
            data_.hideHotkeyMods = mk.getRawFlags();
        }
        else
        {
            data_.unlockLyricsHotkeyKey  = key.getKeyCode();
            data_.unlockLyricsHotkeyMods = mk.getRawFlags();
        }
        stopCapture();
        if (onSettingsChanged) onSettingsChanged (data_);   // 保存 + MainComponent 注册热键
        return true;
    }

    /** 两条录入按钮文字：录入态 / 未设置 / 当前组合键描述 + 生效状态。 */
    void refreshHotkeyText()
    {
        auto paintBtn = [this] (juce::TextButton& b, int key, int mods, bool ok, int target)
        {
            const bool cap = capturing_ && captureTarget_ == target;
            b.setColour (juce::TextButton::buttonColourId,
                         cap ? mb::ui::accent[0] : mb::ui::card);
            b.setColour (juce::TextButton::textColourOffId,
                         cap ? juce::Colours::white : mb::ui::text);
            const juce::String status = ok
                ? mb::tr ("  ✓ 已生效", "  ✓ active")
                : mb::tr ("  ⚠ 未生效", "  ⚠ inactive");
            b.setButtonText (cap
                ? mb::tr ("请按下快捷键…（Esc 取消 / Del 清除）",
                          "Press keys… (Esc cancel / Del clear)")
                : (key == 0
                       ? mb::tr ("未设置（点击录入）", "Not set — click to record")
                       : mb::tr ("快捷键  ", "Hotkey:  ")
                             + juce::KeyPress (key, juce::ModifierKeys (mods),
                                               0).getTextDescription() + status));
        };
        paintBtn (hotkeyBtn_, data_.hideHotkeyKey, data_.hideHotkeyMods, hotkeyOk_, 1);
        paintBtn (unlockHotkeyBtn_, data_.unlockLyricsHotkeyKey,
                  data_.unlockLyricsHotkeyMods, unlockHotkeyOk_, 2);
    }

    std::unique_ptr<juce::FileChooser> chooser_;

    std::unique_ptr<DeviceSetupOverlay> devicePage_;   // 「设备设置」Tab 内嵌页
    std::unique_ptr<PluginPanel> pluginPage_;          // 「插件库」Tab 内嵌页

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};
