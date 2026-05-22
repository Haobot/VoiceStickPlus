@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d C:\Dev\FFE\George\voicestick\desktop\windows
cmake --build build-x64 2>&1
