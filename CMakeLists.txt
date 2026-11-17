cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(MicroBridge VERSION 0.1.0 LANGUAGES C CXX)

# 轻量化约束：不复制 JUCE 源码树，直接复用本机已有的 JUCE 8.0.15。
# 换机器时用 -DJUCE_DIR=<juce-8.x 根目录> 覆盖。
set(JUCE_DIR "D:/Xiangmu/S-Rack/ThirdParty/juce-8.0.15" CACHE PATH "Path to an existing JUCE 8 source tree")
if (NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at JUCE_DIR=${JUCE_DIR}; pass -DJUCE_DIR=<path to juce-8.x>")
endif()
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/juce")

# CI / 纯测试构建：关掉 GUI app 可避开 ASIO SDK 与 Media Foundation 等平台依赖，
# 只编译路由内核与录音回读测试（本地开发保持 ON）。
option(MICROBRIDGE_BUILD_APP "Build the MicroBridge GUI application" ON)

# 不引入：ONNX Runtime / AI 模型 / VST3 宿主 SDK / Media Foundation / 虚拟声卡驱动。
# 只使用 JUCE 的设备 I/O、GUI 与基础 DSP 模块。
# JUCE_ASIO 默认关闭，这里显式打开以支持启动时选择 ASIO 驱动；
# 使用 JUCE 随附的 Steinberg ASIO SDK 头文件（见 README 的许可说明）。

# ---- 纯 C++ 路由内核（无 JUCE 依赖，可独立编译测试）----
add_library(RouteGraph INTERFACE)
target_include_directories(RouteGraph INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_compile_features(RouteGraph INTERFACE cxx_std_20)

enable_testing()
add_executable(RouteGraphTest Tests/RouteGraphTest.cpp)
target_link_libraries(RouteGraphTest PRIVATE RouteGraph)
target_compile_options(RouteGraphTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME RouteGraphTest COMMAND RouteGraphTest)

# ---- 录音链路：真实写盘 + reader 回读，覆盖 Sum/MultiTrack 与格式矩阵 ----
juce_add_console_app(ChannelRecorderTest PRODUCT_NAME "ChannelRecorderTest")
juce_generate_juce_header(ChannelRecorderTest)
target_sources(ChannelRecorderTest PRIVATE
    Tests/ChannelRecorderTest.cpp
    Source/ChannelRecorder.h
    Source/I18n.h
    Source/RouteGraph.h)
target_include_directories(ChannelRecorderTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_compile_options(ChannelRecorderTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(ChannelRecorderTest PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_USE_MP3AUDIOFORMAT=0)
target_link_libraries(ChannelRecorderTest PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)
add_test(NAME ChannelRecorderTest COMMAND ChannelRecorderTest)

# ---- GUI 应用 ----
if (MICROBRIDGE_BUILD_APP)
juce_add_gui_app(MicroBridgeApp
    PRODUCT_NAME "MicroBridge"
    COMPANY_NAME "SKCore"
    BUNDLE_ID "com.skcore.microbridge"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Icons/app_icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Icons/app_icon.png")

juce_generate_juce_header(MicroBridgeApp)

# 图标资源嵌入：界面顶栏 / 抽屉使用（BinaryData::app_icon_png）
juce_add_binary_data(MicroBridgeAssets SOURCES Assets/Icons/app_icon.png)

target_sources(MicroBridgeApp PRIVATE
    Source/Main.cpp
    Source/MainComponent.h
    Source/ChannelStrip.h
    Source/ChannelLinks.h
    Source/ChannelLinkOverlay.h
    Source/DeviceSetup.h
    Source/AudioEngine.h
    Source/AppSettings.h
    Source/SettingsPanel.h
    Source/ColourPicker.h
    Source/DeviceCatalog.h
    Source/ChannelFx.h
    Source/ChannelRecorder.h
    Source/SessionStore.h
    Source/FxPanel.h
    Source/PluginPanel.h
    Source/PluginRack.h
    Source/PluginWindow.h
    Source/DiagnosticsPanel.h
    Source/I18n.h
    Source/SourcePicker.h
    Source/PresetSwitcherOverlay.h
    Source/BuiltinDefaultPreset.h
    Source/Theme.h
    Source/RouteGraph.h
    Source/MFAudioFormat.h
    Source/Lyrics.h
    Source/MediaPlayer.h
    Source/MediaPlayerPanel.h
    Source/PanelChrome.h)

# 源码含中文界面文本，强制 MSVC 按 UTF-8 解析
target_compile_options(MicroBridgeApp PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_compile_definitions(MicroBridgeApp PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_ASIO=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0)

target_link_libraries(MicroBridgeApp PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_gui_basics
    MicroBridgeAssets
    PUBLIC
    juce::juce_recommended_config_flags)

# 「音频播放」自带插件：Media Foundation 解码（MP3/M4A/AAC/WMA/FLAC 等）
if (WIN32)
    target_link_libraries(MicroBridgeApp PRIVATE mfplat mfuuid mfreadwrite)
endif()
endif()  # MICROBRIDGE_BUILD_APP
