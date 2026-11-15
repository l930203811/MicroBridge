@echo off
rem MicroBridge - ninja-only incremental build (skips cmake configure).
rem Same MSVC/SDK env as build.bat; build.ninja must already exist.
set "VSROOT=D:\Program Files\Visual Studio\Visual Studio1"
set "MSVC=%VSROOT%\VC\Tools\MSVC\14.52.37025"
set "SDK=D:\Windows Kits\10"
set "SDKVER=10.0.26100.0"

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%SDK%\bin\x64;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt;%SDK%\Include\%SDKVER%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "ROOT=D:\Xiangmu\MicroBridge"

"%NINJA%" -C "%ROOT%\build" MicroBridgeApp RouteGraphTest
