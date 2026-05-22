@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
echo === Configuring CMake ===
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S C:\Dev\FFE\George\voicestick\desktop\windows -B C:\Dev\FFE\George\voicestick\desktop\windows\build-x64 -G Ninja -DCMAKE_MAKE_PROGRAM="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if %errorlevel% neq 0 (
    echo CMake configuration failed!
    exit /b %errorlevel%
)
echo === Building ===
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C C:\Dev\FFE\George\voicestick\desktop\windows\build-x64
if %errorlevel% neq 0 (
    echo Build failed!
    exit /b %errorlevel%
)
echo === Build succeeded ===
