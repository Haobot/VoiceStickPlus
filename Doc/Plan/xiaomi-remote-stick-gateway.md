# 小米遥控器 StickS3 网关（类 KVM 中转）方案

> 状态：**Phase 0 spike 已验证通过（2026-09-16，六项全过，风险 1 解除），待排期 Phase 1**。2026-09-16 用户提出中转设想，经需求梳理与方案对比后定案四项范围决策：方案 A（固件全归一化）/ 本期不做移动端 / 双模式切换 / 屏幕菜单+编码器切换。spike 结论见 §6.1。
> 事实链依据：`Doc/Plan/xiaomi-remote-usage-tap.md`（三键问题现状与 13 键 usage 表）、`Doc/Plan/xiaomi-remote-2-pro-support.md`（ATVV 接入现状）、`Doc/Ref/protocol.md`（现有上行协议）。

## 1. 背景与目标

小米遥控器 2 Pro（RC-XXXX）当前直连桌面端（WinRT BLE central），三键（back/volume_up/volume_down）因键盘页非标准 usage 被微软 kbdhid 翻译层丢弃，刚以 usage tap 方案（`feat/xiaomi-usage-tap`，自研注入 WUDFHost）解决——该方案侵入性高（注入系统进程、需提权、Windows 更新脆弱），且真机冒烟验收尚未执行。同时用户拥有 Mac + Windows 双机，小米遥控器为 BLE 单连接设备，跨机使用需重新配对，切换体验破碎。

用户新设想：**StickS3 升级为桌面蓝牙网关**——小米遥控器只与 StickS3 配对一次，StickS3 以 dual-role 同时作为小米的 central 与目标设备的 peripheral，实现按键与语音音频的中转和多目标切换。

### 1.1 需求清单

| # | 需求 | 说明 |
|---|------|------|
| R1 | 三键识别源头解决 | 返回/音量+/音量- 在固件解析层翻译为跨平台可识别形式，不再依赖各平台桌面端 hack |
| R2 | 一控多机 | 小米遥控器只配对 StickS3 一次，bond 稳定，不再有"被 HID 连上停广播"的单机扫描盲区问题 |
| R3 | 快速切换 | Mac/Windows 双目标间切换当前输出源（StickS3 屏幕菜单+编码器触发） |
| R4 | 音频转发 | 小米 ATVV 语音音频经 StickS3 中转到当前目标设备，端到端时延增量可接受（≤50ms 量级） |

### 1.2 非目标（本期不做）

- 平板/手机作为目标设备（纯 HID 模式与 iOS/Android App 均推迟；ESP32-S3 无 BR/EDR、无 LE Audio ISO，移动端语音无标准协议可用，是硬边界而非取舍）。
- 桌面端结构性改动（目标：macOS/Windows 桌面端**零结构性改动**，允许的例外见 §5.7）。
- 多目标同时激活（网关模式单活跃目标）。
- usage tap 存量代码的移除（网关成熟后再定退役，见 §9.3）。

## 2. 技术事实核查结论（2026-09-16 实测于仓库）

| 事实 | 结论 | 对方案的影响 |
|---|---|---|
| 固件 BLE 栈 | NimBLE，`ROLE_CENTRAL/PERIPHERAL` 均已开启，`MAX_CONNECTIONS=1`、`MAX_BONDS=3` | 多角色网关无需换栈，仅需调参 |
| 内存 | SPIRAM 8MB（octal）已启用 | 双 ACL + GATT client/server 余量充足 |
| ATVV 桌面端实现体量 | `xiaomi_atvv_protocol.cc`(55 行) + `xiaomi_atvv_session.cc`(424 行) + `ima_adpcm_decoder`(独立小文件) | 固件 C 移植体量可控（约 500 行） |
| 小米按键报文 | 9 字节 = `01 00 00` 前缀 + 3×LE16 usage（当前按下集合，消费端 diff 出沿）；13 键 usage 表见 usage-tap 文档 §1 | 固件解析层有完整事实输入 |
| ESP32-S3 蓝牙能力 | 仅 BLE，无经典蓝牙（BR/EDR），无 LE Audio ISO 通道 | 移动端语音输入无标准协议路径，必须配套 App（故推迟） |

## 3. 方案对比（2026-09-16 用户已定案）

| | A. 固件全归一化（**已选**） | B. ATVV 隧道透传 | C. 维持现状多机配对 |
|---|---|---|---|
| 架构 | 小米按键/音频在固件解析：按键经标准 HID 直通+语音键截留（§5.3），音频 ADPCM→PCM→Opus 复用 `audio_tx` | 固件透传小米原始 ATVV 流，桌面端新增私有隧道特征+解码 | 每台设备各自配对小米 |
| 桌面端改动 | 零结构性改动 | macOS+Windows 均需加隧道层 | 无 |
| 固件工作量 | 大（ATVV client + ADPCM 解码 + Opus 复用 + 切换器） | 中（只搬运字节） | 无 |
| CPU 增量 | 解码+重编码（双核可承受，与现有 mic Opus 编码同量级） | 几乎零 | — |
| 跨平台性 | 标准 HID + 现有私有协议，未来移动端仅按键可用 | 仅装有桌面端的平台 | 仅单机 |

选 A 的核心理由：**三端改动收敛为一端**。桌面端现有 keymap/ASR/微信模式全链路原样工作，协议不动，风险集中在固件（可通过 Phase 0 spike 提前证伪）。

## 4. 总体架构

```text
                         ┌─────────────────────────────────────────────┐
                         │            StickS3（网关模式）                │
  小米遥控器 2 Pro        │                                             │      Mac / Windows
  ┌──────────────┐ BLE   │  ┌──────────────┐      ┌─────────────────┐  │ BLE  ┌──────────────┐
  │ HID(13键)    │◄──────┼──┤ HID host     │─────►│ HOGP 外设       │◄─┼──────┤ 桌面端        │
  │ (0x2A4D 特征)│  CT   │  │ (9B 报文解析  │ 翻译 │ (标准 Consumer  │  │  CT  │ (central，   │
  │ ATVV 语音    │◄──────┼──┤  usage diff) │      │  usage 直通)    │  │      │  现状不变)    │
  │ (AB5E0001-…) │  CT   │  ├──────────────┤      ├─────────────────┤  │      │              │
  └──────────────┘       │  │ ATVV client  │─────►│ audio_tx        │──┼─────►│ Opus→ASR→注入 │
                         │  │ (会话+ADPCM   │ 归一 │ (Opus 编码复用) │  │      │  零结构性改动 │
                         │  │  解码→PCM)    │      │ state_tx 按键   │  │      │              │
                         │  └──────────────┘      │ 事件(语音键)     │  │      └──────────────┘
                         │  会话仲裁器 / 切换状态机 / 屏幕菜单(LVGL)     │
                         └─────────────────────────────────────────────┘
                          CT = StickS3 为 central（对小米）
                              目标侧维持现状：桌面端为 central，StickS3 为 peripheral
```

要点：
- **角色不反转**：对目标设备仍是"桌面端 central 连 StickS3 peripheral"（现有架构原样），StickS3 仅新增对小米的 central 角色，dual-role 并存。
- 小米遥控器在操作系统中"消失"（不再与任何主机直连），三键问题、扫描盲区问题、单连接限制全部在源头消解。
- usage tap（注入 WUDFHost）在此链路下自然闲置，网关成熟后可退役。

## 5. 详细设计

### 5.1 连接拓扑与生命周期

- 网关模式下 StickS3 维持两条 ACL：① 对小米（central，bond 于 NVS，主动 direct connect 回连）；② 对当前目标桌面端（peripheral，等待桌面端心跳重连）。
- `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` 1→3（两条活跃 + 切换期余量），`MAX_BONDS` 3→6（小米 + 多目标）。
- 网关模式进入/退出：LVGL 设置菜单选择，模式持久化 NVS（支持上电默认进网关）；退出时断开小米、恢复普通外设广播。普通模式全部代码路径与现状完全一致（网关模块不注册/不启动），保证零回归。

### 5.2 新增固件组件（建议组件化，各自单一职责）

| 组件 | 职责 | 依赖 |
|---|---|---|
| `xiaomi_hid_host` | GATT client：订阅 0x2A4D 特征，9 字节报文解析为 usage 集合，diff 出 pressed/released 沿 | NimBLE client |
| `xiaomi_atvv_client` | ATVV 会话状态机（bearer 建立/start/stop）+ ADPCM 帧提取，移植自桌面端 `xiaomi_atvv_session.cc`（C 化约 500 行） | NimBLE client |
| `gateway_keymap` | usage→动作翻译表：直通键映射为标准 Consumer/Keyboard usage；截留键（语音/power/tv）转内部事件 | `xiaomi_hid_host` |
| `gateway_audio` | ADPCM 解码（移植 `ima_adpcm_decoder`）→ 16k PCM → 复用现有 Opus 编码器 → `audio_tx` 帧 | `xiaomi_atvv_client`, `audio_pipeline` |
| `gateway_session_arbiter` | 语音会话仲裁：小米会话与 StickS3 自身 mic 会话互斥（单活跃音源），忙时另一源给屏幕反馈 | `gateway_audio`, 现有音频管线 |
| `gateway_switcher` | 目标表（NVS：名称+地址+类型）+ 切换状态机（断开当前→广播等待→桌面端重连确认）+ LVGL 菜单 | LVGL, NimBLE |

### 5.3 按键链路（分两期路由）

**Phase 1 路由（P2：HID 直通 + 截留）——零改动可用：**

小米 13 键分三类处理：

| 类别 | 键 | 处理 |
|---|---|---|
| 直通键 | up/down/left/right/ok/menu/home | 翻译为标准 usage（方向→Keyboard page、ok→Enter、home→Consumer Home/Win 键），经 HOGP 外设直发系统层 |
| 直通键（原三键） | back→Consumer AC Back(0x0224)、volume_up/down→Volume Increment/Decrement(0x00E9/0x00EA)、volume_mute→Mute(0x00E2) | 标准 Consumer usage，**全平台原生识别，R1 达成** |
| 截留键 | 语音键、power、tv | 不出网关：语音键→内部语音会话事件（§5.4）；power/tv→屏幕菜单/忽略（防误关机） |

**Phase 1+ 路由（P1：隧道融合，可选增强）：** 需要桌面端 keymap 全键语义时，新增按键事件源字段向后兼容扩展（协议小改，两端同步），将小米按键沿并入现有按键事件上报、进桌面端按键映射对话框。P1 不在首期验收范围。

**双击检测归位：** 小米语音键的双击时序检测从桌面端适配层移回固件（复用 StickS3 主键双击检测组件），与架构红线"双击检测在固件端"重新对齐——这是本方案对现有架构的**简化**而非违背。

### 5.4 音频链路（R4 核心）

```text
小米语音键按下(固件截留)
  → gateway_session_arbiter: 仲裁通过(自身 mic 空闲)
  → xiaomi_atvv_client: bearer 建立 + start
  → ADPCM 音频帧 → 解码 16kHz PCM → 现有 Opus 编码器 → audio_tx 上行
  → 桌面端: 与 StickS3 自身语音完全同路(ASR/微信模式零改动)
小米语音键松开 → stop → 尾帧冲刷(复用现有 trailing-syllable-drain 机制)
```

- 采样率对齐：ATVV 16kHz ADPCM 与 ES8311 mic 采集同源采样率（Phase 2 实施时按 `xiaomi_atvv_session.cc` 实际参数核对），Opus 编码器实例复用或按会话独占。
- 会话互斥规则：小米会话激活时暂停 StickS3 mic 采集（`gateway_session_arbiter` 状态机，先到先得，忙源请求→屏幕提示+短提示音）。
- 桌面端视角：小米语音键的按下/松开被固件翻译为与 StickS3 主键一致的"按住说话"会话触发沿（现有按键事件通道），协调器状态机无感知切换——**语音功能对桌面端真正零改动**。

### 5.5 切换器（R3）

- 目标表：NVS 持久化，字段 {名称, 主机地址, 类型}，上限 4；经屏幕菜单增删（扫描附近已 bond 桌面端/手输地址，Phase 3 细化 UX）。
- 切换流程：编码器按键唤起菜单→旋转选目标→确认→断开当前目标（桌面端凭心跳感知）→进入可连接广播→目标桌面端心跳重连（已有机制：直连失败入队主动重连）→屏幕显示"已连接：<目标>"。
- 切换时长预算：≤2s（BLE 连接建立 ~100ms 级 + 桌面端重连轮询间隔，实测后若超标，优化桌面端重连退避参数——属配置级微调，非结构性改动）。
- 小米侧链路在切换期间保持不断（bond 稳定），切换只影响目标侧 ACL。

### 5.6 时延预算（R4 验收口径）

| 环节 | 预算 | 说明 |
|---|---|---|
| 小米→StickS3（BLE 一跳） | ~20-50ms | 连接间隔 15ms 级 + 40ms 帧粒度 |
| 固件处理（ADPCM 解码+Opus 编码） | ~10-20ms | 双核 240MHz，与现有 mic 编码同量级 |
| StickS3→目标（BLE 一跳） | ~15-30ms | 现有 audio_tx 链路实测水平 |
| **总增量 vs 现状直连** | **~30-60ms** | 语音流程本身含 0.5-3s ASR，无感；按键两跳 ~30-60ms 同样低于感知阈值 |

### 5.7 桌面端零改动原则与例外清单

零改动（结构性）：协议帧格式、GATT 服务、协调器状态机、ASR 管线、微信模式、按键映射对话框、ATVV 客户端代码（保留给"小米直连"遗留模式）。

允许的例外（配置/枚举级）：
1. 桌面端重连退避参数调优（服务切换时长，仅 config 值）。
2. P1 阶段的按键事件源字段扩展（向后兼容，桌面端不升级则忽略）。

## 6. 风险清单与 Phase 0 spike（一票否决点）

| # | 风险 | 等级 | 验证/缓解 |
|---|---|---|---|
| 1 | 小米不接受 ESP32 发起的 pairing/bond（MITM/IO 能力不匹配） | **高（生死）** | Phase 0 spike 第 4 项 |
| 2 | 小米对非 TV 主机（ESP32）给出的 ReportMap 与 9 字节报文格式不一致 | 高 | Phase 0 spike 第 5 项（连 Mac 正常说明其自适应，但 ESP32 形态未知） |
| 3 | 双 ACL 并发吞吐/抖动不达标 | 中 | Phase 2 首项实测（ADPCM 16kbps + Opus 上行并发压力） |
| 4 | 网关常开功耗/发热 | 中 | 实测网关模式电流；形态定位为插电台站（用户已接受双模式） |
| 5 | ATVV 会话状态机 C 移植缺陷 | 中 | 纯逻辑模块 host 侧单测（§7） |

**Phase 0 spike 清单（1 天，独立验证程序，不并入正式固件）：**
1. 小米进配对模式，ESP32 NimBLE 扫描发现。
2. 发起连接 + pairing（NoInput/NoOutput Just Works 起步，失败再升 passkey）+ bond 持久化 NVS。
3. 断电重连：ESP32 主动 direct connect 回连成功。
4. 读 HID Report Map，与已知 13 键 usage 表比对。
5. 订阅 0x2A4D 通知，实测三键（back/vol+/vol-）usage 收到。
6. 发现 ATVV 服务（`AB5E0001-…`）及全部特征。

任一项失败→回到方案评审（备选：B 隧道透传同样依赖 1/2/3，风险同源；彻底失败则维持 usage tap 现状路线）。

### 6.1 Phase 0 spike 结论（2026-09-16 真机验证，六项全过 ✅）

验证程序：`firmware/spikes/xiaomi_gateway_poc/`（一次性，不并入正式固件；串口采集工具 `serial_listen.py`）。

| # | 验证项 | 结论 | 证据 |
|---|---|---|---|
| 1 | 扫描发现 | ✅ | **正常态广播名 `U-RFRC478`**（桌面端白名单的 `MI RC` 等是配对模式/已配对形态）；识别条件=名称含 `u-rfrc` 或配对模式白名单；Flags 0x06 纯 BLE 可连接，广播含 Service Data(0xFF01) |
| 2 | 连接+配对+bond | ✅ | **配对模式下 Just Works 接受**（NoInputNoOutput，LE Secure Connections），bond 持久化 NVS；非配对模式连接会在 ~30s 后 Authentication Failure（HCI 0x05）断开——预期行为，非兼容性问题 |
| 3 | 断电重启回连 | ✅ | 重启后 direct connect 对端身份地址 `c0:5d:39:xx:xx:xx`（public），4s 内连上，LTK 恢复加密成功 |
| 4 | Report Map | ✅ | 86 字节：Report ID 1 = 3×16bit usage（键盘页 0x00~0xFE，同报最多 3 键）+ 厂商页 0xFF00 三个 120 字节输入报告（Report ID 6/7/8） |
| 5 | 三键 notify | ✅ | back=`0x00F1` / volume_up=`0x0080` / volume_down=`0x0081`，与 usage-tap 文档 13 键表完全一致 |
| 6 | ATVV 服务 | ✅ | `AB5E0001` 服务 + TX(0x0302, write)/Audio(0x0304, notify)/Control(0x0307, notify) 三特征齐全，句柄形态与协议档案一致 |

**对 Phase 1/2 的设计输入（spike 附带收获）**：

1. **按键报文实为 8 字节**：`[2 字节头][3×LE16 usage 槽]`，usage 在槽 1（byte 2-3），松开帧全零；与桌面端经 Windows HID 栈看到的 9 字节（`01 00 00` 前缀）不同——固件解析器按 8 字节实现。
2. **Report notify 无需写 CCCD**：连接加密后小米默认推送（非标但稳定复现）——固件仍按标准先尝试写 CCCD，失败不视为错误。
3. **双地址形态**：广播地址（`5c:24:1f:*` 小米 OUI）≠ 配对后身份地址（`c0:5d:39:*` public）；bond 后回连一律用身份地址。
4. **连接参数**：遥控器主动请求 itvl=10(12.5ms)/latency=49/supervision=500ms；MTU 协商 247~256。
5. **对端服务全景**：GATT/GAP/电量(0x2A19，句柄 0x30 推 89%)/设备信息/HID(0x1812)/ATVV/小米私有 `8a7a0001-…`/`0x01bf`/`0xfe59`。
6. HID 服务内 0x2A4D 特征有 7 个带 notify（`0x0064` 为 Report ID 1 通道）+ 10 余个轮询形态（props=0x0a 无 CCCD）——订阅时必须按 notify 属性筛选。

**风险 1（小米 bond 兼容性）解除**，方案 A 继续。遗留观察项（Phase 2 首日验证）：ATVV 音频流吞吐与双 ACL 并发。

## 7. 测试策略（TDD 纪律）

- **纯逻辑 C 模块 host 侧单测**（固件首次引入单测目标）：`xiaomi_hid_host` 报文解码器（9 字节集合 diff）、`gateway_keymap` 翻译表、`gateway_switcher` 状态机、`gateway_session_arbiter`、ADPCM 解码器（与桌面端 C++ 实现互为金标准比对）——CMake host 目标，红-绿-重构。
- **真机验收**：每 Phase 附带验收清单（Phase 4 汇总，仿 usage-tap §7.1 格式）。
- **回归红线**：普通模式全功能回归（网关模块零注册），双模式切换往返稳定性（100 次循环）。

## 8. 分期交付计划

| Phase | 内容 | 预估 | 出口判据 |
|---|---|---|---|
| 0 | spike（§6 清单） | 1 天 | 全 6 项通过，报告归档 |
| 1 | 按键直通链路（P2）+ 双模式框架 + `xiaomi_hid_host`/`gateway_keymap` | 3-5 天 | Mac/Win 均识别三键与全直通键；普通模式零回归 |
| 2 | 语音链路（`xiaomi_atvv_client`+`gateway_audio`+仲裁器） | 5-8 天 | Mac/Win 语音端到端可用，时延增量实测 ≤80ms |
| 3 | 切换器 + 目标管理 + 菜单 UX | 3-5 天 | 双目标切换 ≤2s，往返稳定 |
| 4 | 打磨 + 全链路真机验收 + 文档收尾 | 2-3 天 | §7 验收清单全绿 |

## 9. 开放决策点（待用户审阅定案）

1. **power/tv 键默认行为**：截留（本文默认，防误关机）还是映射（如 power→屏幕菜单、tv→模式切换）？
2. **P1 隧道融合是否排期**：需要全键 keymap 时才做，默认不排。
3. **usage tap 存量定位**：网关 Phase 4 验收后退役 Windows usage tap（卸载组件保留一代），还是长期双路径共存？（建议：验收后退役，减少维护面。）

## 10. 变更记录

- 2026-09-16：初版设计稿（需求梳理、方案对比定案、架构与分期），待用户审阅。
- 2026-09-16（二）：Phase 0 spike 真机验证六项全过（§6.1），风险 1 解除；spike 程序与串口采集工具入库；正常态广播名/8 字节报文/双地址/无 CCCD 推送四项新协议事实回填。
