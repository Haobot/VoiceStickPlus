# P1 网关切换器（设计，待确认）

> 日期：2026-09-19　上游：`Doc/Plan/xiaomi-remote-stick-gateway.md` §5.1/§5.5/§7/§8、
> `Doc/Plan/xiaomi-gateway-followup-roadmap.md` P1
> 状态：**待用户确认后实施**（brainstorming 门禁；roadmap 已定交付物，本文细化机制与交互）
> 前置：抑制直连 ATVV 的设计 `Doc/Plan/xiaomi-gateway-direct-atvv-suppression.md`（同一枚 `gateway_status` 帧）

## 1. 目标与交付物（承 roadmap）

StickS3 作为唯一枢纽，遥控器只与它配对，由它在多个目标（Win / Mac）之间切换按键与语音去向。

1. 目标表（NVS 持久化 {名称, 主机地址, 类型}，上限 4）
2. `gateway_switcher` 纯逻辑状态机（host 单测，沿用 `firmware/components/gateway/test/` 的 TDD 纪律）
3. LVGL 菜单 + 编码器交互（唤起 / 选择 / 确认）
4. 屏幕状态显示（"已连接：<目标>"）

**验收**：切换 ≤2s；切换后新目标语音与按键均可用；切换期间小米链路零断开；多目标来回 ≥10 次稳定。

## 2. 关键事实（2026-09-19 核实，避免重复踩）

| 事实 | 出处 |
|---|---|
| 网关模式状态机已存在（NVS 持久化，当前入口=开机窗口按住主键翻转） | `gateway_mode.c/.h` |
| 目标侧是 **peripheral**：桌面端为 central 主动连过来 | `voice_ble.c` `s_connected` / `start_advertising()` |
| 断连后看门狗自动重开广播（`start_advertising` 在两处 fail 路径调用）→ 切换**不需要新造广播机制** | `voice_ble.c:780,806,1003` |
| NimBLE 可枚举 bond（`ble_store_iterate` / `ble_store_util_count`），当前固件**未使用** | grep 无命中 |
| `device_info` 已到长度预算，新能力必须走独立小帧 | `voice_ble.c:1374` |
| 编码器：main.c 轮询 → `APP_EVENT_ENCODER_ROTATE` + 按键电平；`ui_status` 只有状态画面，**没有菜单框架** | `main.c:800,2338`；`components/ui_status` |
| 小米侧（central）与目标侧（peripheral）互不干扰，切换只动目标侧 ACL | 方案 §5.5 |

## 3. 设计

### 3.1 目标身份与目标表

- **目标 = 一台与本机 bond 的桌面端**（PC 侧是 central）。
- **稳定标识**：bond 记录的**对端 identity address**（+ 地址类型）。Windows 用 RPA 时地址会轮换，
  但 identity address 在 bond 生命周期内稳定 —— 用它做表主键。
- **昵称**无法从 BLE 得到 → 桌面端连接后上报一次
  `{"event":"gateway_target_info","name":"<hostname>"}`（新增小命令，桌面端取 `GetComputerName`）。
  固件收到后更新该目标的名字并持久化。
- **NVS 表** `gw_targets`：最多 4 条 `{id_addr[6], addr_type, name[24], last_seen_s}`。
  维护方式：bond 建立事件插入（名字未到前显示"未命名(地址后4位)"）；bond 删除事件移除。
- **桌面端侧**：app 需要知道"我是当前活跃目标吗"——复用 `gateway_status` 帧扩展到
  `{"event":"gateway_status","mode":"gateway","active":true|false,"target":"<name>"}`。
  非活跃目标收到后进入"待机"（不重连、UI 提示"由其他目标占用"），避免两台 PC 抢连接。

### 3.2 `gateway_switcher` 纯逻辑状态机（host 单测）

```
IDLE ──选定目标──► SWITCHING(from?,to) ──连接建立(target)──► CONNECTED(target)
  ▲                      │ 超时(5s)                              │
  └──────────────────────┴────────────── 回退上一目标 / 报错 ◄────┘
```

- 输入：目标列表快照、当前连接（peer/无）、用户选择、超时事件、bond 增删。
- 输出动作（枚举，由 NimBLE 薄壳执行）：`kDisconnectCurrent`、`kStartAdvAccepting(peer)`、
  `kSetUiState(switching|connected|error)`、`kSendGatewayStatus(active)`、`kPersistTargets`。
- 无 ESP-IDF 依赖 → host 单测覆盖：正常切换、目标不存在、超时回退、切换中再次选择、
  非目标被拒、bond 删除当前目标、列表满（>4）等。

### 3.3 连接过滤与切换机制（关键）

1. 用户确认切换 → `kDisconnectCurrent`（当前目标凭心跳感知断连；app 已有主动重连队列）→
   进入 `SWITCHING`，**只接受选中目标**的连接。
2. `BLE_GAP_EVENT_CONNECT` 里比对对端 identity address：
   - 命中目标 → `CONNECTED`，发 `gateway_status(active=true)`；
   - 非目标 → `ble_gap_terminate` 并计数（日志一行；**v1 不单独发 standby 帧**，靠 app 侧重连退避）。
3. 超时 5s 未连上 → 回退到上一目标并把结果打到屏幕（"切换失败：<目标> 未响应"）。
4. 小米链路（central）全程不断：切换期间不触碰 `gateway_hid_host` / `xiaomi_atvv_client`。

**开放点 O2**：广播+过滤（推荐，简单可回退） vs **定向广播**到目标地址（更快，但 RPA 下需要
controller 解析支持，风险高）。建议先做前者，真机量到切换时延 >2s 再考虑后者。

### 3.4 LVGL 菜单与编码器交互

- **唤起**：长按编码器按钮 ≥600ms（不占用现有旋转/点击语义）；录制中不允许唤起（防误操作）。
- **导航**：旋转移动高亮；短按确认；长按 / 8s 无操作退出。
- **列表项**：`<目标名>`（当前目标打勾）、末项"删除目标"（二次确认后 `ble_store_util_delete_peer`）。
- **切换中**：显示"正在切换 → <目标>"，成功/失败各有终态提示。
- 列表满（4）时新增目标提示"请先删除一个"。

**开放点 O4**：唤起方式（长按 600ms）是否符合直觉？替代：双击编码器按钮、或专用组合（主键+编码器）。

### 3.5 屏幕状态显示

- 常驻区域新增"目标：<name>"（`ui_status` 增加一个字段/画面），15s 无操作后回默认画面；
  未连接时显示"目标：<name>（未连接）"。

## 4. 分步实施顺序（每步可独立验收）

| 步 | 内容 | 验收 |
|---|---|---|
| 1 | `gateway_status` 小帧（固件+协议）+ 桌面端抑制直连 ATVV | 见抑制设计 §4.3 |
| 2 | 目标表 + `gateway_target_info`（固件+桌面端上报主机名） | 单测 + 日志可见目标入表/命名 |
| 3 | `gateway_switcher` 纯逻辑 + host 单测 | 单测全绿（无 UI） |
| 4 | 连接过滤 + 切换动作接 NimBLE（日志驱动，无菜单） | 用 `nRF Connect` 模拟第二目标验证"非目标被拒/目标可连" |
| 5 | LVGL 菜单 + 屏幕状态显示 | 真机手操切换 ≤2s、来回 ≥10 次 |
| 6 | 文档/经验沉淀 + roadmap 状态更新 | Hub + Expe 同步 |

### 4.1 实施状态（2026-09-20 夜）

| 步 | 状态 | 证据 / 缺口 |
|---|---|---|
| 1 | ✅ 完成 | `gateway_status` 小帧 + 桌面端抑制直连 ATVV（提交 `fd18b22c`，真机日志 `gateway status VS-53A8 mode=gateway`） |
| 2 | ✅ 完成 | 目标表 + `gateway_target_info`（提交 `6840449f`）。真机：`gw_targets: loaded 1 target(s)` → `网关目标 #0: 未命名-FA44` → `target named: PROART16`。**踩坑**：`gateway_status` 帧先于会话 ready 到达，最初在 `SetDeviceGatewayMode` 里发主机名被静默丢弃，现改到 `on_connection_change`（只发布 ready 会话）里发 |
| 3 | ✅ 完成 | `gateway_switcher` 纯逻辑 + 8 组 host 单测（`test_gateway_switcher: ALL PASS`） |
| 4 | ✅ 代码完成，**缺真机验收** | 连接过滤/切换动作已接 NimBLE（`gateway_on_peer` → 状态机 → 动作执行：断开当前/拒非目标/屏幕提示）。真机已验证 IDLE 分支（`切换器动作: accept_peer (state=idle)`）。**缺口**：非目标拒绝、切换来回需要第二台 central（nRF Connect 模拟），今晚未做 |
| 5 | ⚠️ 代码完成，**缺手操验收** | LVGL 覆盖层菜单（`ui_status_menu_show/set_selection/hide`）+ 编码器长按 ≥600ms 唤起 / 旋转选 / 短按确认 / 8s 自动关 / 录制中不打扰。**缺口**：编码器是物理 I2C 旋钮，无人手操作无法验证；菜单渲染、长按判定、确认路径均未真机走通 |
| 6 | ✅ 完成 | 本文 + `Doc/Ref/protocol.md`（`gateway_target_info`）+ roadmap 状态 |

真机（无手操）已验证的链路：开机 → `boot 模式应用：网关模式` → `gw_targets: loaded 1 target(s)`
→ `网关目标 #0: PROART16`（名字已持久化）→ `切换器动作: accept_peer (state=idle)` → `target named: PROART16`；
无 crash/assert。

**下一步验收清单（需人手 / 第二台机器）**：

1. 编码器长按 600ms：屏幕出现目标菜单（PROART16 高亮）；旋转移动高亮；短按确认 → 屏幕显示目标名；8s 无操作自动关。
2. 录音中长按：不应弹菜单；若这次按压本身已启动录音（`encoder_press_action=recording`），长按应立刻结束该录音并打开菜单（<600ms 会话被桌面端最短时长门控丢弃）。
3. 第二台 central（nRF Connect 或另一台 PC）：选定目标后，非目标连接应被立刻断开（日志 `非目标桌面端连接，主动断开` + `reject_peer`）。
4. 切换来回 ≥10 次，切换 ≤2s，且**小米链路零断开**（`gw_hid`/`gw_atvv` 无断开日志）。

### 4.2 切换路径真机实测（2026-09-20 10:4x，桌面端驱动）

设备菜单要手操，而第三方探针连不上（见下），所以补了**桌面端入口** `VoiceStick.exe --gateway-target self|clear`
（复用 `--ota` 的 WM_COPYDATA 转发；固件 `gateway_select_target` 命令新增 `self` 变体，以「当前连接对端」
为目标，桌面端无需知道表下标），用它驱动真机切换并量时延。

| 轮次 | 发起 → 断开 | 断开 → 重新广播 | 广播 → 目标重连 | **发起 → accept_peer** |
|---|---|---|---|---|
| 1（10:46） | 250ms | 6ms | 411ms | **675ms** |
| 2（10:49） | 33ms | 7ms | 398ms | **447ms** |

- ✅ **设备侧切换 ≤2s 达标**（两次 675ms / 447ms，且 `accept_peer (state=connected)` 确认选定目标被接受）。
- ✅ **切换期间小米 central 链路零断开**：切换后 `gw_hid`/`gw_atvv` 无任何断开/重连行（会话状态停在 `1 -> 2`）。
- ⚠️ **发现（后续项）：目标机 app 恢复远慢于设备侧**。目标 PC 的恢复时间呈两极：首次尝试就成功时 1.5–2.0s，
  但多数轮次会先失败 1–4 次（`state subscribe timeout after 2500ms`、`audio_tx discovery failed: AccessDenied`、
  `notification subscriptions reported success but the device never registered them`），
  实测两次分别约 **26s** 和 **14s** 才回到 `stage=ready`。这正对应设计稿「切换后目标侧依赖 P0」的预判，
  但用户可感知的可用性是这 10–30s，而不是设备侧的 0.5s。

  **下一步定位建议**：断开瞬间 Windows 的系统级 HOGP 配对会抢先自动重连（实测设备侧 `connected handle=1 … since_adv=411ms`，
  而 app 自己的连接请求随后撞上这条链路，报 `AccessDenied`）——即「OS 抢链路 vs app 重连」竞态。
  可能的缓解：网关模式下设备主动断链后，app 侧延长 settle（现 1500ms）或等链路空闲再发起；
  或复用 P0 的 CCCD 缓存击穿 + 免退避重试节奏。**未实施，仅记录**。


### 4.3 目标机恢复慢：根因定位与修复（2026-09-20 11:2x）

§4.2 那个「目标机 app 要 14–26s 才恢复」不是重连节奏问题，而是**固件侧的帧序问题**：

**根因**：`BLE_GAP_EVENT_SUBSCRIBE`（state_tx 订阅成功）时，固件立刻推送初始状态帧串
（device_info 235B + encoder_status + gateway_status），但此刻**我们自己发起的 MTU 交换还没完成**，
`att_mtu` 仍是 23 ⇒ 单帧通知预算只有 20B ⇒ 235B 的 device_info 被截断成 ~16B。
桌面端解析不出任何合法状态帧，其「订阅存活证明」（2.5s）超时 ⇒ 判为僵尸会话 ⇒ zombie-suspect
免退避重试风暴 ⇒ 实测 14–26s 才恢复。设备日志现场：

```
I (28642) voice_ble: pairing complete conn=1 status=0
I (28642) voice_ble: subscribe state desync: ... resyncing
W (28643) voice_ble: state json 235B + 4B header exceeds notify budget (att_mtu=23), peer will truncate
I (28673) NimBLE: GATT procedure initiated: exchange mtu      ← MTU 交换排在推送之后
```

**修复**（`firmware/components/voice_ble/voice_ble.c`）：订阅时若 `ble_att_mtu(conn) <= 23`，把初始帧串
挂起，等 `BLE_GAP_EVENT_MTU` 到达后再推（实测只晚 33–48ms）；另加 1.2s 兜底定时器，防对端不响应
MTU 交换时静默不发（此时按旧行为发送，不劣于修复前）。断连时清除挂起标志。

**修复后实测**（同一条 `--gateway-target self` 路径）：

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 设备侧 发起→accept_peer | 447 / 675ms | **642ms** |
| 目标机 app 断开→`stage=ready` | 14s / 26s | **2.53s**（首次尝试即成功，无 connect failed） |
| 订阅时 MTU 截断告警 | 每次必现 `exceeds notify budget (att_mtu=23)` | **零**（`state burst flushed after MTU exchange (att_mtu=247)`） |

这条同时清掉了 P4 里的「device_info 首帧被截断」技术债——两者是同一个 bug。

**第三方 BLE 探针为何连不上（对后续 E2E 很重要）**：app 退出后，Windows 会用系统级 HOGP 配对
自动把设备连走（设备随即停止广播），所以 `bleak` 之类的第三方 central 既扫不到也连不上；
要驱动设备只能走 app（或先解除系统配对——**绝不可做**，会弄死 HOGP 直通）。

## 5. 风险

| 风险 | 缓解 |
|---|---|
| 目标身份不稳（Windows RPA 轮换） | 用 bond 的 identity address；真机验证重启/重连后仍命中同一目标 |
| 非目标 PC 抢连接 | 连接过滤 + 目标超时回退；v1 不做 standby 帧，先看真机日志 |
| 切换期间小米链路被牵连 | 切换代码只碰目标侧；真机验收明确要求"小米链路零断开" |
| LVGL 菜单是全新 UI 层（无框架可抄） | 菜单先做最小可用（列表+高亮+确认），不进设置树 |
| 只有一台 PC，第二目标无法真机 | 第二目标可用第二台机器，或 `nRF Connect` 模拟 central（验证过滤与拒绝路径） |
| 菜单与语音会话并发 | 录制中禁止唤起菜单（O5） |

## 6. 待用户定案

1. **O4 交互**：长按编码器 ≥600ms 唤起菜单（推荐）？还是双击/组合键？
2. **O5 并发**：录制中禁止唤起菜单（推荐）？
3. **O2 机制**：广播+过滤（推荐）先做，还是直接上定向广播？
4. **O3 非目标提示**：v1 是否需要给非目标 PC 发"待机"帧，还是先只靠日志（推荐先不做）？
5. **O6 第二目标**：你有第二台可当目标的机器吗？没有的话我按"nRF Connect 模拟"验证路径设计验收。
6. **命名**：目标名用桌面端主机名（`GetComputerName`）可以吗？还是允许在设备上重命名？
