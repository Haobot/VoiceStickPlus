# Phase 2 语音链路 + P1 按键自定义 — AI 交接文档（2026-09-18）

> 你将接手 VoiceStick 项目 `feat/stick-gateway` 分支的网关 Phase 2 工作。上一个 AI 已完成全部代码交付与 7 个真机问题修复，当前剩最后一段端到端问题未闭环。请先完整阅读本文档，再开始行动。

> **【2026-09-18 14:40 闭环】本文档的残留症状已全部定位并修复，提交 `81d91195`，真机验证通过。**
> 经验沉淀见 `Doc/Expe/xiaomi-gateway-voice-key-and-hid-passthrough-2026-09-18.md` 与
> `Doc/Expe/claude-memory-distilled.md` §1.6 / §1.10；方案文档补齐 §6.3 问题 8-12。

## 零、闭环结论（2026-09-18 14:40）

两个残留症状是**两套完全独立的通道**，互相掩盖，必须分开定位：

| 症状 | 根因 | 修复 |
|---|---|---|
| 按语音键屏幕报 `Audio wait:ESP_ERR_NO_MEM` | `xStreamBufferCreate(64000)` 走 `pvPortMalloc`，而 IDF 把 `portFREERTOS_HEAP_CAPS` 硬编码为内部 RAM，网关模式凑不出 64KB 连续内部 RAM | 改 `xStreamBufferCreateWithCaps(..., MALLOC_CAP_SPIRAM)` + 网关入口预创建 |
| 遥控器除语音键外所有按键无反应 | ① HID 服务内 25 个 Report 特征、7 个带 notify，代码只认第一个；② CCCD 靠「特征值 +1」猜（0x0076 真值 0x0078）被拒；③ HOGP 直通只发第一条 role=slave 链路；④ **Windows 没有 VS-53A8 的 OS 级配对节点**（无 HOGP HID 链路） | ① 全量订阅 + 按句柄集合接收；② 描述符枚举找 0x2902；③ 广播所有外设链路；④ 用户在 Windows 设置手动配对 |

**关键教训**：语音键会话沿由 **ATVV Control 帧**驱动（`gateway_atvv_on_press`），其他按键走
**HOGP HID Report**——「语音正常、其他键全死」会把排查方向误导到 ATVV/音频侧，实际断点在 HID 订阅 +
系统配对。**BLE HID 直通还要求目标机与设备有 OS 级配对**，这是固件之外的硬前提。

**本文档中已被推翻或修正的判断**：
- §三「MAX_CONNECTIONS 被 HID 主机+小米占满」属误判：**Windows 的 HID 主机与 app 复用同一条 ACL 链路**
  （app 重连日志 `link-layer connected VS-53A8 after 0ms`），设备侧始终只有一条 peripheral 连接——
  共存不需要第二条连接，也**不需要**保持广播，勿据此改 `voice_ble` 的 `stop_advertising()`。
- §五.5「`sec_cb` 每次连接都 `delete_peer`」→ 已修（只在加密失败时清键重配）。
- §五.6「`XIAOMI_HID_HOST_EXTRAS` 三件套二分验证」→「三件套致停推」假设已证伪：CCCD 订阅与 Exit
  Suspend 都是协议必需动作，已拆成独立开关默认打开。

**遗留项（未做，建议排期）**：
1. ~~桌面端失效恢复路径删掉系统配对却不重建~~ **已修（`e0db7c63`）**：新增 `TryRestoreOsBondAsync`，
   两条恢复路径（stale-bond / Unreachable）重开设备后补一次 `PairAsync`；小米遥控器不走该路径。
   同批还补了**僵尸会话识别**：纯函数 `BleProtocol::PlanZombieRecovery` 判别「链路仍 Connected
   却长时间零入站」的僵尸会话（订阅与写入全部假成功、固件侧 `state_sub=0`，录音必被拒），
   经 `on_session_zombie` 回调弹托盘气泡指引用户到 Windows 蓝牙设置重配。真机已验证触发。
   **仍待用户手动完成一次系统重配**才能恢复语音；自动化自愈（僵尸即 unpair+radio reset+PairAsync）未做。
2. ~~app 配对/解绑需要用户手动做两遍系统蓝牙配对~~ **已修（`43789aa9`）**：解绑侧本来就会清系统配对，
   配对侧原本只有小米遥控器走 —— 现两类设备统一「先系统配对、再 GATT 连接」，失败策略由纯函数
   `BleProtocol::PlanAfterOsBondAttempt` 决定（小米硬前置 / VS 软前置降级）。真机验收：
   `os unpair: removing Windows pairing` ⇒ `VS os pairing bonded` ⇒ `stage=ready 954ms`，全程未打开系统设置。
   设计 `Doc/Plan/windows-app-os-pairing.md`。
3. `CONFIG_BT_NIMBLE_LOG_LEVEL` 在本 §五.6 曾列为「回 WARNING」，但实为 2026-06-28 提交 `81c37c6b` 的既有设置
   （非本次调试引入），是否回退待定。
4. P1 按键自定义（对话框配动作 → 软件路由 → 注入）尚未做真机验收。
5. 固件升级一律走 **COM19 串口**；BLE 本地文件 OTA 在网关模式下会中途断链（本次 197KB 处断），勿再用。


## 一、任务背景

用户需求：小米蓝牙遥控器 2 Pro 配对到 M5Stack StickS3（ESP32-S3，网关模式），实现：
1. **语音输入**：按住遥控器语音键说话 → StickS3 经 ATVV 协议收音 → ADPCM 解码 → Opus → BLE 上行 → Windows 桌面端 VoiceStick.exe ASR → 文字输入
2. **按键自定义**：遥控器按键可配置为「HOGP 直通系统层」（Phase 1 已交付）或「软件路由到桌面端自定义动作」（P1，本次交付）

方案文档（必读）：`Doc/Plan/xiaomi-remote-stick-gateway.md`（§5.4.1 实施定案、§6.3 真机修复记录、§8.1 交付清单）。协议：`Doc/Ref/protocol.md` 的 `gateway_key`/`gateway_keymap_set` 章节。

## 二、已交付（全部编译/测试通过，多数已提交）

### 提交（feat/stick-gateway 分支）
1. **feat(网关)**：Phase 2 语音链路全链 + P1 按键软件路由。固件 4 个新模块（`gateway_adpcm`/`gateway_atvv_session` 纯逻辑 TDD、`xiaomi_atvv_client` NimBLE 薄壳、audio_pipeline 外部音频源）+ `main.c` 接线（`APP_INPUT_SOURCE_XIAOMI` 走主键状态机 + 延迟停录 170ms）+ Windows 桌面端全链（StateEvent 解析/协调器路由下发/`OnGatewayKeyEdge` 注入/配置复用现有对话框）。host 单测 120/120 + 112/112。
2. **88a1f2b4**：ATVV 发现延迟 10s 避开参数窗口 + 发现看门狗；MAX_CONNECTIONS 2→3。
3. **1325c5eb**：CCCD 直写 + 重试；ReportMap 读取。
4. **e240d655**：HID Control Point（Exit Suspend）；停用会话轮询看门狗。
5. **未提交的工作区改动**（都已在设备上验证过行为）：
   - `XIAOMI_HID_HOST_EXTRAS=0` 纯净模式开关（CCCD/ReportMap/ControlPoint 三件套关闭）
   - 改绑自愈：direct connect 连续 3 次失败清 NVS peer 转扫描重配对（`gateway_hid_host.c`）
   - **StreamBuffer NULL 崩溃修复**（`audio_pipeline.c`：start_ext 预创建 + read_frame_ext/drain 判空）——这是最后一个关键修复
   - 会话轮询（GET_CAPS 30s/bounce 65s）已停用

## 三、当前精确状态（最后用户反馈：「还是有问题」，未说具体现象）

**第一个动作必须是：问清用户具体现象**（语音键？方向键？崩溃重启？）。

已验证的事实链：
- 遥控器已重新配对（用户进配对模式 + 设备自动抓取），**按键沿已恢复到达设备**（12:14 日志：`Control notify op=0x04`、`button front down source=3`、`小米键截留 usage=0x003e`）
- 按语音键曾触发设备崩溃（`assert failed: xStreamBufferReceive` NULL 断言，已修复并于 12:19 烧录）
- 12:19 修复版烧录后用户再测说「还是有问题」，但监听窗口（300s）内无按键数据到达——**具体现象未确认**

## 四、已定案的 7 个真机问题（不要重查）

| # | 现象 | 根因 | 修复 |
|---|------|------|------|
| 1 | ATVV 发现链静默挂死 | 小米连接 ~4.9s 经 L2CAP 请求参数（latency=49/timeout=500 违反 BLE 规范），NimBLE 转发 HCI 被 0x212 拒，**恰在途的 ATT 事务被吞** | 发现延迟 10s + 4s 看门狗 |
| 2 | app 作为第三方连不进 | MAX_CONNECTIONS=2 被 HID 主机+小米占满 | 2→3 |
| 3 | 设备重启后 app 僵尸会话（零 notify 118s 超时循环）/OTA 报 0x80650008 | Windows HID 主机每次设备重启重新配对（LTK 轮换），WinRT 缓存加密上下文失效 | OS 级移除设备重新配对（用户 07:03 做过） |
| 4 | latency=49 协商生效后 ESP32 central 收不到对端任何 notify | 高 slave latency 下 controller bug | CONN_UPDATE 成功回调检测 latency>8 自动修正为 24-40/latency 0（上限 3 次） |
| 5 | 小米对 disc_all_dscs（Read By Type 0x2902）完全无响应 | 小米固件不支持描述符枚举 | CCCD 直写 Report+1 句柄（spike 实证 0x65） |
| 6 | 遥控器所有按键零推送（链路好/CAPS 应答正常） | **排查期几十次 delete_peer 重配对搞坏遥控器配对状态机** | 用户进配对模式 + 设备重新抓取配对（已做，按键恢复） |
| 7 | 按语音键设备崩溃重启（屏幕闪 "Pairing"） | start_ext 时 StreamBuffer 懒创建晚于 audio_task，`xStreamBufferReceive(NULL)` 断言 | 预创建 + 判空（已烧录待验证） |

## 五、下一步排查建议（按优先级）

1. **确认现象** → 若语音键仍无反应：设备串口监听下按语音键，看 `button front down source=3` 是否出现（出现=会话链触发，问题在下游）
2. **分段验证语音链**：
   - 设备侧：`start session`（start_recording）/`external source session`（start_ext）/`audio task exit enqueued=N`（N>0=有帧）
   - app 侧日志：`button_down` 事件 → `audio frame` → `paste_complete`
   - 设备崩溃：`rst:`/`assert` 行
3. **若 button_down 到了 app 但无 audio frame**：查 BLE 吞吐（fast interval 请求）或 seq 丢帧
4. **若设备收到沿但无 start session**：`start_recording denied` 日志（ble_ready/ui_state 检查）
5. **重要遗留隐患**：`gateway_hid_host.c` 的 `sec_cb` 里 **每次连接都 `ble_store_util_delete_peer` + 重配对**——这正是搞坏遥控器（问题 6）的元凶，代码还在！bond 正常时应直接加密恢复、不删键。建议改为「加密失败才删键重配」
6. **收尾清单**（功能验证通过后）：`XIAOMI_HID_HOST_EXTRAS` 开关二分验证（三件套逐项打开看是否有益，最终定去留）→ 清理排查诊断日志 → `CONFIG_BT_NIMBLE_LOG_LEVEL` 回 2（WARNING）→ 未提交改动提交 → 方案文档 §6.3 补充问题 6/7 → P1 按键自定义真机验收（对话框配动作→软件路由→注入）

## 六、环境与工具速查

- **设备**：VS-53A8（USB 串口 COM19）。**用户只有这一台开机**；日志里大量出现的 VS-5A74 是「本机麦克风模式」的虚拟设备标识，**不是物理设备**（上一个 AI 曾误判为第二台 Stick，被用户纠正）
- **遥控器**：内置锂电池（非纽扣，用户纠正过）；配对模式=用户长按组合键（灯快闪）；单连接设备
- **固件构建/烧录**：`python scripts/idf_cli.py -c`（编译）、`-u -p COM19`（烧录，**自动重启无需按键**）
- **host 单测**：`cd firmware/components/gateway && python test/run_tests.py`（应 120/120 + 112/112）
- **Windows 构建**：根目录 `build_win.bat`；测试直跑 `desktop\windows\build-x64\voicestick_windows_tests.exe`（退出码 0=全过）
- **串口监听**（pyserial 115200）；**DTR 复位抓启动日志**（`dtr=False→True 0.3s→False` 后连续读）——**运行时日志经常丢失**（Core0/NimBLE host 任务输出读不到是已知坑），关键事件要靠启动段或 app 日志
- **app 日志**：`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`（连接/事件/ASR 全链）
- **语音会话关键日志关键词**：设备=`button front down source=3`/`start session`/`external source`/`audio task`；app=`state event type=button_down`/`audio frame`/`paste_complete`
- 用户操作习惯：按语音键说话 2-3 秒松开；测试前先与用户约定（等待用户按键期间不要空转，监听窗口给足后主动汇报）

## 七、行为红线（项目纪律）

- 严格 TDD：纯逻辑模块 host 单测先行（配方见 `run_tests.py`，MSVC）
- Windows 源码在 `.gitignore` 里，提交必须 `git add -f` + `git ls-files` 验证
- 不伪造结果：无设备/无凭据时如实报告
- 每次烧录后告知用户测试点，明确等待用户反馈再继续
- 改协议/状态机同步 `Doc/Ref/protocol.md` 与方案文档
