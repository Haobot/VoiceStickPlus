# Pair Device BLE 发现兼容性修复设计

## 背景

部分电脑在 Pair Device 窗口中无法搜索到新的 Stick BLE。现有实现依赖 BLE 广播中的 VoiceStick service UUID 和 `VS-XXXX` LocalName，但不同主机蓝牙适配器、驱动和系统版本对 ADV PDU、scan response、service filter 的合并与暴露行为不完全一致。

当前相关实现：

- 固件：`firmware/components/voice_ble/voice_ble.c` 负责广播 `service_uuid` 与设备名。
- Windows Pair：`desktop/windows/src/pair_device_dialog.cc` 使用 `BluetoothLEAdvertisementWatcher` 主动扫描。
- Windows 自动重连：`desktop/windows/src/ble_central_win.cc` 使用已配对 ID 扫描并连接。
- macOS Pair/Onboarding：`PairDeviceWindowController.swift` 和 `OnboardingWindowController.swift` 使用 CoreBluetooth service filter 扫描。

## 目标

1. 提升 Pair Device 阶段在不同电脑上的发现率。
2. 增加诊断信息，区分“未收到广播”和“收到但被过滤”。
3. 保持已配对设备后台自动重连路径尽量不变，避免增加正常重连耗时。
4. Windows 增加手动输入设备 ID 的兜底能力。
5. 避免因临时地址 fallback 保存错误设备 ID 或错误蓝牙地址。

## 非目标

- 不重构 BLE GATT 连接、ASR、协调器状态机。
- 不改变 VoiceStick GATT service/characteristic UUID。
- 不在 macOS 增加手动输入设备 ID。
- 不让 service-only 临时候选直接配对，除非后续获得合法 `VS-XXXX` 名称。

## 总体方案

采用“固件广播瘦身 + Pair 阶段宽松扫描 + Windows 可诊断兜底”的产品化方案。

### 固件广播

固件仅调整广播字段布局：

- ADV PDU：保留 flags 和 VoiceStick 128-bit service UUID。
- Scan response：保留完整 LocalName `VS-XXXX`。

从 ADV PDU 中移除 shortened name，降低 31 字节容量边界风险。fast/slow advertising 时长、间隔、连接参数、bonding 和 GATT 定义保持不变。

### Windows Pair Device

Pair dialog 成为可诊断的扫描控制器：

- 使用 Active Scan，记录 scan start/stop/restart/failure。
- 记录收到广告总数、VoiceStick 候选数量、扫描重启次数、最近扫描事件时间。
- 记录候选解析结果与丢弃原因。
- 15 秒无候选时自动重启 watcher；只在 Pair dialog 打开期间生效，只在候选列表为空时触发。
- 已配对设备不再静默隐藏，而是在列表中显示 `VS-XXXX (paired)`。
- 有 VoiceStick service UUID 但暂时没有 LocalName 的广播可显示为临时候选，但不可直接 Pair；后续同地址收到 `VS-XXXX` 后更新为真实候选。
- 增加 Windows 专属手动输入 4 位设备 ID 兜底。手动输入只保存 device ID，不伪造蓝牙地址；后台自动重连路径按现有扫描逻辑等待并连接该 ID。

### macOS Pair / Onboarding

Pair 和 Onboarding 阶段改为无 service filter 扫描：

```swift
central.scanForPeripherals(withServices: nil)
```

在 `didDiscover` 中本地过滤：

- 如果 advertisement local name 或 peripheral name 可解析为 `VS-XXXX`，加入列表并允许保存。
- 如果 service UUID 包含 VoiceStick 但无合法 name，可显示等待状态或忽略；不得保存。

后台已配对自动重连仍使用原有 `BleCentral.scanIfReady()`，不扩大扫描范围。

## 详细设计

### 固件

修改 `start_advertising_with_mode()`：

- 删除 ADV fields 中的 `fields.name`、`fields.name_len`、`fields.name_is_complete` 设置。
- 保留 scan response 的 complete name。

验收：串口仍输出 `advertising as VS-XXXX mode=fast/slow`，主机仍可发现 service UUID 和 name。

### Windows PairDeviceDialog 数据结构

新增或抽取可测试的候选逻辑：

- `PairingDevice` 增加候选来源与可配对状态：
  - `id_source`: `name` / `address_fallback` / `manual`
  - `is_existing_device`
  - `is_temporary_candidate`
- 纯函数用于测试：
  - 手动 ID 解析与校验。
  - 候选标题生成。
  - 候选是否允许 Pair。

扫描统计字段：

- `received_advertisement_count_`
- `voice_stick_candidate_count_`
- `scan_restart_count_`
- `last_scan_event_time_`

定时器：

- `kScanRestartTimerId`
- 周期 15 秒。
- 仅当候选列表为空且未处于配对中时重启 watcher。

### Windows 列表与手动输入

Pair dialog UI 增加：

- 列表保留 Device / Signal / Bluetooth Address。
- 状态文案显示 `Scanning`、`N found`、`Still scanning... retrying`。
- 手动输入框提示 `Enter ID shown on Stick`。
- Pair 按钮行为：
  1. 优先处理选中的列表项。
  2. 如果无选中项且手动输入合法，则保存手动 ID。
  3. 如果输入非法，提示输入 4 位十六进制 ID。

行为规则：

- 真实 name 候选且未配对：允许 Pair，走现有 direct connect。
- 已配对候选：不重复 Pair，提示先 Forget 或等待自动连接。
- service-only 临时候选：不允许 Pair，提示等待设备名。
- 手动输入：保存 ID 并更新 coordinator 配对列表，不立即 direct connect。

### macOS 本地过滤

Pair/Onboarding 的 `didDiscover` 使用共同规则：

1. 读取 `CBAdvertisementDataLocalNameKey`、`peripheral.name`。
2. 尝试解析 `VS-XXXX`。
3. 读取 `CBAdvertisementDataServiceUUIDsKey` 判断是否包含 VoiceStick service。
4. 有合法 ID 时加入列表；只有 service 时不保存。

## 错误处理

Windows：

- watcher start 失败：显示 HRESULT 和开启蓝牙/重试提示。
- 15 秒无候选：自动重启 watcher，状态显示重试中。
- 多次无候选：继续显示手动输入提示，不停止扫描。
- 手动 ID 非法：提示 `Enter the 4-digit ID shown on the Stick screen`。
- 已配对设备：提示 `VS-XXXX is already paired. Use Forget Device first or wait for it to reconnect.`。
- 临时候选：提示 `Waiting for device name... move the Stick closer or wake it.`。

macOS：

- 蓝牙不可用仍显示 `Bluetooth unavailable`。
- service-only 但无合法 name 的发现事件不得保存配置。

## 性能与时长影响

- 自动重连路径不改变，不扩大后台扫描范围。
- Windows watcher 自动重启只在 Pair dialog 中候选为空时触发，阈值为 15 秒。
- macOS 无 service filter 扫描只用于 Pair/Onboarding；本地过滤开销很小。
- 固件广播瘦身不会增加发现时间，预期降低丢包/解析失败概率。

## 测试计划

遵循 TDD，先写自动化测试再改实现。

### Windows CTest

扩展 `desktop/windows/tests/core_tests.cc`，覆盖：

- 手动 ID 校验：`abcd`、`VS-abcd` 归一化为 `ABCD`，非 4 位 hex 无效。
- `HasVoiceStickServiceUuid()` 对固件 ADV 格式仍返回 true。
- 候选标题：已配对显示 `(paired)`，临时候选显示 address 或 waiting 状态。
- 候选可 Pair 判断：真实未配对可 Pair，已配对不可 Pair，临时候选不可 Pair。

如需新增 helper，应放入 Windows 可测试核心代码，而不是只写在 UI 回调里。

### Windows 手动验证

1. 构建并运行 CTest。
2. 启动应用，打开 Pair Device。
3. 验证日志包含 scan start、candidate、drop reason、restart。
4. 验证已配对设备显示 `(paired)`。
5. 验证手动输入合法 ID 后配置更新并开始后台自动连接。
6. 在问题电脑上确认无法搜索时可通过日志判断原因。

### macOS 验证

- `swift build` 通过。
- Pair Device 能显示 `VS-XXXX`。
- Onboarding 首次配对能显示并保存合法 ID。
- 已配对自动重连不受影响。

### 固件验证

- `idf.py build` 通过。
- 构建通过后按项目约定烧录当前设备。
- 通过串口日志确认广播仍启动。
- 用 Windows/macOS Pair 验证可发现。

## 风险与缓解

1. **Windows 手动 ID 保存后未立即连接**
   - 缓解：UI 明确提示“已保存，正在等待设备广播”。

2. **service-only 候选无法直接 Pair，用户以为卡住**
   - 缓解：显示等待设备名提示，并保留手动输入兜底。

3. **macOS 无 service filter 收到大量无关设备**
   - 缓解：只显示合法 VoiceStick 候选，不改变后台自动扫描。

4. **固件广播字段调整影响部分主机发现**
   - 缓解：service UUID 保留在 ADV，name 保留在 scan response；这是更标准、更低风险的布局。

## 实施顺序

1. Windows 纯函数测试与 helper。
2. Windows Pair dialog 日志、列表、手动输入、15 秒重启。
3. macOS Pair/Onboarding 宽松扫描与本地过滤。
4. 固件广播瘦身。
5. 运行 Windows CTest、macOS build、固件 build，并烧录验证。
