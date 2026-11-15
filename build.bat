@echo off
rem MicroBridge - manual MSVC + Windows SDK environment (no vcvarsall / reg.exe)
rem Mirrors the working setup used by S-Rack; JUCE is referenced, not copied.
set "VSROOT=D:\Program Files\Visual Studio\Visual Studio1"
set "MSVC=%VSROOT%\VC\Tools\MSVC\14.52.37025"
set "SDK=D:\Windows Kits\10"
set "SDKVER=10.0.26100.0"

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%SDK%\bin\x64;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt;%SDK%\Include\%SDKVER%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "ROOT=D:\Xiangmu\MicroBridge"

"%CMAKE%" -G Ninja -B "%ROOT%\build" -S "%ROOT%" -DCMAKE_BUILD_TYPE=Release %*
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%ROOT%\build" --config Release --target MicroBridgeApp RouteGraphTest %*
