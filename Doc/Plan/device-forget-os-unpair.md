# 「忘记设备」同步清除 Windows 系统级配对记录

- 状态：已评审（2026-09-07），实施中
- 日期：2026-09-07
- 背景：小米遥控器 2 Pro 的接入硬依赖 OS 级 bond（未 bond 时 ATVV 服务不可发现，见 `Doc/Ref/protocol.md` ATVV 章节；HID 按键依赖系统 HID 栈）。用户痛点：软件内「忘记设备」只清 VoiceStick 记录，Windows 系统蓝牙设备列表残留，必须再去系统设置删除一次。
- 已否决的替代路线：「不注册系统、纯软件端适配」不可行——ATVV 服务 bond 前不可发现 + HID over GATT 未 bond 无设备接口，均为设备固件行为（固件零改动红线）。StickS3 实际同样持有 OS bond（NimBLE bonding，LTK 保留加速重连），只是配对/忘记全程软件内闭环所以用户无感。本方案把「软件内闭环」补齐到忘记路径。

## 1. 现状缺口

`win32_app.cc` Forget 菜单分支只做两件事：`coordinator_->RemovePairedDevice()`（拆会话 + 清内存态）+ `config_.RemovePairedDevice()`（清持久化）。不触碰 Windows 系统配对记录。`BleCentralWin` 已有 `TryUnpairAsync()`（ble_central_win.cc 匿名 namespace），但仅用于连接路径的 stale bond 自愈。

## 2. 方案

```
用户点击「忘记设备」
   ├─ ① coordinator_->RemovePairedDevice()   ← 拆会话（现状已有）
   ├─ ② config_.RemovePairedDevice()          ← 清软件记录（现状已有）
   └─ ③ 新增：BleCentralWin::UnpairOsBondAsync(device_id, address, completion)
          └─ GetDeviceSelectorFromPairingState(true) 枚举系统已配对 BLE 设备
             → 按 System.DeviceInterface.Bluetooth.DeviceAddress 匹配地址
             → TryUnpairAsync(info.Id()) 移除系统 bond
          └─ 结果 DispatchToUiThread 回调 → SetStatus 反馈（失败兜底引导系统设置）
```

设计要点：

| 决策 | 理由 |
|---|---|
| 统一按地址枚举而非复用在连 session 的 DeviceId | Forget 先拆会话再清 bond，此时 session 已关闭；且设备未连接时也须能清理。枚举路径与连接状态解耦，两类设备（VS/RC）统一处理 |
| `UnpairOsBondAsync` 是 BleCentralWin 独有公开方法，不进 `BleCentral` 接口 | 与 `RestartForResume()` 同模式：OS 特有操作，macOS 端无对应概念 |
| 地址匹配仅对第一条命中记录 unpair | 同一物理地址可能枚举出多条 DeviceInformation（GATT/HID/电池各一条接口），UnpairAsync 作用于设备容器，任一条等效 |
| 不做 radio reset | controller key cache 残留问题（历史经验）出现在「unpair 后立即以未配对姿态重连」场景；忘记后的路径是重新配对（重新走 SMP 生成新 key）。列入真机验证项兜底 |
| StickS3 忘记同样清理 OS bond | 行为统一；重连时重新静默走 SMP 配对（真机验证项确认无感） |

## 3. 实施触点

- `src/pair_device_helper.h/.cc`（core，可单测）：新增 `ParseBluetoothAddressString(std::string_view) -> std::optional<std::uint64_t>`，解析 Windows 属性 `"AA:BB:CC:DD:EE:FF"`（大小写/首尾空白容错），与 `FormatBluetoothAddress`（ble_central_win.cc，大端冒号格式）互逆。
- `src/ble_central_win.h/.cc`：新增公开方法 `UnpairOsBondAsync(std::string device_id, std::uint64_t bluetooth_address, std::function<void(bool)> completion)`；复用匿名 namespace 的 `TryUnpairAsync`。
- `src/win32_app.cc` Forget 分支：删除前从 `PairedDeviceEntry` 保存 `bluetooth_address`（删除后迭代器失效），在现有两步删除后发起 ③；completion 经 UI 线程 `SetStatus` 反馈。

## 4. 已知边界

| 场景 | 行为 |
|---|---|
| 设备已不在系统配对列表（手动删过） | 枚举无命中 → 记日志，completion(true)（幂等成功） |
| unpair 失败（权限/系统忙） | 日志 + SetStatus 提示去系统设置手动删除（即现状行为兜底） |
| 多台同型号设备 | 按地址精确匹配，不影响其他设备 |
| 应用退出瞬间的在途 unpair | DispatchToUiThread 队列随窗口销毁不再分发，回调不执行，无悬垂调用 |

## 5. 测试

- 单测（`core_tests.cc`）：`ParseBluetoothAddressString` 正常/大小写/空白/非法输入（段数错、非十六进制、空串）。
- WinRT 枚举与 unpair 属真实 OS 链路，无设备不 mock（红线）——真机验收：
  1. 配对 RC 设备 → 软件内忘记 → Windows 设置 → 蓝牙设备列表不残留 RC 设备；
  2. 忘记后重新配对 RC 设备 → ATVV 音频 + 按键映射正常（验证无 stale key 干扰）；
  3. 忘记 VS 设备 → 列表不残留；重连（重新配对）静默完成、录音正常；
  4. 忘记未连接的设备（仅软件记录、系统已手动删过）→ 无报错、状态栏正常反馈。
