#include <JuceHeader.h>

#include "AppSettings.h"
#include "MainComponent.h"
#include "ModernMenu.h"

// 全局默认外观：现代弹出菜单 + 可交互控件手型光标。
// 进程级单例定义在 ModernMenu.h（sharedLookAndFeel），主题切换后可刷新缓存色。
static MicroBridgeLookAndFeel& appLookAndFeel() { return mb::sharedLookAndFeel(); }

//==============================================================================
// 原生标题栏改自绘（MainWindow setUsingNativeTitleBar(false)）：
// 本机为 Windows Server 2022 / Win10 内核（<22000），DWM 不支持按窗口设置
// 标题栏颜色（DWMWA_CAPTION_COLOR 为 Win11 22000+ 专属），浅色模式无法跟随
// 主题。自绘标题栏由 MicroBridgeLookAndFeel::drawDocumentWindowTitleBar 接管，
// 颜色直接读全局主题，拖拽/缩放/最小化/最大化/关闭由 JUCE DocumentWindow 处理。
//==============================================================================

//==============================================================================
// 微音桥 MicroBridge —— Windows 轻量音频路由机架
//==============================================================================
class MicroBridgeApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "MicroBridge"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    // 允许多实例并行：JUCE 的单实例检测会在已有实例（哪怕只是崩溃残留的进程壳）
    // 存在时让新进程静默退出、连窗口都不创建，排查成本高且调音台本就有多开需求。
    bool moreThanOneInstanceAllowed() override { return true; }

    /** 「另起微音桥」带 --show-window 启动：忽略「启动后隐藏到托盘」，
        保证新实例立刻可见，不会让用户以为没启动。 */
    static bool& forceShowWindow() { static bool v = false; return v; }

    void initialise (const juce::String& commandLine) override
    {
        // 必须在创建任何组件之前安装：PopupMenu / ComboBox / Button 的光标与
        // 菜单绘制都在构造或首次显示时取默认 LookAndFeel。
        juce::LookAndFeel::setDefaultLookAndFeel (&appLookAndFeel());
        forceShowWindow() = commandLine.contains ("--show-window");

        // 全局界面缩放（设置页可调，持久化）：字体、控件、布局整体放大
        const auto bootSettings = mb::AppSettings::load();
        juce::Desktop::getInstance().setGlobalScaleFactor (bootSettings.uiScale);
        // 主题在任何组件构造/绘制之前定死，浅色模式不在启动时闪深色
        mb::ui::applyTheme (! bootSettings.lightTheme);
        mb::setLanguageMode (bootSettings.languageMode);

        // 「重启微音桥」：新进程先等旧进程释放音频设备（约 1.4 s），
        // 避免多个实例同时抢占 WASAPI 端点。
        if (commandLine.contains ("--restart-wait"))
            juce::Thread::sleep (1400);

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : juce::DocumentWindow (name, mb::ui::topBar,
                                    juce::DocumentWindow::allButtons)
        {
            // 自绘标题栏：本机（Win10 内核）DWM 不支持标题栏着色，
            // 由 MicroBridgeLookAndFeel::drawDocumentWindowTitleBar 画主题色
            setUsingNativeTitleBar (false);
            setTitleBarHeight (34);
            // 标题栏左上角应用图标（自绘标题栏由 LnF 画到文字左侧）
            int iconSize = 0;
            const char* iconData = BinaryData::getNamedResource ("app_icon_png", iconSize);
            if (iconData != nullptr && iconSize > 0)
                setIcon (juce::ImageCache::getFromMemory (iconData, iconSize));
            setContentOwned (new MainComponent(), true);
            if (auto* mc = dynamic_cast<MainComponent*> (getContentComponent()))
            {
                mc->onHideWindow = [this] { setVisible (false); };
                mc->onShowWindow = [this] { setVisible (true); toFront (true); };
                // 原生标题栏即状态栏：品牌名（跟随语言）+ 驱动/设备/延迟/连接数
                mc->onTitleChanged = [this] (const juce::String& t) { setName (t); };
                // 窗口卷起（折叠）/ 展开：高度收缩到标题栏 + 通道底部控制带
                mc->onFoldChanged = [this] (bool f) { applyFold (f); };
                mc->refreshWindowTitle();       // 启动即同步一次，避免先闪出 "MicroBridge"
            }
            // 窗口尺寸跟随内容（preferredWidth = 抽屉 + 可见通道条），不再写死 1280
            // 导致右侧一大片空白；centreWithSize 仅负责居中。
            if (auto* mc2 = dynamic_cast<MainComponent*> (getContentComponent()))
                centreWithSize (mc2->getWidth(), mc2->getHeight());
            else
                centreWithSize (1280, 720);
            // 上限/下限与 Theme.h 保持一致：下限仅 520×480，确保删除通道后窗口能缩到
            // 紧贴调音台，不再被旧值（1000×600）顶住留一大片空白。原生标题栏已接管拖拽缩放。
            setResizeLimits (mb::ui::kMinWindowWidth, mb::ui::kMinWindowHeight,
                             mb::ui::kMaxWindowWidth, mb::ui::kMaxWindowHeight);
            setResizable (true, false);
            setVisible (true);
            // 启动即恢复「窗口始终置顶」（peer 此时已建立）
            if (auto* mc3 = dynamic_cast<MainComponent*> (getContentComponent()))
                mc3->applyAlwaysOnTop();
            if (mb::AppSettings::load().startMinimizedToTray
                && ! MicroBridgeApplication::forceShowWindow())
                setVisible (false);
        }

        void closeButtonPressed() override
        {
            if (auto* mc = dynamic_cast<MainComponent*> (getContentComponent()))
            {
                mc->handleCloseRequest();       // 内部负责：保存提示 / 隐藏托盘 / 退出
                return;
            }
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        /** 最小化按钮（标题栏）：
            - 已折叠 → 变为向下箭头，点击展开窗口；
            - 未折叠且勾选「最小化时折叠窗口」→ 卷起折叠（只留通道底部控制带）；
            - 否则走 JUCE 默认最小化到任务栏。 */
        void minimiseButtonPressed() override
        {
            auto* mc = dynamic_cast<MainComponent*> (getContentComponent());
            if (folded_)                            // 箭头按钮：展开
            {
                if (mc != nullptr) mc->setFolded (false);
                return;
            }
            if (mb::AppSettings::load().minimizeFolds)   // 最小化即卷起折叠
            {
                if (mc != nullptr) mc->setFolded (true);
                return;
            }
            juce::DocumentWindow::minimiseButtonPressed();
        }

        void lookAndFeelChanged() override
        {
            juce::DocumentWindow::lookAndFeelChanged();
            // 主题切换可能重建标题栏按钮，折叠态的展开箭头需要补回
            if (folded_)
                if (auto* b = dynamic_cast<NativeWindowButton*> (getMinimiseButton()))
                    b->setExpandArrow (true);
        }

    private:
        /** 应用卷起折叠状态：折叠时收紧最小高度限制并把窗口缩到
            标题栏 + 通道底部控制带；展开时还原尺寸与限制。
            最小化按钮同步切换为向下箭头（再点展开）。 */
        void applyFold (bool f)
        {
            folded_ = f;
            auto* mc = dynamic_cast<MainComponent*> (getContentComponent());
            const int tbH = getTitleBarHeight();
            if (f)
            {
                restoreBounds_ = getBounds();
                const int h = tbH + (mc != nullptr ? mc->foldedContentHeight() : 78);
                // 最小宽度同时放宽到 260：通道少时窗口可收窄贴合，避免右侧留白
                setResizeLimits (260, h,
                                 mb::ui::kMaxWindowWidth, mb::ui::kMaxWindowHeight);
                setBounds (getX(), getY(), getWidth(), h);   // 顶边固定，向上收
            }
            else
            {
                setResizeLimits (mb::ui::kMinWindowWidth, mb::ui::kMinWindowHeight,
                                 mb::ui::kMaxWindowWidth, mb::ui::kMaxWindowHeight);
                const auto b = restoreBounds_.isEmpty()
                                 ? juce::Rectangle<int> (getX(), getY(), getWidth(),
                                                         mb::ui::kDefaultWindowHeight)
                                 : restoreBounds_;
                setBounds (b);
            }
            if (auto* b = dynamic_cast<NativeWindowButton*> (getMinimiseButton()))
                b->setExpandArrow (f);
        }

        bool folded_ = false;                    // 当前是否处于卷起折叠态
        juce::Rectangle<int> restoreBounds_;     // 折叠前的窗口位置与尺寸

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (MicroBridgeApplication)
