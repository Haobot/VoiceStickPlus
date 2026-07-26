@echo off
setlocal
cd /d "%~dp0"
set "SOURCE_DIR=%cd%\desktop\windows"
set "BUILD_DIR=%SOURCE_DIR%\build-x64"
set "VS_INSTALL_PATH=D:\Microsoft Visual Studio\18\Community"
set "WINSPARKLE_DIR=%SOURCE_DIR%\third_party\winsparkle\WinSparkle-0.9.2"

echo === Setup ===
call "%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (echo VCVARS FAILED & exit /b 1)

echo === Configure ===
cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja -DFETCHCONTENT_SOURCE_DIR_WINSPARKLE="%WINSPARKLE_DIR%"
if errorlevel 1 (echo CONFIGURE FAILED & exit /b 1)

echo === Build ===
cmake --build "%BUILD_DIR%"
if errorlevel 1 (echo BUILD FAILED & exit /b 1)

echo BUILD SUCCEEDED
endlocal
exit /b 0
