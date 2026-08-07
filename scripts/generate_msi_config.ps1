param(
    [Parameter(Mandatory = $true)]
    [string]$SourceConfig,
    [Parameter(Mandatory = $true)]
    [string]$TemplateConfig,
    [Parameter(Mandatory = $true)]
    [string]$OutputConfig
)

# generate_msi_config.ps1
# Injects real ASR/LLM credentials from a source config.toml into the placeholder
# config.template.toml, producing an MSI config file with real keys (build artifact,
# never committed).
#
# Why: after MSI install, the local %APPDATA%\VoiceStick\config.toml should carry
# test keys so Volcengine/Tencent/LLM work out of the box (Active*() prefers the
# config.toml values and only falls back to exe-embedded keys when empty). This
# script is invoked by build-msi.bat to generate the keyed config.template.toml in
# the build dir; WiX installs it to INSTALLFOLDER and copies it over %APPDATA% at
# install time.
#
# Usage:
#   powershell -File scripts/generate_msi_config.ps1 ^
#       -SourceConfig "dist\VoiceStick_Portable_v2.0.0\config.toml" ^
#       -TemplateConfig "desktop\windows\resources\config.template.toml" ^
#       -OutputConfig "desktop\windows\build-msi-x64\config.template.toml"

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $SourceConfig)) {
    Write-Error "Source config not found: $SourceConfig"
    exit 1
}
if (-not (Test-Path $TemplateConfig)) {
    Write-Error "Template config not found: $TemplateConfig"
    exit 1
}

# Extract a key field value from source content. Values are never printed.
function Get-KeyValue {
    param([string]$Content, [string]$Key)
    $m = [regex]::Match($Content, [regex]::Escape($Key) + '\s*=\s*"([^"]*)"')
    if ($m.Success) { return $m.Groups[1].Value }
    return ''
}

$sourceContent = Get-Content -Raw $SourceConfig

# Credential fields to inject (same 7 as extract_builtin_key.ps1).
$keyFields = @(
    'volcengine_api_key',
    'tencent_secret_id',
    'tencent_secret_key',
    'tencent_appid',
    'llm_api_key',
    'llm_base_url',
    'llm_model'
)

$templateContent = Get-Content -Raw $TemplateConfig
$injected = 0

foreach ($key in $keyFields) {
    $value = Get-KeyValue -Content $sourceContent -Key $key
    if (-not $value) {
        Write-Warning "Key '$key' is empty/missing in source config, leaving template value as-is."
        continue
    }
    # Replace only when the template still holds the empty-string placeholder.
    $pattern = [regex]::Escape($key) + '\s*=\s*""'
    if ($templateContent -match $pattern) {
        # Escape backslashes and double quotes for the TOML string literal.
        $escaped = $value.Replace('\', '\\').Replace('"', '\"')
        $templateContent = [regex]::Replace($templateContent, $pattern, ($key + ' = "' + $escaped + '"'))
        $injected++
    } else {
        Write-Warning "Template has no empty placeholder for '$key'; skipping."
    }
}

$outDir = Split-Path -Parent $OutputConfig
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

$templateContent | Set-Content -Path $OutputConfig -Encoding UTF8

Write-Output "Generated MSI config with $injected of $($keyFields.Count) keys injected: $OutputConfig"
if ($injected -eq 0) {
    Write-Warning "No keys were injected - MSI config will have empty credentials."
}
