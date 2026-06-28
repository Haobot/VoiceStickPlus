@echo off
cd /d "C:\Dev\FFE\George\voicestick\desktop\windows"
set "VOICESTICK_WINSPARKLE_URL=C:/Dev/FFE/George/voicestick/desktop/windows/winsparkle-local/WinSparkle-0.9.2.zip"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > NUL 2>&1
if %ERRORLEVEL% neq 0 (echo vcvars failed & exit /b 1)
cmake -S . -B build-x64 -G Ninja
if %ERRORLEVEL% neq 0 (echo CMAKE CONFIGURE FAILED & exit /b 1)
cmake --build build-x64 --target VoiceStick.exe VoiceStickCtl.exe
if %ERRORLEVEL% neq 0 (echo BUILD FAILED & exit /b 1)
echo BUILD SUCCESS
