@echo off
cd /d C:\Dev\FFE\George\voicestick\desktop\windows
echo Starting build > C:\Dev\FFE\George\voicestick\build_output.log
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >> C:\Dev\FFE\George\voicestick\build_output.log 2>&1
echo. >> C:\Dev\FFE\George\voicestick\build_output.log
echo === Running CMake configuration === >> C:\Dev\FFE\George\voicestick\build_output.log
cmake -S . -B build-x64 -G Ninja >> C:\Dev\FFE\George\voicestick\build_output.log 2>&1
if errorlevel 1 (
    echo CMake FAILED >> C:\Dev\FFE\George\voicestick\build_output.log
    exit /b 1
)
echo. >> C:\Dev\FFE\George\voicestick\build_output.log
echo === Running build === >> C:\Dev\FFE\George\voicestick\build_output.log
ninja -C build-x64 >> C:\Dev\FFE\George\voicestick\build_output.log 2>&1
if errorlevel 1 (
    echo Build FAILED >> C:\Dev\FFE\George\voicestick\build_output.log
    exit /b 1
)
echo. >> C:\Dev\FFE\George\voicestick\build_output.log
echo === Build SUCCEEDED === >> C:\Dev\FFE\George\voicestick\build_output.log
exit /b 0
