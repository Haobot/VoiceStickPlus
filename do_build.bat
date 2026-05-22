@echo off
setlocal enabledelayedexpansion

set VS_DIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set CMAKE_DIR=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
set NINJA_DIR=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja
set PATH=%CMAKE_DIR%;%NINJA_DIR%;%PATH%

cd /d C:\Dev\FFE\George\voicestick\desktop\windows

echo Starting build at %date% %time% > build_output.log
echo. >> build_output.log

echo === Setting up VS build environment === >> build_output.log
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >> build_output.log 2>&1
if errorlevel 1 (
    echo vcvars64.bat FAILED >> build_output.log
    exit /b 1
)
echo VS environment setup done >> build_output.log
echo. >> build_output.log

echo === Running CMake configuration === >> build_output.log
cmake -S . -B build-x64 -G Ninja >> build_output.log 2>&1
if errorlevel 1 (
    echo CMake configuration FAILED >> build_output.log
    exit /b 1
)
echo CMake configuration succeeded >> build_output.log
echo. >> build_output.log

echo === Running build === >> build_output.log
ninja -C build-x64 >> build_output.log 2>&1
if errorlevel 1 (
    echo Build FAILED >> build_output.log
    exit /b 1
)
echo Build SUCCEEDED >> build_output.log
echo. >> build_output.log
echo Build completed at %date% %time% >> build_output.log
exit /b 0
