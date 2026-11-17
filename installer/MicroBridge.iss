; MicroBridge 微音桥 — Windows 安装包脚本 (Inno Setup 6)
;
; 设计要点：
; · 便携化应用：数据写在 exe 同级 MicroBridgeData，故安装到用户目录
;   %LOCALAPPDATA%\Programs\MicroBridge（永远可写，无需管理员权限）。
; · 只安装 exe，不打包 MicroBridgeData —— 那里面是用户本机状态
;   （设备选择 / 会话 / 设置），首次运行由程序自行创建默认文件。
; · 卸载保留数据目录（用户配置不丢），可选彻底删除。
; · 主程序已内嵌应用图标，快捷方式直接用 exe 图标；另附 .ico 供安装向导使用。
;
; 编译：用 Inno Setup 6 打开本脚本 → Compile；或命令行：
;   ISCC.exe installer\MicroBridge.iss
; 输出：installer\Output\微音桥·MicroBridge v{#MyAppVersion}.exe（发布时压缩为同名 .zip）

#define MyAppName      "微音桥·MicroBridge"
#define MyAppVersion   "0.1.0"
#define MyAppPublisher "MicroBridge"
#define MyAppExeName   "MicroBridge.exe"
#define MyAppURL       "https://github.com/l930203811/MicroBridge"

[Setup]
AppId={{F7A812C1-98DD-4049-8159-CB1E6F6798AD}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
; 用户目录，普通权限即可写（便携化数据目录需跟随 exe 可写）
DefaultDirName={localappdata}\Programs\MicroBridge
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; 安装前若旧版本在运行，提示关闭（避免 EXE 被占用导致安装失败）
CloseApplications=yes
RestartApplications=no
; 输出
OutputDir=Output
OutputBaseFilename=微音桥·MicroBridge v{#MyAppVersion}
SetupIconFile=..\Assets\Icons\MicroBridge.ico
; 卸载列表图标直接指向 ico 文件（指 exe 时 Windows 常缓存成通用图标）
UninstallDisplayIcon={app}\MicroBridge.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64os

[Languages]
; 官方版 Inno Setup：Default.isl = 英文，简中在 Languages\ChineseSimplified.isl。
Name: "english";            MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
; 主程序 + 图标文件（图标供卸载列表 DisplayIcon 用）；
; MicroBridgeData 由程序首次运行自行创建，绝不覆盖用户配置
Source: "..\build\MicroBridgeApp_artefacts\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\Assets\Icons\MicroBridge.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\MicroBridge.ico"; Comment: "微音桥 - 软件配线架"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\MicroBridge.ico"; Comment: "微音桥 - 软件配线架"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 MicroBridge / Launch MicroBridge"; Flags: nowait postinstall skipifsilent

; 说明：Inno Setup 卸载只删除 [Files] 安装的文件（MicroBridge.exe），
; 程序运行时生成的 MicroBridgeData 目录会原样保留 —— 用户配置不丢。
; 需要彻底清除时手动删除 %LOCALAPPDATA%\Programs\MicroBridge\MicroBridgeData。
