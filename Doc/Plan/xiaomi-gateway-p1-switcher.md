# P1 网关切换器（设计，待确认）

> 日期：2026-09-19　上游：`Doc/Plan/xiaomi-remote-stick-gateway.md` §5.1/§5.5/§7/§8、
> `Doc/Plan/xiaomi-gateway-followup-roadmap.md` P1
> 状态：**待用户确认后实施**（brainstorming 门禁；roadmap 已定交付物，本文细化机制与交互）
> 前置：抑制直连 ATVV 的设计 `Doc/Plan/xiaomi-gateway-direct-atvv-suppression.md`（同一枚 `gateway_status` 帧）

## 1. 目标与交付物（承 roadmap）

StickS3 作为唯一枢纽，遥控器只与它配对，由它在多个目标（Win / Mac）之间切换按键与语音去向。

1. 目标表（NVS 持久化 {名称, 主机地址, 类型}，上限 4）
2. `gateway_switcher` 纯逻辑状态机（host 单测，沿用 `firmware/components/gateway/test/` 的 TDD 纪律）
3. 侧键切换器（短按预览当前目标 / 窗口内再短按轮流切换）+ 屏幕目标名显示
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

### 3.4 侧键切换器（2026-09-20 定稿，取代原「编码器长按菜单」）

**为什么改**：编码器按钮本身就是录音触发，长按会拉起语音识别，与切换冲突；用户反馈后把切换
移到**侧键**，编码器恢复纯"旋转 + 录音"。交互对齐多设备鼠标的"切换键"逻辑：先显示、再按才切。

- **第 1 次短按**：屏幕（设备号下方网关调试行）显示当前目标名，前缀 `> `（如 `> PROART16`），
  进入 **3s 预览窗**。
- **3s 内第 2 次短按**：轮流切换到目标表中的**下一个目标**（最多 4 个，到头回绕），
  走 `gateway_select_target(index, false)`（断当前 → 重广播 → 新目标重连）。
- **3s 内未再按**：关闭预览，恢复常规目标名显示，不切换。
- **录音进行中**：侧键保留取消语义（转发 `button_click secondary` 给桌面端），不触发切换。
- **普通模式**：侧键行为完全不变（单击取消/退出体感、双击恢复上次输入）。
- **编码器**：恢复原功能——旋转 = `encoder_rotate`、按钮 = 录音触发（与主键一致）。

实现：固件本地（`main.c` `side_switch_show_current/cycle` + `handle_side_up` 分支 + 
`gateway_switcher_timer_cb` 里的 3s 预览超时）；桌面端无改动。目标表、切换动作、屏幕网关行全部复用。

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
| 5 | 侧键切换器 + 屏幕状态显示 | 真机手操切换 ≤2s、来回 ≥10 次 |
| 6 | 文档/经验沉淀 + roadmap 状态更新 | Hub + Expe 同步 |

### 4.1 实施状态（2026-09-20 夜）

| 步 | 状态 | 证据 / 缺口 |
|---|---|---|
| 1 | ✅ 完成 | `gateway_status` 小帧 + 桌面端抑制直连 ATVV（提交 `fd18b22c`，真机日志 `gateway status VS-53A8 mode=gateway`） |
| 2 | ✅ 完成 | 目标表 + `gateway_target_info`（提交 `6840449f`）。真机：`gw_targets: loaded 1 target(s)` → `网关目标 #0: 未命名-FA44` → `target named: PROART16`。**踩坑**：`gateway_status` 帧先于会话 ready 到达，最初在 `SetDeviceGatewayMode` 里发主机名被静默丢弃，现改到 `on_connection_change`（只发布 ready 会话）里发 |
| 3 | ✅ 完成 | `gateway_switcher` 纯逻辑 + 8 组 host 单测（`test_gateway_switcher: ALL PASS`） |
| 4 | ✅ 代码完成，**缺真机验收** | 连接过滤/切换动作已接 NimBLE（`gateway_on_peer` → 状态机 → 动作执行：断开当前/拒非目标/屏幕提示）。真机已验证 IDLE 分支（`切换器动作: accept_peer (state=idle)`）。**缺口**：非目标拒绝、切换来回需要第二台 central（nRF Connect 模拟），今晚未做 |
| 5 | ⚠️ 代码完成，**缺手操验收** | 侧键切换器（短按预览 / 3s 内再按轮流切换 / 录音中转发取消）；编码器已恢复纯"旋转+录音"。原 LVGL 覆盖层菜单已移除（`gateway_menu` 调试命令同步删除） | 8s 自动关 / 录制中不打扰。**缺口**：编码器是物理 I2C 旋钮，无人手操作无法验证；菜单渲染、长按判定、确认路径均未真机走通 |
| 6 | ✅ 完成 | 本文 + `Doc/Ref/protocol.md`（`gateway_target_info`）+ roadmap 状态 |

真机（无手操）已验证的链路：开机 → `boot 模式应用：网关模式` → `gw_targets: loaded 1 target(s)`
→ `网关目标 #0: PROART16`（名字已持久化）→ `切换器动作: accept_peer (state=idle)` → `target named: PROART16`；
无 crash/assert。

**下一步验收清单（需人手 / 第二台机器）**：

1. 侧键短按：屏幕显示 `> <当前目标名>`（3s 预览）；3s 内再短按 → 轮流切换到下一个目标；3s 未按 → 关闭预览不切换。
2. 录音中短按侧键：应转发取消语义（桌面端取消活跃会话），不触发预览/切换。
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
### 4.4 来回切换 soak（2026-09-20 11:33–11:35，`clear`→`self` 成对触发）

因为只有一个目标身份，重复 `self` 会被「已连同一目标」短路（设计如此，实测第 3–10 次确实无链路动作），
所以用 `clear`（回到不限制）→ `self`（选定目标）成对触发，每次 `self` 都走完整切换路径。

| 轮 | 发起→accept | 轮 | 发起→accept |
|---|---|---|---|
| 1 | 490ms | 6 | 691ms |
| 2 | 672ms | 7 | 697ms |
| 3 | 893ms | 8 | 693ms |
| 4 | 654ms | 9 | 643ms |
| 5 | 774ms | — | — |

**9 次连续切换全部成功**：avg **690ms**，worst **893ms**（预算 ≤2s，余量 2.2×）。

目标机 app 侧（每轮断连→`stage=ready`）：**0.92 / 2.56 / 2.54 / 2.53 / 2.54s**（修复前同口径 14–26s），
全程 **零 `connect failed`**（修复前每轮 1–4 次僵尸重试）。

小米 central 链路：9 轮切换期间 `gw_hid`/`gw_atvv` **零事件**（日志里这些行全部集中在开机 7.5s 内），
即切换只动目标侧 peripheral 链路，未牵连小米链路 —— 验收项「切换期间小米链路零断开」达成。

订阅时 MTU 截断告警：**0 次**（修复前每次订阅必现）。

### 4.5 验收 harness（可重复执行）
```powershell
# 自动化部分（桌面端驱动切换 + 三项判据）
python scripts/e2e_test/gateway_switch_acceptance.py --rounds 5 --skip-manual

# 全量（含侧键切换器手操提示，需要人手按侧键）
python scripts/e2e_test/gateway_switch_acceptance.py --port COM19 --address 70:04:1D:DC:53:AA
```

脚本自己采集串口（显式保持 DTR/RTS 低，不扰动设备）、解析 app 日志，逐项给 PASS/FAIL：
设备侧切换 ≤2s、切换期间小米链路零断开、订阅时 MTU 截断告警为 0、目标机恢复时间（记录值），
以及人手阶段的「侧键短按预览 / 3s 内再短按切换」判定依据（设备日志里的
`侧键预览目标` / `侧键切换` 与后续 `accept_peer`）。2026-09-20 冒烟：2 轮 avg 688ms、worst 722ms，
小米链路事件 0、截断告警 0、app 恢复 2.52s / 0.82s。

### 4.6 交互变更记录（2026-09-20 晚：编码器菜单 → 侧键切换器）

编码器长按唤起菜单的方案已**废弃**（长按会拉起语音识别，与录音冲突）。改为侧键切换器（§3.4）。
原 `gateway_menu` 调试命令与 LVGL 覆盖层菜单一并移除；切换路径（断当前 → 重广播 → accept_peer）
逻辑不变，下段 12:00 的验证日志仍能证明该切换路径真实可用。

真机结果（串口日志）：

```
I (6535)  gateway_menu action=open
I (6542)  目标菜单打开 count=1 index=0          ← 覆盖层创建成功，无 crash/assert
I (14546) 目标菜单关闭                          ← 8s 无操作自动关闭（6542→14546 = 8.0s，时序正确）
I (15785) gateway_menu action=confirm
I (15785) 切换器动作: disconnect_current / start_adv / ui_switching
I (15788) 目标菜单确认 index=0
I (16432) 切换器动作: accept_peer (state=connected)   ← 确认 → 完整切换，647ms
```

**仍未验收（侧键切换器）**：①侧键短按预览 / 再按切换的**手操**、②**屏幕视觉效果**（我无法拍照）、
③非目标拒绝（需第二台机器/第二个适配器提供不同 identity address）、④多**目标**轮换（此前 9 轮是同目标来回）。

**第三方 BLE 探针为何连不上（对后续 E2E 很重要）**：app 退出后，Windows 会用系统级 HOGP 配对
自动把设备连走（设备随即停止广播），所以 `bleak` 之类的第三方 central 既扫不到也连不上；
要驱动设备只能走 app（或先解除系统配对——**绝不可做**，会弄死 HOGP 直通）。
### 4.7 非目标拒绝验收步骤（需第二台机器）

**目标**：选定目标 A 后，第二台机器 B 连接应被设备**立即断开**（`reject_peer`）。

**准备**：机器 A = 目标（现 PROART16），机器 B = 第二台（手机/笔记本都行）；B 装 nRF Connect
（最省事，避免 VoiceStick 的重连循环）。Stick 在网关模式。

**步骤**：
1. A 上确认已连且网关模式（app 日志 `stage=ready` + `mode=gateway`）。
2. A 上执行 `VoiceStick.exe --gateway-target self` 把 A 设为目标（设备进入「只允许 A」）。
3. A 上开始采集串口（`vs_serial_listen.py COM19`，保持 DTR/RTS 低）。
4. **A 关闭蓝牙**（设置里关，或飞行模式/关机）——设备随即断连、重新广播、等 A 回来；
   关 A 蓝牙是为了**不让 A 的 OS-HID 立刻抢连**，否则设备不会处于「广播等目标」态。
5. B 上扫描 → 找到 `VS-53A8`（地址 `70:04:1D:DC:53:AA`）→ 连接。
6. **预期**：B 一连上就被设备主动断开；nRF Connect 显示连接后立刻断开。

**证据（设备串口）**：
```
W (...) voice_stick: 切换器动作: reject_peer (state=switching)
W (...) voice_stick: 非目标桌面端连接，主动断开
```

**恢复**：A 重新开蓝牙，VoiceStick 重连（A 是目标，被接受）。**绝不在 A 解除系统配对**（红线）。

**若 B 扫不到设备**：说明设备没在广播——多半是第 4 步没生效（A 的 OS-HID 抢连了），确认 A 蓝牙真的关了
再试；或用 B 直接按地址连接（nRF Connect 支持直接连接已知地址）。

## 5. 风险

| 风险 | 缓解 |
|---|---|
| 目标身份不稳（Windows RPA 轮换） | 用 bond 的 identity address；真机验证重启/重连后仍命中同一目标 |
| 非目标 PC 抢连接 | 连接过滤 + 目标超时回退；v1 不做 standby 帧，先看真机日志 |
| 切换期间小米链路被牵连 | 切换代码只碰目标侧；真机验收明确要求"小米链路零断开" |
| 侧键切换器误触（预览→切换两步） | 3s 预览窗 + 录音中转发取消，避免误切换 |
| 只有一台 PC，第二目标无法真机 | 第二目标可用第二台机器，或 `nRF Connect` 模拟 central（验证过滤与拒绝路径） |
| 侧键切换与语音会话并发 | 录音中侧键保留取消语义，不触发切换 |

## 6. 待用户定案

1. **交互（已定案）**：侧键短按预览当前目标、3s 内再短按轮流切换（对齐多设备鼠标切换键逻辑）。
2. **并发（已定案）**：录音中侧键保留取消语义；空闲时才做切换器。
3. **O2 机制**：广播+过滤（推荐）先做，还是直接上定向广播？
4. **O3 非目标提示**：v1 是否需要给非目标 PC 发"待机"帧，还是先只靠日志（推荐先不做）？
5. **O6 第二目标**：你有第二台可当目标的机器吗？没有的话我按"nRF Connect 模拟"验证路径设计验收。
6. **命名**：目标名用桌面端主机名（`GetComputerName`）可以吗？还是允许在设备上重命名？
