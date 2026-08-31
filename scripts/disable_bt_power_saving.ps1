# 关闭 Intel 蓝牙设备的「允许计算机关闭此设备以节约电源」，
# 并禁用交流供电下的 USB 选择性暂停。
# 目的：修复蓝牙 radio 省电策略导致 BLE 连接 ~60 秒被主动断开的问题。
# 运行方式：管理员 PowerShell 执行（由上层脚本 Start-Process -Verb RunAs 调用）。
$ErrorActionPreference = 'Stop'
$result = @()
try {
    $devices = Get-WmiObject -Namespace root\wmi -Class MSPower_DeviceEnable |
        Where-Object { $_.InstanceName -match 'VID_8087' }
    foreach ($d in $devices) {
        $d.Enable = $false
        [void]$d.Put()
        $result += "set Enable=false: $($d.InstanceName)"
    }
    powercfg /SETACVALUEINDEX SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226 0
    powercfg /SETDCVALUEINDEX SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226 0
    powercfg /SETACTIVE SCHEME_CURRENT
    $result += "usb selective suspend disabled (AC+DC)"
    $result += "OK"
} catch {
    $result += "ERROR: $($_.Exception.Message)"
}
$result | Out-File -FilePath "$env:USERPROFILE\bt_pm_result.txt" -Encoding utf8
