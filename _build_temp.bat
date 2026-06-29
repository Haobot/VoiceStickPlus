@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d "C:\Dev\FFE\George\voicestick\desktop\windows"
cmake -S . -B build-x64 -G Ninja
if errorlevel 1 exit /b 1
cmake --build build-x64
if errorlevel 1 exit /b 1
echo BUILD SUCCESS
