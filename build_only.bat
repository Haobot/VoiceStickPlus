@echo off
setlocal
cd /d "%~dp0"
set "BUILD_DIR=%cd%\desktop\windows\build-x64"
set "VS_INSTALL_PATH=D:\Microsoft Visual Studio\18\Community"

call "%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo VCVARS FAILED & exit /b 1)

echo Building...
cmake --build "%BUILD_DIR%"
set EXITCODE=%ERRORLEVEL%
echo Exit code: %EXITCODE%
exit /b %EXITCODE%
