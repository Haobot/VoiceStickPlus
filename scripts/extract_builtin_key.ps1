param(
    # 凭据源：默认用户 config.toml；发布构建由 build-msi.bat 传 desktop/Ref/Config/config.toml
    # （测试凭据专用配置，不随用户配置漂移）
    [string]$ConfigPath = (Join-Path $env:APPDATA 'VoiceStick\config.toml')
)
$ErrorActionPreference = 'SilentlyContinue'
# Extract all built-in credentials from the local config.toml and emit them as
# "VOICESTICK_BUILTIN_<NAME>=<value>" lines. build-msi.bat loops over these lines
# and sets each as an env var before invoking CMake. Baked into VoiceStick.exe at
# compile time; Active*() accessors fall back to them on first launch.
$configPath = $ConfigPath
if (-not (Test-Path $configPath)) { exit 0 }
$content = Get-Content -Raw $configPath

function Emit-Builtin {
    param([string]$Name, [string]$Pattern)
    if ($content -match $Pattern) {
        Write-Output "$Name=$($matches[1])"
    }
}

Emit-Builtin 'VOICESTICK_BUILTIN_API_KEY'           'volcengine_api_key\s*=\s*"([^"]+)"'
Emit-Builtin 'VOICESTICK_BUILTIN_TENCENT_SECRET_ID'  'tencent_secret_id\s*=\s*"([^"]+)"'
Emit-Builtin 'VOICESTICK_BUILTIN_TENCENT_SECRET_KEY' 'tencent_secret_key\s*=\s*"([^"]+)"'
Emit-Builtin 'VOICESTICK_BUILTIN_TENCENT_APPID'      'tencent_appid\s*=\s*"([^"]+)"'
Emit-Builtin 'VOICESTICK_BUILTIN_LLM_API_KEY'        'llm_api_key\s*=\s*"([^"]+)"'
Emit-Builtin 'VOICESTICK_BUILTIN_LLM_BASE_URL'       'llm_base_url\s*=\s*"([^"]+)"'
Emit-Builtin 'VOICESTICK_BUILTIN_LLM_MODEL'          'llm_model\s*=\s*"([^"]+)"'
