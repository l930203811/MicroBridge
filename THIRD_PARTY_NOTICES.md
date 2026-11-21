# Third-party notices / 第三方声明

This file lists third-party components linked into or required to build
**MicroBridge**. The project's own source code is licensed under the
[GNU AGPLv3](LICENSE); the components below are **not** re-licensed by that grant —
each keeps its own terms.

本文件列出 **MicroBridge** 链接或构建所需的第三方组件。项目自身源码采用
[GNU AGPLv3](LICENSE)；下列组件**不在**该授权范围内，各自保留其原始许可。

---

## 1. JUCE 8.0.15 — AGPLv3 or commercial

- Source: https://github.com/juce-framework/JUCE (release 8.0.15)
- This repository does not vendor JUCE. `CMakeLists.txt` uses an external JUCE
  8 source tree via the `JUCE_DIR` cache variable (by default the local
  `../S-Rack/ThirdParty/juce-8.0.15`).
- Modules used: `juce_audio_utils`, `juce_audio_devices`, `juce_audio_processors`,
  `juce_dsp`, `juce_gui_basics`, plus required core/graphics/events/data
  structures/cryptography modules.
- JUCE is dual-licensed under **AGPLv3** and the commercial JUCE 8 licence.
  This project uses the **AGPLv3** path — see `ThirdParty/juce-8.0.15/LICENSE.md`
  in the JUCE source tree.
- JUCE bundles further sub-components under their own licences (VST3 SDK MIT,
  Ogg Vorbis / FLAC BSD, AudioUnitSDK Apache-2.0, jpeglib IJG, CHOC ISC, etc.).
  The full list is in the JUCE tree's `LICENSE.md`.

## 2. Steinberg VST3 SDK — MIT (VST3 branding/guidelines apply separately)

- Used through `JUCE_PLUGINHOST_VST3=1` for hosting third-party VST3 plugins.
- The VST3 SDK portion bundled in JUCE is MIT-licensed
  (`modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt`).
- The VST3 trademark and logo usage is governed by Steinberg's separate
  guidelines; this project does not claim the VST3 mark.

## 3. Steinberg ASIO SDK — proprietary

- Enabled via `JUCE_ASIO=1`; JUCE's `juce_audio_devices/native/asio/` references
  the Steinberg ASIO SDK.
- The ASIO SDK is **not** redistributable; it is proprietary and requires a
  licence from Steinberg. No ASIO SDK binary is shipped in this repository or
  in the release archives. Obtain it from Steinberg and comply with its licence.

## 4. Windows Media Foundation — Microsoft system component

- Used for MP3/M4A/AAC/WMA/FLAC decoding on Windows (`mfplat`, `mfuuid`,
  `mfreadwrite`). A built-in OS component, not redistributed by this project.

## 5. Third-party plugins hosted by MicroBridge

- MicroBridge can host VST3 plugins supplied by the user. Those plugins are
  independent products with their own licences; MicroBridge does not bundle
  or claim rights over them.

---

**Note / 注意**: this project builds against JUCE under the AGPLv3 path. If you
wish to distribute closed-source binaries, you must instead obtain a commercial
JUCE licence. This is not legal advice.

本项目按 AGPLv3 路径使用 JUCE 构建。若需分发闭源二进制，须另行取得 JUCE 商业许可。
本声明不构成法律意见。
