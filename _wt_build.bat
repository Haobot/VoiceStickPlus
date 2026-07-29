@echo off
REM worktree 本地增量构建脚本（不提交）：vcvars + 按需 configure + build + ctest
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
if not exist desktop\windows\build-x64 (
  cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
  if errorlevel 1 exit /b 1
)
cmake --build desktop\windows\build-x64
if errorlevel 1 exit /b 1
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
