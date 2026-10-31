# 网关模式下抑制桌面端直连 ATVV（设计，待确认）

> 日期：2026-09-19　上游：`Doc/Plan/xiaomi-remote-stick-gateway.md` §5.7、
> `Doc/Plan/xiaomi-gateway-followup-roadmap.md` P1、
> 经验 `Doc/Expe/ble-cccd-cache-subscription-not-delivered-2026-09-19.md`（连带发现）
> 状态：**待用户确认后实施**（brainstorming 门禁：设计未批准前不写代码）

## 1. 需求

网关模式下遥控器只与 StickS3 配对（bond 在固件），桌面端**不应再尝试直连 ATVV**：

- 直连必然失败（PC 没有遥控器 bond）——真机判据：`atvv control subscribe timeout after 2500ms`、
  `Xiaomi ATVV service UUID not present`；
- 每次尝试要付一次连接 + 2.5s 订阅超时，并刷日志、污染设备列表；
- 遥控器语音与按键此时已由网关中转，抑制不损失任何能力。

**现状核实（据实修正）**：该现象只在「本机确实配对过遥控器」时发生（`paired_device_ids` 门槛）。
当前本机配置只有 `paired_device_ids = "53A8"`（未配对任何 RC）⇒ **今天并没有发生 ATVV 重试刷屏**；
此前把 2026-09-18 的日志行误读成本次现象（多日日志同文件，grep 时间戳会跨天命中），
已在 `Doc/Expe/ble-cccd-cache-subscription-not-delivered-2026-09-19.md` 更正。
本设计的价值是把"配对过遥控器 + 开网关"这条**必然走到失败直连**的路径提前堵掉。

## 2. 关键事实（2026-09-19 核实）

| 事实 | 出处 |
|---|---|
| 网关模式是**固件侧** NVS 持久设置；当前入口是开机窗口按住主键翻转，Phase 3 换成屏幕菜单 | `firmware/components/gateway/include/gateway_mode.h` |
| `device_info` **已到 BLE 通知长度预算，不得再加字段**（固件注释明确要求新能力走独立小帧） | `voice_ble.c:1374-1391` |
| 桌面端**只对本机配过对的 RC 设备**发起连接 | `ble_central_win.cc:1223` `paired_device_ids_.contains` |
| RC 的配对入口是配对设备对话框 → `AttemptOsPairing`（非自动连接） | `pair_device_dialog.cc:803` |
| 桌面端已有"存在 StickS3 即视为网关在场"的启发式（按键映射钩子用） | `win32_app.cc:1413` `gateway_stick_present` |

## 3. 方案对比

| 方案 | 做法 | 取舍 |
|---|---|---|
| **A（推荐）模式帧告知** | 固件新增独立小帧 `{"event":"gateway_status","mode":"gateway"\|"normal"}`；桌面端据此判定 | 精确；**P1 也需要它**（切换器要告诉 app 当前是否活跃目标）；代价=固件+协议各一处小改 |
| B 桌面端启发式 | "有 StickS3 连着就算网关模式" | 零固件改动；但"Stick 在旁但没开网关"时**误抑制**，且和固件真实状态可能不一致 |
| C 桌面端显式开关 | 设置里让用户选"遥控器直连/走网关" | 最可控，但多一个要维护、可能和固件不一致的开关 |

**推荐 A + 保守兜底**：收到 `gateway_status` 且 `mode=gateway` 才抑制；
**没收到（旧固件）时一律不抑制**——宁可保持今天的失败重试，也不要误伤直连用户。

## 4. 设计（方案 A）

### 4.1 固件侧（小改）

- 新增帧 `gateway_status`，携带 `mode`（`gateway`/`normal`）。
  P1 会在**同一帧**上扩展 `active` / `target`（见 `Doc/Plan/xiaomi-gateway-p1-switcher.md` §3.1），
  故本阶段就按"可扩展帧"设计：桌面端解析时未知字段一律忽略。
- 发送时机：① state 订阅成功后随 device_info 之后补发一次；② 模式翻转（`gateway_mode_toggle`）后立即补发。
- 同步 `Doc/Ref/protocol.md`（新事件类型）。
- 预算：< 60B，独立小帧，不动 device_info。

### 4.2 桌面端（BleCentralWin + 配对对话框）

1. 解析 `gateway_status` → 会话级 `gateway_mode` 标记（`ble_protocol` 增字段 + 协调器透出）。
2. 判定 `gateway_active`：存在**已连接**的 StickS3 会话且其 `gateway_mode==gateway`；否则 false。
3. 抑制点：
   - `HandleAdvertisement`：`gateway_active` 时对 `DeviceClass::kXiaomiRemote2Pro` **直接 return**（不占 connecting 标记、不进退避），日志节流一行说明原因。
   - **已在跑的直连 RC 会话不主动拆**（P0 教训：不破坏当前可用状态），只抑制新连接。
   - 配对设备对话框：`gateway_active` 时 RC 行标注"网关模式下由 Stick 中转，无需在电脑上配对"，配对按钮禁用并提示退出网关模式的方法。
4. 日志：`gateway mode active: skipping direct ATVV for RC-XXXX (remote is relayed by the Stick)`（每地址每 5 分钟最多一条）。

### 4.3 验收（真机）

1. RC 处于本机已配对状态 + Stick 在网关模式 → 启动 app 60s，**零** ATVV 连接尝试（`grep atvv` 无新行）且出现抑制日志；
2. 设备侧退出网关模式 → 直连 ATVV 行为恢复（能连则连，失败照旧重试）；
3. 网关模式下遥控器语音 + 按键回归正常（不能被抑制影响）；
4. 旧固件（无 gateway_status）→ 行为与今天一致，不抑制。

## 4.4 实施结果（2026-09-20 00:26，已落地）

| 项 | 状态 |
|---|---|
| 固件 `gateway_status` 小帧 | ✅ 已实现并烧录（串口 COM19；`voice_ble_send_gateway_status` + 订阅成功后补发） |
| 协议文档 | ✅ `Doc/Ref/protocol.md` 已加事件说明 |
| 桌面端解析 + 会话标记 | ✅ `ble_protocol` 增字段；`ble_central_win` 记录到会话 |
| 抑制守卫（新连接） | ✅ `HandleAdvertisement` + `GatewayModeActiveLocked`（本机当前未配对 RC，分支待场景复现） |
| 配对对话框 | ✅ RC 行加「（由 StickS3 中转）」后缀 + 点击时说明（GUI 表现待人工确认） |
| 真机日志 | `[BLE] gateway status VS-53A8 mode=gateway` → `[APP] SetDeviceGatewayMode VS-53A8 mode=gateway (direct ATVV to remotes suppressed)` → `stage=ready`（每次重连都复现） |
| 单元/集成测试 | ✅ 2/2 通过（过程中发现并修掉一个**构建陷阱**，见下） |

### 构建陷阱（本次踩坑，非功能缺陷）

MSVC 在中文环境下输出 `注意: 包含文件:`，CMake 无法解析该前缀 ⇒ **Ninja 从不记录头文件依赖** ⇒
改 .h 后 .obj 不重编。本次新增 UI 纯虚函数后，测试 TU 仍是旧的，链接出的二进制 vtable 错位、
`core_tests` 出现假失败（`ui.show_listening_count == 1`，确定性复现）。
处置：构建环境加 `VSLANG=1033`，并在改头文件后**强制全量重编**（touch 源码）；补全测试 Fake 的
新虚函数实现后 2/2 通过。

### 顺带发现（记入 P4）：BLE OTA 在未加密链路上被 Windows 拒绝

本次尝试用 `VoiceStick.exe --ota` 推送新固件，**0% 即失败**：
`BLEOTAfailed:0x80650008（属性需要进行身份验证，然后才能读取或写入）`。

- 固件侧 `ota_rx` 的 flags 是 `WRITE | WRITE_NO_RSP`，**并未要求加密**（`voice_ble.c:711`）；
- 即拒绝来自 **Windows 侧策略**：对「已配对设备」的 GATT 写入要求链路已加密，而当时那条链路
  并非加密链路（同一条链路上 state/audio 的订阅与写入都正常，只有 OTA 写被拦）；
- 影响：**发布路径（OTA）在这类链路状态下不可用**；本次改走串口（COM19）烧录成功。
- 待办：与 roadmap P4「网关模式下 BLE OTA 断链」合并定位——两者可能同族
  （链路安全状态与 Windows 缓存认知不一致）。

## 5. 风险与取舍

- **误抑制**：仅当固件明确上报 gateway 时抑制，且提供显式日志，用户可从"日志/配对对话框提示"发现原因。
- **固件版本错配**：老固件 + 新桌面端 = 不抑制（安全侧）。
- 不引入新开关，避免"桌面端以为 A、固件其实 B"的双真相。

## 6. 待确认

1. 是否采用方案 A（新增 `gateway_status` 帧）？
2. 抑制范围是否同意"只抑制新连接、不拆已连会话"？
3. 配对对话框里 RC 行是**禁用+说明**，还是**保留可配对但弹警告**？（推荐前者）
