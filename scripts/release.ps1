#requires -Version 5.1
<#
.SYNOPSIS
  VoiceStick 一键发布脚本（在 Windows 签名机上运行）。

.DESCRIPTION
  串起完整发布流程：版本文件同步 -> MSI 构建+签名 -> commit/tag/push ->
  等待 release.yml CI -> 上传 MSI(+sha256) -> 触发网站部署 -> 验证全部更新 URL。
  固件/macOS/网站由 GitHub Actions 负责；本脚本只做本地能与编排。

  详见 Doc/Plan/downloads-and-release.md 阶段一。

.PARAMETER Version
  目标版本号（如 2.3.9）。脚本会把 VERSION 与 firmware/version.txt 写成该值。

.PARAMETER SkipMsi
  跳过 Windows MSI 构建与上传（纯固件发布）。

.PARAMETER Repo
  目标仓库，默认 Haobot/VoiceStickPlus。

.PARAMETER DryRun
  只打印将执行的步骤，不实际执行（git commit/tag/push 也不执行）。

.EXAMPLE
  powershell -File scripts\release.ps1 -Version 2.3.9
  powershell -File scripts\release.ps1 -Version 2.3.9 -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [switch]$SkipMsi,
    [string]$Repo = "Haobot/VoiceStickPlus",
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$ProjectDir = Split-Path -Parent $PSScriptRoot
$Tag = "v$Version"
$RepoOwner = $Repo.Split('/')[0].ToLower()
$RepoName = $Repo.Split('/')[1]
$PagesBase = "https://$RepoOwner.github.io/$RepoName"
# 国内 COS 分发面域名（Doc/Rfc/tencent-cos-domestic-distribution-2026-09-09.md）
$DistDomain = "https://dl.davenger.cloud"
$MsiDir = Join-Path $ProjectDir 'desktop\windows\build-msi-x64'
$MsiNames = @("VoiceStick_${Version}_zh-CN.msi", "VoiceStick_${Version}_en-US.msi")
$report = New-Object System.Collections.Generic.List[string]

function Invoke-Step {
    # DryRun 时只打印；否则执行并检查退出码（git/gh/build-msi 等控制台命令统一走这里）
    param([string]$Description, [scriptblock]$Action)
    Write-Host ""
    Write-Host "==> $Description" -ForegroundColor Cyan
    if ($DryRun) {
        Write-Host "    [DRYRUN] 跳过：$($Action.ToString().Trim())" -ForegroundColor Yellow
        return
    }
    & $Action
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        throw "步骤失败（退出码 $LASTEXITCODE）：$Description"
    }
}

function Assert-Tool {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "缺少必需工具：$Name"
    }
}

function Get-LatestRunId {
    param([string]$Workflow)
    $runs = gh run list --repo $Repo --workflow $Workflow --limit 1 --json databaseId 2>$null | ConvertFrom-Json
    if ($runs) { [long]$runs[0].databaseId } else { [long]0 }
}

function Wait-WorkflowRun {
    # 等待 workflow 出现 databaseId 大于基线的新 run 并跑完；Branch 按 headBranch 过滤（tag 触发的 run headBranch 即 tag 名）
    param([string]$Workflow, [long]$BaselineRunId, [string]$Branch, [int]$TimeoutMinutes = 40)
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    while ((Get-Date) -lt $deadline) {
        $args = @('run', 'list', '--repo', $Repo, '--workflow', $Workflow, '--limit', '10', '--json', 'databaseId,status,conclusion,headBranch')
        if ($Branch) { $args += @('--branch', $Branch) }
        $runs = (gh @args 2>$null | ConvertFrom-Json) | Where-Object { [long]$_.databaseId -gt $BaselineRunId }
        $latest = $runs | Select-Object -First 1
        if ($latest -and $latest.status -eq 'completed') {
            if ($latest.conclusion -ne 'success') {
                throw "$Workflow run $($latest.databaseId) 结论为 $($latest.conclusion)，详见 https://github.com/$Repo/actions"
            }
            return
        }
        Start-Sleep -Seconds 20
    }
    throw "等待 $Workflow 超时（${TimeoutMinutes} 分钟），请到 https://github.com/$Repo/actions 查看进度"
}

function Test-Url {
    param([string]$Url, [string]$ExpectContent)
    try {
        if ($ExpectContent) {
            $body = (Invoke-WebRequest -UseBasicParsing -Uri $Url -TimeoutSec 30).Content
            if ($body -notmatch [regex]::Escape($ExpectContent)) { return $false }
        } else {
            Invoke-WebRequest -UseBasicParsing -Method Head -Uri $Url -TimeoutSec 30 | Out-Null
        }
        return $true
    } catch {
        return $false
    }
}

# ---------- 1. 前置校验 ----------
if ($DryRun) { Write-Host "*** DRYRUN 模式：不执行任何实际变更 ***" -ForegroundColor Yellow }
Invoke-Step "前置工具检查（gh 已认证）" {
    Assert-Tool gh
    Assert-Tool git
    gh auth status | Out-Null
}

$branch = git -C $ProjectDir rev-parse --abbrev-ref HEAD
if ($branch -ne 'main' -and -not $DryRun) { throw "必须在 main 分支发布，当前是 $branch" }
if ($branch -ne 'main') { Write-Host "警告：当前分支 $branch（DRYRUN 下仅提示，正式发布必须在 main）" -ForegroundColor Yellow }
$dirty = git -C $ProjectDir status --porcelain
if ($dirty -and -not $DryRun) { throw "工作区不干净：`n$dirty`n请先提交或暂存后再发布" }
if ($dirty) { Write-Host "警告：工作区有未提交内容（DRYRUN 下仅提示）：" -ForegroundColor Yellow; $dirty | ForEach-Object { Write-Host "  $_" } }

$minVersion = (Get-Content (Join-Path $ProjectDir 'FIRMWARE_MIN_VERSION') -Raw).Trim()
if ($minVersion -notmatch '^\d+\.\d+\.\d+$') { throw "FIRMWARE_MIN_VERSION 格式非法：$minVersion" }
if ([version]$minVersion -gt [version]$Version) {
    throw "FIRMWARE_MIN_VERSION($minVersion) 不能大于发布版本($Version)"
}

# ---------- 2. 版本文件同步 ----------
Invoke-Step "版本文件同步 VERSION / firmware/version.txt -> $Version" {
    [IO.File]::WriteAllText((Join-Path $ProjectDir 'VERSION'), $Version)
    [IO.File]::WriteAllText((Join-Path $ProjectDir 'firmware\version.txt'), $Version)
}

# ---------- 3. MSI 构建 + 签名（可跳过）----------
if (-not $SkipMsi) {
    Invoke-Step "构建并签名 Windows MSI（build-msi.bat，读 VERSION=$Version）" {
        & (Join-Path $PSScriptRoot 'build-msi.bat')
    }
    if (-not $DryRun) {
        foreach ($m in $MsiNames) {
            if (-not (Test-Path (Join-Path $MsiDir $m))) { throw "MSI 未生成：$(Join-Path $MsiDir $m)" }
        }
    }
} else {
    Write-Host "==> 跳过 Windows MSI（-SkipMsi）" -ForegroundColor DarkGray
}

# ---------- 4. commit + tag + push ----------
Invoke-Step "提交版本变更" {
    $pending = git -C $ProjectDir status --porcelain VERSION firmware/version.txt
    if ($pending) {
        git -C $ProjectDir add VERSION firmware/version.txt
        git -C $ProjectDir commit -m "chore(release): 发布 $Tag"
    } else {
        Write-Host "    版本文件已是目标值，无需提交"
    }
}
Invoke-Step "打 tag $Tag 并推送 main 与 tag" {
    git -C $ProjectDir tag -a $Tag -m "VoiceStick $Version"
    git -C $ProjectDir push origin main
    git -C $ProjectDir push origin $Tag
}

# ---------- 5. 等待 release.yml（固件构建 + GitHub Release 发布）----------
Invoke-Step "等待 Release Build CI 完成（tag $Tag）" {
    Wait-WorkflowRun -Workflow 'release.yml' -BaselineRunId 0 -Branch $Tag
}

# ---------- 6. 上传 MSI + sha256（可跳过，失败重试一次）----------
if (-not $SkipMsi) {
    Invoke-Step "生成 MSI sha256 并上传到 $Tag" {
        $files = @()
        foreach ($m in $MsiNames) {
            $p = Join-Path $MsiDir $m
            $hash = (Get-FileHash -Algorithm SHA256 $p).Hash.ToLower()
            [IO.File]::WriteAllText("$p.sha256", "$hash  $m`n")
            $files += $p
            $files += "$p.sha256"
        }
        $uploaded = $false
        foreach ($attempt in 1..2) {
            gh release upload $Tag $files --repo $Repo
            if ($LASTEXITCODE -eq 0) { $uploaded = $true; break }
            Write-Host "上传失败（第 $attempt 次），3 秒后重试..." -ForegroundColor Yellow
            Start-Sleep -Seconds 3
        }
        if (-not $uploaded) { throw "MSI 上传重试后仍失败" }
    }
}

# ---------- 7. 触发并等待网站部署（appcast + downloads.json + 固件同步）----------
$deployBaseline = Get-LatestRunId -Workflow 'deploy-website.yml'
Invoke-Step "触发 deploy-website.yml 并等待完成" {
    gh workflow run deploy-website.yml --repo $Repo --ref main
    Start-Sleep -Seconds 20   # 等 run 出现在列表里
    Wait-WorkflowRun -Workflow 'deploy-website.yml' -BaselineRunId $deployBaseline -TimeoutMinutes 15
}

# ---------- 8. 验证全部更新 URL ----------
Write-Host ""
Write-Host "==> 验证更新 URL" -ForegroundColor Cyan
$checks = [ordered]@{
    "appcast.xml（Sparkle/WinSparkle 更新源）" = @{ Url = "$PagesBase/appcast.xml"; Expect = $null; Retry = 1 }
    "manifest.json（固件 OTA，含 min_version）" = @{ Url = "https://github.com/$Repo/releases/latest/download/manifest.json"; Expect = 'min_version'; Retry = 1 }
    "downloads.json（下载页数据）" = @{ Url = "$PagesBase/downloads.json"; Expect = $null; Retry = 6 }
    "固件 OTA bin" = @{ Url = "https://github.com/$Repo/releases/download/$Tag/voicestick-firmware-sticks3-ota-$Version.bin"; Expect = $null; Retry = 1 }
    "固件 merged bin" = @{ Url = "https://github.com/$Repo/releases/download/$Tag/voicestick-firmware-sticks3-merged-$Version.bin"; Expect = $null; Retry = 1 }
    "COS appcast.xml（国内更新源）" = @{ Url = "$DistDomain/appcast.xml"; Expect = $null; Retry = 3 }
    "COS manifest.json（国内固件 OTA 源）" = @{ Url = "$DistDomain/firmware/latest/manifest.json"; Expect = 'min_version'; Retry = 3 }
    "COS downloads.json（国内下载页数据）" = @{ Url = "$DistDomain/downloads.json"; Expect = $null; Retry = 6 }
    "COS 固件 OTA bin" = @{ Url = "$DistDomain/firmware/$Tag/voicestick-firmware-sticks3-ota-$Version.bin"; Expect = $null; Retry = 3 }
    "COS 固件 merged bin" = @{ Url = "$DistDomain/firmware/$Tag/voicestick-firmware-sticks3-merged-$Version.bin"; Expect = $null; Retry = 3 }
}
if (-not $SkipMsi) {
    $checks["Windows MSI zh-CN"] = @{ Url = "https://github.com/$Repo/releases/download/$Tag/VoiceStick_${Version}_zh-CN.msi"; Expect = $null; Retry = 1 }
    $checks["Windows MSI en-US"] = @{ Url = "https://github.com/$Repo/releases/download/$Tag/VoiceStick_${Version}_en-US.msi"; Expect = $null; Retry = 1 }
    $checks["COS Windows MSI en-US（appcast enclosure）"] = @{ Url = "$DistDomain/software/windows/$Tag/VoiceStick_${Version}_en-US.msi"; Expect = $null; Retry = 6 }
}
if ($DryRun) {
    $checks.GetEnumerator() | ForEach-Object { Write-Host "    [DRYRUN] 将检查：$($_.Value.Url)" }
} else {
    foreach ($c in $checks.GetEnumerator()) {
        $spec = $c.Value
        $ok = $false
        for ($i = 1; $i -le [int]$spec.Retry; $i++) {
            if (Test-Url -Url $spec.Url -ExpectContent $spec.Expect) { $ok = $true; break }
            if ($i -lt [int]$spec.Retry) { Start-Sleep -Seconds 20 }
        }
        if ($ok) { $report.Add("✅ $($c.Key)"); Write-Host "  ✅ $($c.Key)" -ForegroundColor Green }
        else { $report.Add("❌ $($c.Key) -> $($spec.Url)"); Write-Host "  ❌ $($c.Key) -> $($spec.Url)" -ForegroundColor Red }
    }
}

# ---------- 9. 发布报告 ----------
Write-Host ""
Write-Host "==================== 发布报告 ====================" -ForegroundColor Magenta
if ($DryRun) {
    Write-Host "DRYRUN 完成，未执行任何变更。" -ForegroundColor Yellow
} else {
    $report | ForEach-Object { Write-Host "  $_" }
    $failed = @($report | Where-Object { $_ -like '❌*' })
    Write-Host ""
    Write-Host "Release: https://github.com/$Repo/releases/tag/$Tag"
    if ($failed.Count) {
        Write-Host "有 $($failed.Count) 项验证失败，请人工检查！" -ForegroundColor Red
        exit 1
    }
    Write-Host "全部通过。" -ForegroundColor Green
}
