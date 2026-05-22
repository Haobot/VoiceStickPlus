@echo off
setlocal enabledelayedexpansion

cd /d %~dp0
set ROOT_DIR=%cd%
set BUILD_DIR=%ROOT_DIR%\desktop\windows\build-x64
set LOG_FILE=%ROOT_DIR%\build_output.log

echo Starting build at %date% %time% > "%LOG_FILE%"

:: ==========================================
:: 步骤1：终止残留进程
:: ==========================================
echo.
echo [1/4] Cleaning up leftover processes...
echo === Killing leftover processes === >> "%LOG_FILE%"

:: 杀掉正在运行的 VoiceStick 程序
taskkill /f /im VoiceStick.exe /t >> "%LOG_FILE%" 2>&1
if not errorlevel 1 (
    echo   - Terminated running VoiceStick.exe
)

:: 杀掉残留的 ninja / cmake / cl.exe 进程
taskkill /f /im ninja.exe /t >> "%LOG_FILE%" 2>&1
taskkill /f /im cmake.exe /t >> "%LOG_FILE%" 2>&1
taskkill /f /im cl.exe /t >> "%LOG_FILE%" 2>&1
taskkill /f /im link.exe /t >> "%LOG_FILE%" 2>&1

:: 等待一下让进程完全退出
timeout /t 1 /nobreak > nul 2>&1

:: ==========================================
:: 步骤2：清理旧的构建产物
:: ==========================================
echo.
echo [2/4] Cleaning previous build output...
echo === Cleaning build directory === >> "%LOG_FILE%"

if exist "%BUILD_DIR%" (
    :: 先尝试解锁文件
    if exist "%BUILD_DIR%\VoiceStick.exe" (
        del /f /q "%BUILD_DIR%\VoiceStick.exe" >> "%LOG_FILE%" 2>&1
    )
    :: 全量清理构建目录，避免增量构建缓存问题
    rd /s /q "%BUILD_DIR%" >> "%LOG_FILE%" 2>&1
    if errorlevel 1 (
        echo   ! Warning: failed to fully clean build directory
        echo   ! Some files may still be locked, trying partial build
    ) else (
        echo   - Build directory cleaned
    )
)

:: 清理旧的日志锁文件
del /f /q "%ROOT_DIR%\build_output.log.lock" > nul 2>&1

:: ==========================================
:: 步骤3：配置 MSVC 编译环境
:: ==========================================
echo.
echo [3/4] Setting up MSVC build environment...
echo === Setting up MSVC environment === >> "%LOG_FILE%"

:: 自动检测 VS 2022 安装路径（支持 Community / BuildTools / Professional / Enterprise）
set VS_INSTALL_PATH=
for %%v in (Community, BuildTools, Professional, Enterprise) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%v\VC\Auxiliary\Build\vcvars64.bat" (
        set VS_INSTALL_PATH=C:\Program Files\Microsoft Visual Studio\2022\%%v
    )
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\%%v\VC\Auxiliary\Build\vcvars64.bat" (
        set VS_INSTALL_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\%%v
    )
)

if "%VS_INSTALL_PATH%"=="" (
    echo ERROR: Visual Studio 2022 not found
    echo ERROR: Visual Studio 2022 not found >> "%LOG_FILE%"
    exit /b 1
)

echo   - Using Visual Studio 2022 from: %VS_INSTALL_PATH%
call "%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvars64.bat" >> "%LOG_FILE%" 2>&1
if errorlevel 1 (
    echo ERROR: Failed to setup MSVC environment
    exit /b 1
)

:: ==========================================
:: 步骤4：执行 CMake 配置 + 构建
:: ==========================================
echo.
echo [4/4] Running CMake build...

echo. >> "%LOG_FILE%"
echo === Running CMake configuration === >> "%LOG_FILE%"
cmake -S "%ROOT_DIR%\desktop\windows" -B "%BUILD_DIR%" -G Ninja >> "%LOG_FILE%" 2>&1
if errorlevel 1 (
    echo ERROR: CMake configuration failed
    echo CMake FAILED >> "%LOG_FILE%"
    echo.
    echo See build_output.log for details
    exit /b 1
)

echo. >> "%LOG_FILE%"
echo === Running build === >> "%LOG_FILE%"
ninja -C "%BUILD_DIR%" >> "%LOG_FILE%" 2>&1
if errorlevel 1 (
    echo ERROR: Build failed
    echo Build FAILED >> "%LOG_FILE%"
    echo.
    echo See build_output.log for details
    exit /b 1
)

:: ==========================================
:: 完成
:: ==========================================
echo.
echo Build SUCCEEDED! Output: %BUILD_DIR%\VoiceStick.exe
echo. >> "%LOG_FILE%"
echo === Build SUCCEEDED at %date% %time% === >> "%LOG_FILE%"

endlocal
exit /b 0
