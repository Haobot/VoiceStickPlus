# Extract volcengine_api_key from %APPDATA%\VoiceStick\config.toml to stdout.
# Consumed by build-msi.bat to bake a built-in API key into VoiceStick.exe at compile
# time, so ActiveApiKey() can fall back to it on first launch and skip the ASR onboarding
# step for new users. See Doc/Plan/windows-builtin-api-key.md.
$ErrorActionPreference = 'SilentlyContinue'
$configPath = Join-Path $env:APPDATA 'VoiceStick\config.toml'
if (-not (Test-Path $configPath)) { exit 0 }
$content = Get-Content -Raw $configPath
if ($content -match 'volcengine_api_key\s*=\s*"([^"]+)"') {
    Write-Output $matches[1]
}
