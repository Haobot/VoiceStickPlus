# VoiceStick 绿色便携版打包脚本（PowerShell 版）
# 替代 package-portable.bat：用 .NET 写 UTF-8 文件，规避 cmd 中文 echo 在 GBK 代码页下的解析错位。
# 用法：在仓库根目录执行  powershell -ExecutionPolicy Bypass -File scripts\package-portable.ps1
# 默认只做收集+配置+打包；如需重新构建，传 -Build 开关。
# 自 v2.3.6 起，便携版同时包含固件烧录工具 VoiceStickFlash.exe + FlashTool\（自包含
# python-embed + esptool 运行时，由 scripts\prepare_flash_payload.ps1 生成），与 MSI 布局一致，
# 使便携版同样具备 COM 口固件烧录能力（救砖 / 分区表变更 / bootloader 更新）。

[CmdletBinding()]
param(
  [switch]$Build
)

$ErrorActionPreference = 'Stop'
$Root = (Get-Location).Path
$Version = (Get-Content (Join-Path $Root 'VERSION') -Raw).Trim()
Write-Host "=== VoiceStick 绿色便携版打包 v$Version ===" -ForegroundColor Cyan

# 可选：先调 build_win.bat 构建 Release
if ($Build) {
  Write-Host "[0/4] 构建 Release..." -ForegroundColor Yellow
  & cmd /c (Join-Path $Root 'build_win.bat')
  if ($LASTEXITCODE -ne 0) { Write-Host "构建失败！" -ForegroundColor Red; exit 1 }
}

$ExePath = Join-Path $Root 'desktop\windows\build-x64\VoiceStick.exe'
if (-not (Test-Path $ExePath)) {
  Write-Host "未找到构建产物 VoiceStick.exe，请先构建。" -ForegroundColor Red
  exit 1
}

# 核对产物时间戳，提示是否过期
$exeItem = Get-Item $ExePath
Write-Host ("VoiceStick.exe: {0} bytes, {1}" -f $exeItem.Length, $exeItem.LastWriteTime)

$OutDir = Join-Path $Root "dist\VoiceStick_Portable_v$Version"
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

Write-Host "[1/4] 收集产物..." -ForegroundColor Yellow
Copy-Item (Join-Path $Root 'desktop\windows\build-x64\VoiceStick.exe') $OutDir -Force
Copy-Item (Join-Path $Root 'desktop\windows\build-x64\WinSparkle.dll') $OutDir -Force
Copy-Item (Join-Path $Root 'desktop\windows\build-x64\VoiceStickFlash.exe') $OutDir -Force

# 烧录工具运行时：FlashTool\python\python.exe + site-packages（esptool）。
# 优先复用 MSI 打包已生成的 build-msi-x64\flash_payload（幂等，避免重复下载），
# 不存在时再现场调用 prepare_flash_payload.ps1 生成到便携版目录。
# 布局与 LocatePythonExe()（flash_tool_dialog.cc）候选 1 一致：<exe_dir>\FlashTool\python\python.exe。
$FlashPayloadDir = Join-Path $Root 'desktop\windows\build-msi-x64\flash_payload'
if (Test-Path (Join-Path $FlashPayloadDir 'python\python.exe')) {
  Write-Host "[1/4] 复用 MSI flash_payload: $FlashPayloadDir" -ForegroundColor Green
  Copy-Item $FlashPayloadDir (Join-Path $OutDir 'FlashTool') -Recurse -Force
} else {
  Write-Host "[1/4] 未找到 MSI flash_payload，现场生成 FlashTool\..." -ForegroundColor Yellow
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'scripts\prepare_flash_payload.ps1') -OutputDir (Join-Path $OutDir 'FlashTool')
  if ($LASTEXITCODE -ne 0) { Write-Host "生成 FlashTool 运行时失败！" -ForegroundColor Red; exit 1 }
}

Write-Host "[2/4] 生成便携配置 config.toml..." -ForegroundColor Yellow
$Config = @'
# VoiceStick 绿色便携版配置
# 本文件存在即激活便携模式：所有数据（日志/调试音频）存储在程序目录。
# 删除本文件则恢复标准安装版行为（数据写入 %APPDATA%\VoiceStick）。

asr_provider = "volcengine"
voicestick_api_key = ""
voicestick_cloud_url = "wss://api.xiaozhi.me/voicestick/asr/"
volcengine_api_key = "your_volcengine_asr_api_key"
# 腾讯云 ASR（仅在 asr_provider = "tencent" 时需要）
# tencent_secret_id = "AKID..."
# tencent_secret_key = "..."
# tencent_appid = "1234567890"
llm_base_url = "https://api.openai.com/v1"
llm_api_key = "your_openai_compatible_llm_api_key"
llm_model = "gpt-5.5"
refine_enabled = true
interaction_mode = "hold_to_talk"
resource_id = "volc.seedasr.sauc.duration"
asr_hotwords = ""
paired_device_ids = ""
auto_enter = true
debug_audio_cache = false

[output]
target = "focused_app"
transform = "original"
translation_target = "en"
'@
# 用 UTF-8（无 BOM）写入，TOML 解析器对此无歧义
[System.IO.File]::WriteAllText((Join-Path $OutDir 'config.toml'), $Config, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "[3/4] 生成说明文件 README.txt..." -ForegroundColor Yellow
$Readme = @"
VoiceStick 绿色便携版 v$Version
================================

使用方法：
1. 编辑 config.toml，填入 API Key 等配置（至少需要 ASR 的 API Key）
2. 双击 VoiceStick.exe 启动
3. 在系统托盘中右键图标进行配对等操作

固件烧录工具（VoiceStickFlash）：
- 双击 VoiceStickFlash.exe 打开 COM 口固件烧录工具
- 需要 FlashTool\ 目录（自包含 python-embed + esptool 运行时），已随本包提供
- 支持整包 / 仅应用分区 / 先擦除再整包三种烧录模式

便携模式说明：
- 本目录存在 config.toml 时自动激活便携模式
- 所有数据（配置/日志/调试音频）存储在本目录，不会写入系统 AppData
- 便携模式下不支持开机自启和自动更新
- 删除 config.toml 后程序会恢复标准安装版行为

系统要求：Windows 10 1903+ / Windows 11 x64
"@
[System.IO.File]::WriteAllText((Join-Path $OutDir 'README.txt'), $Readme, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "[4/4] 打包 ZIP..." -ForegroundColor Yellow
$ZipFile = Join-Path $Root "dist\VoiceStick_Portable_v$Version.zip"
if (Test-Path $ZipFile) { Remove-Item $ZipFile -Force }
Compress-Archive -Path $OutDir -DestinationPath $ZipFile

Write-Host ""
Write-Host "=== 打包完成 ===" -ForegroundColor Green
Write-Host ("ZIP: " + $ZipFile)
Write-Host ("目录: " + $OutDir)
