# MSI 内容完整性校验：枚举 File 表，断言顶层必需文件全部在位。
# 背景：v2.4.1 及更早的 MSI 从未包含 sherpa-onnx-c-api.dll / onnxruntime.dll
# （VoiceStick.exe 隐式链接前者，干净机器首装启动即报"找不到 DLL"）。
# 打包清单手工维护，第三方运行时 DLL 曾被整条链（wxs 组件/bat 检查/发布门禁）
# 漏掉——本脚本作为构建后门禁兜底此类遗漏。
# 用法：powershell -File verify_msi_contents.ps1 -Msi <path> [-RequiredFiles a,b,c]
param(
    [Parameter(Mandatory = $true)][string]$Msi,
    # 与 installer/VoiceStick.wxs 顶层组件一一对应；新增顶层文件时同步更新
    [string[]]$RequiredFiles = @(
        'VoiceStick.exe',
        'WinSparkle.dll',
        'config.template.toml',
        'VoiceStickFlash.exe',
        'VoiceStickHidTap.dll',
        'VoiceStickTapInject.exe',
        'sherpa-onnx-c-api.dll',
        'onnxruntime.dll'
    )
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Msi)) {
    Write-Error "MSI 不存在: $Msi"
    exit 1
}

# File 表的 FileName 列为 "短名|长名" 格式（长文件名场景），两段都可能承载目标名
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Msi, 0))
$view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @('SELECT FileName FROM File'))
$view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)

$packaged = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)
while ($true) {
    $rec = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
    if ($null -eq $rec) { break }
    $raw = $rec.GetType().InvokeMember('StringData', 'GetProperty', $null, $rec, @(1))
    foreach ($segment in $raw -split '\|') {
        [void]$packaged.Add($segment.Trim())
    }
}

$missing = @($RequiredFiles | Where-Object { -not $packaged.Contains($_) })

if ($missing.Count -gt 0) {
    Write-Host "FAIL: MSI 缺少必需文件 ($([System.IO.Path]::GetFileName($Msi))):" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}

Write-Host "OK: MSI 内容完整（$($RequiredFiles.Count)/$($RequiredFiles.Count) 必需文件在位）: $([System.IO.Path]::GetFileName($Msi))" -ForegroundColor Green
exit 0
