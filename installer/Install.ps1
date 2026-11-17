<#
.SYNOPSIS
    MicroBridge 微音桥 - 免依赖 Windows 安装脚本（用户目录模式）

.DESCRIPTION
    与 MicroBridge.iss 等效，但不依赖 Inno Setup。安装到
    %LOCALAPPDATA%\Programs\MicroBridge（普通权限可写，符合便携化数据设计），
    创建开始菜单 + 桌面快捷方式。卸载只删程序本体，保留 MicroBridgeData 用户数据。

.PARAMETER Uninstall
    卸载模式：移除快捷方式与程序文件，保留数据目录。

.PARAMETER InstallDir
    自定义安装目录（默认 %LOCALAPPDATA%\Programs\MicroBridge）。

.PARAMETER SourceExe
    源 MicroBridge.exe（默认 ../build/MicroBridgeApp_artefacts/Release/MicroBridge.exe）。

.PARAMETER NoShortcut
    不创建桌面/开始菜单快捷方式（便于自动化验证）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File installer\Install.ps1
    powershell -ExecutionPolicy Bypass -File installer\Install.ps1 -Uninstall
#>
param(
    [switch] $Uninstall,
    [string] $InstallDir = "",
    [string] $SourceExe = "",
    [switch] $NoShortcut
)

$ErrorActionPreference = "Stop"
$AppName = "MicroBridge 微音桥"

if ([string]::IsNullOrEmpty($InstallDir)) {
    $InstallDir = Join-Path $env:LOCALAPPDATA "Programs\MicroBridge"
}
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)

$exeName = "MicroBridge.exe"
$dataDir = Join-Path $InstallDir "MicroBridgeData"

# 快捷方式路径（开始菜单 / 桌面）
$startMenuDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$startLink = Join-Path $startMenuDir "$AppName.lnk"
$desktopLink = Join-Path ([Environment]::GetFolderPath("Desktop")) "$AppName.lnk"

function New-Shortcut {
    param([string] $LinkPath, [string] $Target, [string] $WorkDir, [string] $Icon)
    $linkDir = Split-Path -Parent $LinkPath
    if (-not (Test-Path $linkDir)) { New-Item -ItemType Directory -Path $linkDir -Force | Out-Null }
    $shell = New-Object -ComObject WScript.Shell
    $sc = $shell.CreateShortcut($LinkPath)
    $sc.TargetPath = $Target
    $sc.WorkingDirectory = $WorkDir
    $sc.IconLocation = $Icon
    $sc.Description = "微音桥 - 软件配线架"
    $sc.Save()
}

if ($Uninstall) {
    Write-Host "卸载 $AppName ..."
    # 关闭正在运行的实例（按安装路径匹配，不误杀其它位置）
    $procs = Get-Process -Name "MicroBridge" -ErrorAction SilentlyContinue |
             Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) }
    foreach ($p in $procs) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }

    foreach ($lnk in @($startLink, $desktopLink)) {
        if (Test-Path $lnk) { Remove-Item $lnk -Force; Write-Host "  已删除快捷方式: $lnk" }
    }
    $exePath = Join-Path $InstallDir $exeName
    if (Test-Path $exePath) { Remove-Item $exePath -Force; Write-Host "  已删除: $exePath" }

    if (Test-Path $dataDir) {
        Write-Host "  已保留用户数据: $dataDir"
        Write-Host "  （如需彻底清除，手动删除该目录）"
    }
    Write-Host "卸载完成。"
    exit 0
}

# ---- 安装 ----
if ([string]::IsNullOrEmpty($SourceExe)) {
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    $SourceExe = Join-Path (Split-Path -Parent $here) "build\MicroBridgeApp_artefacts\Release\$exeName"
}
$SourceExe = [System.IO.Path]::GetFullPath($SourceExe)
if (-not (Test-Path $SourceExe)) { throw "找不到源文件: $SourceExe （请先构建 Release）" }

Write-Host "安装 $AppName 到: $InstallDir"
if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null }

$targetExe = Join-Path $InstallDir $exeName
Copy-Item $SourceExe $targetExe -Force
Write-Host "  已复制: $targetExe"

if (-not $NoShortcut) {
    New-Shortcut -LinkPath $startLink  -Target $targetExe -WorkDir $InstallDir -Icon "$targetExe,0"
    New-Shortcut -LinkPath $desktopLink -Target $targetExe -WorkDir $InstallDir -Icon "$targetExe,0"
    Write-Host "  已创建开始菜单 + 桌面快捷方式"
} else {
    Write-Host "  已跳过快捷方式（NoShortcut）"
}

Write-Host "安装完成。数据目录将由程序首次运行时创建: $dataDir"
