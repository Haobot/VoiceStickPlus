@echo off
setlocal

echo === Setting up VS build environment ===
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo vcvars64.bat FAILED
    exit /b 1
)

echo === Switching to project directory ===
cd /d C:\Dev\FFE\George\voicestick\desktop\windows

echo === Cleaning old build ===
if exist build-x64 rmdir /s /q build-x64

echo === Running CMake configuration ===
cmake -S . -B build-x64 -G Ninja
if errorlevel 1 (
    echo CMake configuration FAILED
    exit /b 1
)

echo === Running build ===
cmake --build build-x64
if errorlevel 1 (
    echo Build FAILED
    exit /b 1
)

echo === Running tests ===
ctest --test-dir build-x64 --output-on-failure
if errorlevel 1 (
    echo Tests FAILED
    exit /b 1
)

echo === Build and tests SUCCEEDED ===
endlocal
exit /b 0
