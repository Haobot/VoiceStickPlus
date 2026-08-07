#Requires -Version 5.1
<#
.SYNOPSIS
    准备 VoiceStickFlash 的自包含 esptool 运行时（python-embed + esptool）。

.DESCRIPTION
    产物布局（<OutputDir> 即 WiX 的 FlashPayloadDir，MSI 安装到 INSTALLFOLDER\FlashTool\）：
        <OutputDir>\python\python.exe        embeddable CPython
        <OutputDir>\python\Lib\site-packages esptool 及依赖
    幂等：payload.version 标记匹配且 `python.exe -m esptool version` 冒烟通过则跳过。
    下载源可用环境变量 VOICESTICK_PYTHON_EMBED_URL 覆盖（镜像 URL 或本地 zip 路径），
    仿 VOICESTICK_WINSPARKLE_URL 模式。本文件须保存为 UTF-8 with BOM（含中文）。
#>
param(
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\desktop\windows\build-msi-x64\flash_payload'),
    [string]$PythonVersion = '3.12.10',
    [string]$EsptoolVersion = '5.2.0'
)

$ErrorActionPreference = 'Stop'

# 统一为绝对路径（Get-ChildItem 返回 FullName，相对路径相减会错位）。
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)

$markerPath = Join-Path $OutputDir 'payload.version'
$markerContent = "python=$PythonVersion esptool=$EsptoolVersion"
$pythonDir = Join-Path $OutputDir 'python'
$pythonExe = Join-Path $pythonDir 'python.exe'

function Test-Payload {
    if (-not (Test-Path $pythonExe)) { return $false }
    $out = & $pythonExe -m esptool version 2>&1 | Out-String
    return ($LASTEXITCODE -eq 0 -and $out -match 'esptool')
}

if ((Test-Path $markerPath) -and ((Get-Content $markerPath -Raw).Trim() -eq $markerContent) -and (Test-Payload)) {
    Write-Host "[flash_payload] already up to date ($markerContent), skip."
} else {

# 1) 下载 embeddable CPython 并解压
if (Test-Path $OutputDir) { Remove-Item $OutputDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $pythonDir | Out-Null

$url = $env:VOICESTICK_PYTHON_EMBED_URL
if (-not $url) {
    $url = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip"
}
$zip = Join-Path $env:TEMP "voicestick-python-embed-$PythonVersion.zip"
if (Test-Path $url) {
    Write-Host "[flash_payload] using local zip: $url"
    Copy-Item $url $zip -Force
} elseif (-not (Test-Path $zip)) {
    Write-Host "[flash_payload] downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
} else {
    Write-Host "[flash_payload] using cached $zip"
}
Expand-Archive -Path $zip -DestinationPath $pythonDir -Force

# 2) 打开 ._pth 的 site-packages（embeddable 默认不加载 site-packages）
$pyMM = ($PythonVersion -split '\.')[0..1] -join '.'   # 3.12.10 -> 3.12
$pyShort = $pyMM -replace '\.', ''                     # 3.12 -> 312
$pthPath = Join-Path $pythonDir "python$pyShort._pth"
if (-not (Test-Path $pthPath)) { throw "expected ._pth not found: $pthPath" }
@"
python$pyShort.zip
.
Lib\site-packages
import site
"@ | Set-Content -Path $pthPath -Encoding Ascii

# 3) 用本机 python 的 pip 装 esptool 到 payload（锁定 win_amd64 / cp3xx 二进制 wheel，
#    避免本机 python 版本与 embeddable 不一致时装错 ABI 的包）
$sitePackages = Join-Path $pythonDir 'Lib\site-packages'
$hostPython = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $hostPython) { throw 'host python not found in PATH (required to pip-install esptool into payload)' }
# pip 索引：默认官方 PyPI（部分镜像可能缺 esptool 包），可用 VOICESTICK_PIP_INDEX_URL 覆盖。
$pipIndex = $env:VOICESTICK_PIP_INDEX_URL
if (-not $pipIndex) { $pipIndex = 'https://pypi.org/simple' }
Write-Host "[flash_payload] installing esptool==$EsptoolVersion into $sitePackages"
& $hostPython -m pip install --disable-pip-version-check --no-input `
    --index-url $pipIndex `
    --target $sitePackages `
    --python-version $pyMM --platform win_amd64 --only-binary :all: `
    "esptool==$EsptoolVersion" pyserial pyyaml
if ($LASTEXITCODE -ne 0) {
    Write-Warning 'strict wheel mode failed, falling back to plain --target install (host interpreter wheels)'
    & $hostPython -m pip install --disable-pip-version-check --no-input `
        --index-url $pipIndex `
        --target $sitePackages "esptool==$EsptoolVersion" pyserial pyyaml
    if ($LASTEXITCODE -ne 0) { throw 'pip install esptool failed' }
}

# 4) 冒烟验证 + 写幂等标记
$ver = & $pythonExe -m esptool version 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or $ver -notmatch 'esptool') {
    throw "smoke test failed: $ver"
}
Write-Host "[flash_payload] smoke: $($ver.Trim())"
Set-Content -Path $markerPath -Value $markerContent -Encoding Ascii
Write-Host "[flash_payload] ready: $OutputDir"
}  # end else（payload 需重建）

# 5) 生成 WiX 片段（本机 wix.exe 为 v4，无 v5 的 <Files> 收割能力）：
#    把 payload 全目录逐文件展开为 Component/File，Feature 里 ComponentGroupRef 引用。
#    组件/文件 ID 取相对路径 SHA1 前 32 位，跨构建稳定；payload 未变时幂等覆盖同内容。
$wxsPath = Join-Path ([System.IO.Path]::GetDirectoryName($OutputDir.TrimEnd('\'))) 'flash_payload.wxs'
Write-Host "[flash_payload] generating $wxsPath"

$sha1 = [System.Security.Cryptography.SHA1]::Create()
function Get-StableId([string]$prefix, [string]$key) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($key.ToLower())
    return $prefix + ([System.BitConverter]::ToString($sha1.ComputeHash($bytes)).Replace('-', '').Substring(0, 32))
}
function ConvertTo-XmlEscaped([string]$text) {
    return $text.Replace('&', '&amp;').Replace('<', '&lt;').Replace('>', '&gt;').Replace('"', '&quot;')
}

$files = Get-ChildItem -Path $OutputDir -Recurse -File
$dirs = @{}   # 相对目录 -> 该目录下的文件相对路径列表
foreach ($f in $files) {
    $rel = $f.FullName.Substring($OutputDir.TrimEnd('\').Length).TrimStart('\')
    $relDir = Split-Path $rel -Parent
    if (-not $dirs.ContainsKey($relDir)) { $dirs[$relDir] = [System.Collections.Generic.List[string]]::new() }
    $dirs[$relDir].Add($rel)
}

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine('<?xml version="1.0" encoding="UTF-8"?>')
[void]$sb.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
[void]$sb.AppendLine('  <!-- 由 prepare_flash_payload.ps1 自动生成，请勿手改（构建产物，gitignored） -->')
[void]$sb.AppendLine('  <Fragment>')
[void]$sb.AppendLine('    <ComponentGroup Id="FlashPayloadGroup">')
foreach ($rel in ($dirs.Values | ForEach-Object { $_ } | Sort-Object)) {
    [void]$sb.AppendLine("      <ComponentRef Id=`"$(Get-StableId 'cmp' $rel)`" />")
}
[void]$sb.AppendLine('    </ComponentGroup>')
[void]$sb.AppendLine('  </Fragment>')
[void]$sb.AppendLine('  <Fragment>')
[void]$sb.AppendLine('    <DirectoryRef Id="FlashToolDir">')

function Write-PayloadDir([string]$relDir, [int]$indent) {
    $pad = ' ' * $indent
    if ($dirs.ContainsKey($relDir)) {
        foreach ($rel in ($dirs[$relDir] | Sort-Object)) {
            $compId = Get-StableId 'cmp' $rel
            $source = ConvertTo-XmlEscaped("`$(var.FlashPayloadDir)\$rel")
            [void]$sb.AppendLine("$pad<Component Id=`"$compId`" Guid=`"*`" Bitness=`"always64`">")
            [void]$sb.AppendLine("$pad  <File Id=`"$(Get-StableId 'fil' $rel)`" Source=`"$source`" KeyPath=`"yes`" />")
            [void]$sb.AppendLine("$pad</Component>")
        }
    }
    $prefix = if ($relDir) { $relDir + '\' } else { '' }
    $childNames = @{}
    foreach ($key in $dirs.Keys) {
        if (-not $key.StartsWith($prefix)) { continue }
        $rest = $key.Substring($prefix.Length)
        if ($rest -eq '') { continue }
        $first = ($rest -split '\\')[0]
        $childNames[$first] = $true
    }
    foreach ($name in ($childNames.Keys | Sort-Object)) {
        $childRel = $prefix + $name
        [void]$sb.AppendLine("$pad<Directory Id=`"$(Get-StableId 'dir' $childRel)`" Name=`"$(ConvertTo-XmlEscaped $name)`">")
        Write-PayloadDir $childRel ($indent + 2)
        [void]$sb.AppendLine("$pad</Directory>")
    }
}
Write-PayloadDir '' 6

[void]$sb.AppendLine('    </DirectoryRef>')
[void]$sb.AppendLine('  </Fragment>')
[void]$sb.AppendLine('</Wix>')
[System.IO.File]::WriteAllText($wxsPath, $sb.ToString(), [System.Text.UTF8Encoding]::new($true))
Write-Host "[flash_payload] wxs components: $($files.Count) files"
