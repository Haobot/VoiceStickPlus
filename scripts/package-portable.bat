@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0.."

for /f "usebackq tokens=*" %%v in (VERSION) do set VERSION=%%v
echo === VoiceStick 绿色便携版打包 v%VERSION% ===

echo [1/4] 构建 Release...
call build_win.bat
if %ERRORLEVEL% neq 0 (
    echo 构建失败！
    exit /b 1
)

set OUT_DIR=dist\VoiceStick_Portable_v%VERSION%
if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%OUT_DIR%"

echo [2/4] 收集产物...
copy /y "desktop\windows\build-x64\VoiceStick.exe" "%OUT_DIR%\" >nul
copy /y "desktop\windows\build-x64\VoiceStickCtl.exe" "%OUT_DIR%\" >nul
copy /y "desktop\windows\build-x64\WinSparkle.dll" "%OUT_DIR%\" >nul

echo [3/4] 生成便携配置...
(
echo # VoiceStick 绿色便携版配置
echo # 本文件存在即激活便携模式：所有数据（日志/调试音频）存储在程序目录。
echo # 删除本文件则恢复标准安装版行为（数据写入 %%APPDATA%%\VoiceStick）。
echo.
echo asr_provider = "volcengine"
echo voicestick_api_key = ""
echo voicestick_cloud_url = "wss://api.xiaozhi.me/voicestick/asr/"
echo volcengine_api_key = "your_volcengine_asr_api_key"
echo llm_base_url = "https://api.openai.com/v1"
echo llm_api_key = "your_openai_compatible_llm_api_key"
echo llm_model = "gpt-5.5"
echo refine_enabled = true
echo interaction_mode = "hold_to_talk"
echo resource_id = "volc.seedasr.sauc.duration"
echo asr_hotwords = ""
echo paired_device_ids = ""
echo auto_enter = true
echo debug_audio_cache = false
echo.
echo [output]
echo target = "focused_app"
echo transform = "original"
echo translation_target = "en"
) > "%OUT_DIR%\config.toml"

echo [4/4] 生成说明文件...
(
echo VoiceStick 绿色便携版 v%VERSION%
echo ================================
echo.
echo 使用方法：
echo 1. 编辑 config.toml，填入 API Key 等配置（至少需要 ASR 的 API Key）
echo 2. 双击 VoiceStick.exe 启动
echo 3. 在系统托盘中右键图标进行配对等操作
echo.
echo 便携模式说明：
echo - 本目录存在 config.toml 时自动激活便携模式
echo - 所有数据（配置/日志/调试音频）存储在本目录，不会写入系统 AppData
echo - 便携模式下不支持开机自启和自动更新
echo - 删除 config.toml 后程序会恢复标准安装版行为
echo.
echo 命令行工具：
echo - VoiceStickCtl.exe ota-pull --device XXXX --url http://... --sha256 ... --wait healthy
echo   用于通过 BLE 触发设备 Wi-Fi OTA 升级
echo.
echo 系统要求：Windows 10 1903+ / Windows 11 x64
) > "%OUT_DIR%\README.txt"

set ZIP_FILE=dist\VoiceStick_Portable_v%VERSION%.zip
if exist "%ZIP_FILE%" del "%ZIP_FILE%"
powershell -Command "Compress-Archive -Path '%OUT_DIR%' -DestinationPath '%ZIP_FILE%'"

echo.
echo === 打包完成 ===
echo ZIP: %ZIP_FILE%
echo 目录: %OUT_DIR%
