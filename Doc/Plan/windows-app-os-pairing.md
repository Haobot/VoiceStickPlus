# Windows 桌面端：app 配对自动完成系统蓝牙配对

> 日期：2026-09-18　目标平台：Windows（macOS 另行排期）
> 相关文件：`desktop/windows/src/pair_device_dialog.{h,cc}`、`ble_protocol.{h,cc}`、
> `localization.{h,cc}`、测试 `desktop/windows/tests/core_tests.cc`
> 背景：`Doc/Expe/xiaomi-gateway-voice-key-and-hid-passthrough-2026-09-18.md`（问题 12）

## 1. 问题

网关模式下 StickS3 把小米遥控器的按键经 **HOGP** 转发进目标机，这要求 StickS3 自己与目标机
有**系统级蓝牙配对**（否则 Windows 不建立 HID 节点）。但 app 的配对流程对 VS 设备**从不做**
系统配对 —— `pair_device_dialog.cc` 只有在设备类别是小米遥控器时才走
`AttemptXiaomiOsPairing`。

后果（2026-09-18 真机实测）：用户必须在 app 里配一次、再手动去 Windows 蓝牙设置里配一次，
否则现象是「语音键正常、所有按键毫无反应」；而这条路径又会被 app 的失效恢复
（`TryUnpairAsync`）悄悄删掉，反复发作。排查成本极高。

**解绑侧已经是对的**：`win32_app.cc` 的「忘记设备」会同步调
`UnpairOsBondAsync` 清除系统配对（VS/RC 通用，失败有状态栏兜底）。所以本设计
只补**配对侧**这半边。

## 2. 目标与非目标

**目标**：app 内配对 VS 设备时自动完成系统级配对，用户不再需要访问 Windows 蓝牙设置；
失败不阻断既有可用路径，但给出明确、可执行的提示。

**非目标**：
- 不改固件、不改 BLE 协议（不涉及 `Doc/Ref/protocol.md` 与各实现端同步）。
- 不改小米遥控器现有的**硬前置**语义（ATVV GATT 的读取/订阅确实要求 OS bond，失败必须中止）。
- macOS 不做（其配对模型不同且无 HOGP 直通，单独排期）。

## 3. 设计

### 3.1 流程统一

两类设备走同一条「先系统配对、再 GATT 连接」的流程，差别只在**失败策略**：

```
BeginPairing(device)
  └─ 状态栏「正在通过 Windows 蓝牙配对…」（复用 kPairXiaomiOsPairing，措辞本身通用）
     AttemptOsPairing(device, bond_required)     // 由 AttemptXiaomiOsPairing 泛化
        ├─ IsPaired() 已配对      → 视为成功（幂等，跳过 PairAsync）
        ├─ PairAsync 成功          → PostMessage(kBondFinishedMessage{addr, bonded=true})
        └─ 异常/未找到/非 Paired   → PostMessage(kBondFinishedMessage{addr, bonded=false})
              ↓ UI 线程
     HandleBondFinished(addr, bonded)
        ├─ 地址与 pending 候选不符 → 丢弃（沿用现有陈旧消息防护）
        ├─ 小米：bonded=false → 报错中止（硬前置，维持现状）
        └─ VS  ：bonded=false → 状态栏提示，**继续**
              ↓
     StartGattConnect(device)     // 原 VS 分支抽出：状态栏 + 30s 定时器 + on_pair_
```

消息名由 `kXiaomiBondedMessage` 泛化为 `kBondFinishedMessage`，LPARAM 载荷由
「裸 `uint64_t` 地址」扩为堆结构 `BondFinishedPayload{address, bonded}`，
仍由 handler 以 `unique_ptr` 接管（沿用既有约定，不引入新线程语义）。

### 3.2 失败策略抽成纯函数

```cpp
enum class OsBondFollowUp { kContinue, kAbortWithError, kContinueWithWarning };
static OsBondFollowUp PlanAfterOsBondAttempt(bool bonded, bool bond_required);
```

「软前置 / 硬前置」这条被用户拍板的策略**只存在于这一个可单测的函数里**；将来若改为硬前置，
只动这个函数与它的断言。与既有 `PlanReconnectAfterConnectFailure` /
`PlanZombieRecovery` 的纯函数风格一致。

### 3.3 幂等性

`Pairing::IsPaired()` 为真时直接视为已配对成功、跳过 `PairAsync`。
因此**已经手动在系统设置配过对的存量用户升级后行为零变化** —— 这是「不让现在能用的场景
变坏」的具体保证。

### 3.4 文案

新增 **1 条**：`kPairOsBondOptionalFailed`（中英双表）。
进度态复用 `kPairXiaomiOsPairing`，不新造。新增条目放在 `kStaleSessionBody` 之后，
**同步把 `localization.cc` 的 `kStringCount` 哨兵移到新的最后一项**
（2026-09-11 曾因枚举与表不同步导致越界 UB；`core_tests` 已有中英表完整性断言覆盖）。

## 4. 失败与边界

| 情况 | 处理 |
|---|---|
| 设备枚举不到 / `PairAsync` 抛异常 | VS 软降级：提示 + 继续 GATT；小米维持报错中止 |
| `PairAsync` 返回 AccessDenied / AuthenticationFailure / Failed | 同上 |
| `PairAsync` 卡住不返回 | 沿用既有 30s `kPairingTimeoutTimerId` → 既有 `kPairTimedOut` 路径。**已知限制**：会中断本次配对，需用户重试 |
| Windows 弹系统配对确认气泡 | 不处理，观察项（小米路径已有先例，Just Works 理论上静默） |
| 配对时设备未在广播 | 对话框刚扫描到该设备，配对阶段设备仍在广播；若期间被别处连走导致配对失败，走软降级 |

## 5. 测试与验证

- **单测**：`PlanAfterOsBondAttempt` 三分支（成功 / 失败+必需 / 失败+非必需）。
- **构建**：`build_win.bat` + CTest 全过（windows_tests + integration_tests）。
- **真机验收**（`PairAsync` 无法单测，必须真机）：
  1. 托盘「忘记设备」→ Windows 蓝牙列表中 VS-53A8 消失（**验证已存在的解绑侧**）
  2. 托盘「配对设备…」→ 选 VS-53A8 → **不去系统设置**，检查 `Enum\BTHLE\Dev_70041ddc53aa` 自动出现
  3. 遥控器按键直通可用、语音键可用

## 6. 风险

- **系统配对弹窗**：若 Windows 对 BLE 设备弹出确认气泡，会打断「一站式」体验。小米路径已有
  先例且未见弹窗，先按静默预期实现，真机观察后再定。
- **企业策略/驱动异常**下 `PairAsync` 长期失败：软降级保证 app 仍可用，
  但用户会拿到「语音能用、按键没反应」的中间态 —— 由既有僵尸会话提示（`e0db7c63`）兜底指引。
