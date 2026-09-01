# 小米蓝牙遥控器 2 Pro 接入设计

- 状态：已评审，实施中
- 日期：2026-08（分支 `feat/add-MiRemote`）
- 协议事实来源：`C:\Dev\FFE\George\MiVibe-Remote`（GPL-3.0，仅作协议参考；本项目 Apache-2.0，全部代码按协议规范重新实现，不复制其代码）

## 1. 背景与目标

Voice Stick 目前唯一输入设备是自研 M5Stack StickS3（ESP32-S3）固件。小米蓝牙遥控器 2 Pro（VID `0x2717`/PID `0x32B8`，内部型号 RC001/RC003）自带麦克风与语音键，硬件成熟易得。本设计让它成为第二种输入设备，与 StickS3 并存：

- 配对：桌面端发现、配对、记忆、自动重连
- 使用：按住语音键说话 → ASR → 粘贴/字幕/微信输入法，复用现有交互状态机
- 切换：两类设备同时连接，按哪台用哪台；托盘菜单统一展示
- 设置：按设备输出覆盖复用 `[device.<id>.*]` 框架；能力差异驱动 UI 显隐
- 测试：协议单测 + 真机 golden 采集 + 离线 ASR 评测 + 真机探针的闭环

## 2. 范围

### 做

- Windows 桌面端完整支持（配对/使用/切换/设置/测试闭环）——第一阶段
- macOS 桌面端移植——第二阶段
- 语音键 → `primary` 角色全语义（按住说话、松开结束、双击注入 Enter）
- 标准 Battery Service（0x180F/0x2A19）电量上报
- 语音键附带 F5 系统键的抑制（Windows 低级键盘钩子）

### 不做（一期）

- 固件改动（ESP32-S3 完全不动；小米遥控器是独立 BLE 外设）
- 其余 HID 键（OK/返回/方向/菜单等）的拦截与 `secondary` 角色映射——一期保持系统原生行为，二期可选（避开 Windows kbdhid 丢弃 0xF1 返回键的坑，不引入 Frida）
- 小米遥控器的 OTA 升级（无公开协议）
- 体感鼠标/编码器/屏幕状态下发（设备无此硬件能力）

## 3. 协议规范（逆向事实，已核实）

### 3.1 键位通道：标准 HID over GATT

普通键走标准 HID service `0x1812`，被 OS 当键盘消费。Report ID=1，payload 为最多 3 个 16 位小端 HID usage；按下/释放靠前后报告集合差分，空集合为全部释放。

键码表（Keyboard/Consumer page usage）：

| usage | 键 | usage | 键 |
|---|---|---|---|
| 0x28 | OK | 0x65 | 菜单 |
| 0x4A | 主页 | 0x66 | 电源 |
| 0xF1 | 返回（非标准） | 0x7F | 静音 |
| 0x4F/0x50 | 右/左 | 0x80/0x81 | 音量+/− |
| 0x51/0x52 | 下/上 | 0x35 | TV |

**语音键不是普通 HID 键**：按下时遥控器发起 ATVV 会话（见 3.2），同时向 OS 多发一个 F5 键，需抑制以免焦点应用收到刷新。

### 3.2 音频通道：Google ATVV 自定义 GATT service

| 特征 | UUID | 方向/属性 |
|---|---|---|
| Service | `AB5E0001-5A21-4F05-BC7D-AF01F617B664` | — |
| TX（命令） | `AB5E0002-…-B664` | 主机→遥控器，write without response |
| Audio | `AB5E0003-…-B664` | 遥控器→主机，notify |
| Control | `AB5E0004-…-B664` | 遥控器→主机，notify |

会话流程（**遥控器主导，主机应答**）：

1. 连接后发现 service + 3 特征，订阅 Audio/Control notify，写 TX `0A 01 00 00 03 03`（GET_CAPS v1.0）
2. Control 收 `0x0B`（CAPS 应答）：`[1:3]` 版本 BE16；v≥1.0 时 `[3]`=codec 位掩码、`[4]`=interaction；`[5:7]`=帧长 BE16（0 → 默认 120 字节）。codec 位 `0x02`=16kHz、`0x01`=8kHz；**只接受 16kHz**。存在「报 v1 但用旧版双字节 codec 布局」的兼容分支
3. 按下语音键：Control 收 `0x08`（MIC_OPEN 请求）→ 主机回写 TX `0x0C 0x00`（v≥1.0；旧版 `0x0C 0x00 <codec>`）
4. Control 收 `0x04`（流开始，`[1]`=interaction、`[2]`=codec、`[3]`=session id 可选）→ Audio notify 流入
5. Control 收 `0x0A`（AUDIO_SYNC，`[4:6]`=predictor BE 有符号、`[6]`=step index）→ 重置 ADPCM 解码器并清空帧累积器，同时清空 PCM 组帧器与按键缓冲。**RC003 坑：会话开始编码器从 0/0 重启但不发 SYNC，必须在收到 `0x04` 时硬重置 `reset(0,0)`，否则第二次按键 DC 饱和**
6. 松开语音键：Control 收 `0x00`（STOP）。**Audio 与 Control 是两条特征，最后音频尾包可能在 STOP 之后到**——留 150ms 宽限；STOP 后 300ms 内拒绝重开会话
7. 断开/退出时主机写 TX `0x0D <sessionID>`（MIC_CLOSE）

音频格式：**IMA/DVI ADPCM，4bit/采样，高半字节优先**，16kHz 单声道。Audio notify 是裸 ADPCM 字节流，无帧头无序号，按 CAPS 帧长（默认 120 字节=240 采样）跨 notify 累积切帧。解码为 int16 PCM 后做三点平滑 + ±24dB 限幅增益。ADPCM 无序号，丢包即漂移直到下次 sync。

### 3.3 配对与发现

- 必须先由 **OS 完成 Bond 配对**（无配对码/账号 token）；Windows 优先尝试 WinRT 应用内配对，失败则引导系统蓝牙设置
- 名称白名单（trim+小写）：`MI RC`、`Xiaomi Bluetooth Remote 2 Pro`、`小米蓝牙语音遥控器`、`RC001`/`RC003`；或广播含 ATVV service UUID
- 电量：标准 Battery Service 0x180F / 0x2A19（读 + notify）

## 4. 总体架构

```
小米遥控器                       StickS3
    │                               │
    ▼                               ▼
┌────────────────────────────────────────────┐
│ BleCentralWin（按 device_class 分支连接/订阅）  │
├───────────────────┬────────────────────────┤
│ XiaomiAtvvSession  │ 现有 StickS3 直线流程    │  ← voicestick_core 新增纯逻辑类
│ (ATVV 状态机 +      │                         │
│  ADPCM→PCM→Opus)  │                         │
└───────┬───────────┴────────────┬───────────┘
        │ 归一化 StateEvent       │ 归一化 AudioFrame(Opus)
        ▼                        ▼
   VoiceStickCoordinator（状态机零改动）→ ASR/字幕/wechat/注入
```

设计决策：

1. **不引入大抽象接口**。`DeviceSession` 加 `device_class` 字段，仅在服务匹配、特征发现、值分发、`Send*` 门控四处分支；ATVV 会话逻辑封装为 core 纯逻辑类 `XiaomiAtvvSession`（不碰 WinRT，可单测）
2. **音频归一化在进协调器之前完成**：ADPCM→PCM→Opus（core 新增 `AudioOpusEncoder`，libopus 已是 core 链接依赖），产出标准 `AudioFrame`，下游 Ogg mux/ASR/字幕/wechat/调试缓存零改动
3. **按键归一化**：ATVV Control `0x08`/`0x00` → `button_down`/`button_up` × `primary`；双击由适配层时序检测合成 `button_double_click`（语义镜像固件双击参数，默认窗 350ms 有意小于固件 500ms，勿对齐）
4. **设备 ID**：StickS3 保持 `VS-XXXX`；小米遥控器用 `RC-XXXX`（蓝牙地址低 16 位，沿用现有兜底逻辑）。`NormalizeDeviceId`/`DeviceIdFromName` 扩展双前缀，向后兼容旧配置
5. **能力标志**：`PairedDeviceEntry.hardware`（`"stick_s3"`/`"xiaomi_remote_2_pro"`）派生能力集（has_screen/has_ota/has_encoder/has_imu/has_battery），驱动 `Send*` 跳过与托盘菜单显隐（复用 `encoder_present` 范式）

## 5. 详细设计

### 5.1 Windows core（voicestick_core，新增文件全部纯逻辑可单测）

| 文件 | 内容 |
|---|---|
| `src/xiaomi_atvv_protocol.h/.cc` | UUID 常量、GET_CAPS/MIC_OPEN_ACK/MIC_CLOSE 构造、CAPS 解析（含旧版布局兼容）、Control opcode 常量 |
| `src/ima_adpcm_decoder.h/.cc` | IMA/DVI ADPCM 解码器（高半字节优先，predictor/step 钳位 [-32768,32767]/[0,88]）；标准 IMA 步长/索引表 |
| `src/pcm_postprocessor.h/.cc` | 三点平滑 + 增益限幅（±24dB）；`FrameAccumulator`（按帧长跨包切帧） |
| `src/xiaomi_atvv_session.h/.cc` | 会话状态机 Idle→CapsRequested→Ready→TapPending→Streaming→Draining→WaitSecondTap（另含 Error 终态）；输入 Control opcode/Audio 字节/时钟，输出动作列表（写 TX 命令 / 合成 StateEvent / 合成 AudioFrame）；含 RC003 硬重置、150ms 尾包宽限、300ms 重开拒绝窗、双击时序检测 |
| `src/audio_opus_encoder.h/.cc` | libopus 编码器封装：16kHz/mono/40ms（640 采样/帧，与固件 `AUDIO_FRAME_MS=40` 一致），参数对齐 `firmware/components/audio_pipeline` 实际配置 |

修改：

- `ble_protocol.h/.cc`：`DeviceIdFromName`/`NormalizeDeviceId` 支持 `RC-` 前缀；新增 `DeviceClass` 枚举与名称/广告判定辅助
- `app_config.h/.cc`：`[device.<id>.xiaomi]` 表（`gain_db`、`double_click_ms`），解析/序列化/round-trip；`XiaomiSettingsForDevice(device_id)` 访问器（结构镜像 `[device.<id>.encoder]`）
- `CMakeLists.txt`：新源文件加入 `voicestick_core`

### 5.2 Windows BLE 接入（VoiceStickApp 层）

- `ble_central_win.h/.cc`：
  - 发现过滤 `HandleAdvertisement`：新增小米白名单通道（名称/ATVV UUID）→ 生成 `RC-XXXX`，查 `paired_device_ids_` 放行
  - `DeviceSession` 加 `device_class`；小米类连接序列：发现 ATVV 3 特征（Audio/Control 订阅 + TX 可写）+ 可选 Battery 0x2A19；`ready` 判定按类放宽
  - 值分发：ATVV Control/Audio → `XiaomiAtvvSession`，产出的 StateEvent/AudioFrame 走既有 `on_state_event`/`on_audio_frame` 回调
  - `Send*`（ui_state/interaction/encoder/airmouse/OTA…）对无能力会话静默跳过；协调器连接时全量同步循环按能力门控
  - 电量 0x2A19 → 合成 `battery_status` StateEvent
  - 心跳差异：小米设备空闲时无入站 notify（不像 StickS3 心跳写有 `battery_status` 回包），为兼容既有 90s 静默拆除机制，BLE 层对小米会话周期读探针特征（电量 0x2A19，缺省时 GAP 0x2A00 设备名兜底）强制链路层收发以刷新 `last_rx_ms` 维持活性；读返回 Unreachable/抛异常按链路已死拆除
  - 断开时先发 MIC_CLOSE；僵尸链路四机制（陈旧判定/安定窗/打标/免退避重试）复用
- F5 抑制（VoiceStickApp）：`SetWindowsHookEx WH_KEYBOARD_LL`，仅「80ms 窗内有 ATVV MIC_OPEN」时吞 F5，配置开关 `xiaomi_suppress_f5`（默认开），按「有已配对/已连接 RC 设备」门控装载（未配对小米的用户不挂钩子）；装载/卸载由 `SyncF5Suppressor` 按需执行（配对完成、配置热更、连接集变化时重估，无 RC 设备即卸载钩子）。已知边界：F5 经 HID 栈、MIC_OPEN 经 ATT 栈，F5 先于 MIC_OPEN 到达的极端到达序下会漏吞（属罕见竞态，接受）

### 5.3 Windows UI

- `pair_device_dialog.cc`：双模扫描（VS-* + 小米白名单），候选带类型标签；`RC-XXXX` 展示；WinRT 应用内配对尝试，失败引导系统蓝牙设置；配对成功写 `paired_devices`（`hardware="xiaomi_remote_2_pro"`，地址持久化优先按地址重连）
- 托盘设备子菜单（`win32_app.cc:1245-1427`）：设备条目带类型标签；小米设备隐藏「固件更新」「编码器设置」「体感鼠标」「电池监测」（按能力显隐）；按设备输出覆盖直接复用
- 「遥控器设置…」对话框（照 `encoder_settings_dialog` 模式，仅小米设备显示）：`gain_db`、`double_click_ms`。F5 抑制保持全局 TOML 配置 `xiaomi_suppress_f5`，不进按设备对话框（避免按设备对话框写全局键的结构张力）；设置值由桌面端会话创建时消费，热更对已连接会话不生效，重连后生效
- 本地化：全部新文案进 `localization.h StringId` + `localization.cc` 中英表

### 5.4 macOS（第二阶段）

> **暂缓实施（2026-09，用户决策），待后续版本。** 本节描述的 macOS 端方案本轮未实施，保留作后续移植依据；当前仅 Windows 端支持小米遥控器。

- `BleCentral.swift`：扫描双 service UUID；按设备类分支特征发现/订阅/值路由；`deviceID(from:)` 与 `AppConfig.normalizedDeviceID` 扩展 `RC-` 前缀
- 新增 `XiaomiAtvvProtocol.swift`/`XiaomiAtvvSession.swift`（按 §3 规范重新实现）
- Opus 编码：先试 AudioToolbox `kAudioFormatOpus`（macOS 12+）；不可行则 vendored libopus 以 SwiftPM C target 接入（参考 `CZlib` shim 模式）
- 配对窗双模扫描；`StatusController` 菜单能力显隐；F5 抑制用 CGEvent tap
- ui_state 下发天然降级（缺特征仅 NSLog 跳过），无需额外处理

### 5.5 切换语义

两类设备同时连接，主录音会话全局单例（`active_device_id_`）：按哪台设备的语音键/主键，哪台成为活跃设备；字幕会话按 `(device_id, session_id)` 隔离。无独立「切换」开关，交互即切换。托盘菜单按设备展示状态与输出覆盖。

## 6. 配置

新增（`Doc/Ref/desktop-config.md` 同步）：

```toml
# 全局
xiaomi_suppress_f5 = true        # 语音键附带 F5 的抑制开关（Windows）

# 按设备（镜像 [device.<id>.encoder] 结构）
[device.3A7F.xiaomi]
gain_db = 12.0                   # ADPCM 解码后增益，±24 限幅
double_click_ms = 350            # 语音键双击时序阈值
```

`paired_device_ids` 接受 `VS-XXXX` 与 `RC-XXXX` 混合列表；`paired_devices` 条目 `hardware` 新增取值 `"xiaomi_remote_2_pro"`。

## 7. 测试计划

### 7.1 单元测试（`desktop/windows/tests/core_tests.cc`，CTest）

- `TestXiaomiAtvvCapsParsing`：v1.0 正常/旧版双字节布局/8kHz 拒绝/帧长缺省 120
- `TestImaAdpcmDecoderGolden`：标准向量（`0x11 → [1,2]` 起）；`TestImaAdpcmDecoderGoldenFixtures`：扫描 `scripts/e2e_test/fixtures/xiaomi/**` 的 golden 采集（`VOICESTICK_REPO_ROOT` 编译宏解析路径，`VOICESTICK_ATVV_FIXTURES_DIR` 环境变量可覆盖），按 sidecar 复现解码并与 raw.wav/wav 逐样本对拍；无 fixtures 打印 SKIP 不算失败
- `TestFrameAccumulator` / `TestPcmPostprocessor`
- `TestXiaomiAtvvSessionFlow`：完整会话（CAPS→OPEN→ACK→STREAM→STOP→CLOSE）、RC003 无 SYNC 硬重置、150ms 尾包宽限、300ms 重开拒绝、8kHz 失败路径
- `TestXiaomiAtvvSessionKeyMapping`：0x08/0x00→primary down/up、双击合成 double_click、三击不重复触发
- `TestAudioOpusEncoderRoundTrip`：编码→解码 SNR sanity、帧长 640 对齐
- `TestDeviceIdRcPrefix` / `TestAppConfigXiaomiTable`（round-trip）
- 协调器行为：FakeBleCentral 注入小米事件流（录音/取消/wechat/字幕路径各一）

### 7.2 闭环评测（`scripts/e2e_test/`）

- `atvv_capture.py`（bleak）：真机抓取会话 → `fixtures/xiaomi/<ts>/`（Control 事件 JSONL + 原始 ADPCM + `session_<N>.json` sidecar + 双 WAV：`*.raw.wav` 纯解码供 golden 对拍，`*.wav` 平滑+增益供听感/对照）——单测 golden 的数据源
- ASR 离线评测：`atvv_bench.py` 按 sidecar 复现解码 → 桌面端同款后处理（平滑+增益）→ **裸 PCM 直送**（火山 `format=pcm` / 腾讯 `voice_format=1`；Python 侧不引入 Opus 绑定，PCM 是协议支持的等价路径）→ 复用 `asr_bench/` 指标与报告口径。与 StickS3 语料基线（`run_asr_bench.py`）对比得小米麦克风识别基线
- 调参闭环：`atvv_bench.py --gain-db X --no-smooth` 改参数重跑 bench 对比收敛（无需硬件）
- `atvv_probe.py`（bleak）：会话建立延迟、首音频帧延迟、帧统计、STOP 尾包时延分布；长连接静置稳定性（`ble_idle_stability_probe.py` 思路），检测 VoiceStick.exe 互斥
- L4 微信输入法：`run_l4_wechat.py` 换输入源复跑

### 7.3 集成测试（`integration_tests.cc`）

- golden 会话经 `XiaomiAtvvSession` 重放为 Opus 帧 → 协调器 → 真实火山 ASR，断言返回非空文本（内容准确率归 `atvv_bench.py`）；无 `volcengine_api_key` 返回 77 SKIP（沿用惯例），无 fixtures 时该段打印 SKIP 不影响 L1 语料结论

### 7.4 真机验收清单

1. 配对：对话框发现小米遥控器（类型标签），配对/记忆/重启自动重连
2. 使用：按住语音键说话→识别粘贴；松开即停；双击语音键注入 Enter；F5 不泄漏到焦点应用
3. 切换：与 StickS3 同连交替使用，互不干扰，各自会话正确
4. 能力：托盘菜单对小米设备隐藏固件更新/编码器/体感项；电量正确显示
5. wechat 模式：经小米遥控器可用
6. 稳健：断连/重连/睡眠唤醒自动恢复（含僵尸链路场景）

## 8. 边界与异常

| 场景 | 处理 |
|---|---|
| CAPS 只有 8kHz | 判定不支持，断开重连一次后报错事件，不进入录音 |
| 未 Bond 配对 | ATVV service 发现失败 → 引导系统配对 |
| 语音键连击（>2） | 时序窗内只合成一次 double_click，后续 down 正常开录 |
| STOP 后无尾包/尾包迟到超窗 | 宽限到期强制结束会话，音频按已收齐部分送 ASR |
| 会话中断连 | 发 MIC_CLOSE（尽力），清理会话状态，走既有重连退避 |
| 与 StickS3 同时按键 | 先到先占 `active_device_id_`，后者按「识别中忽略新录音」规则处理（既有行为） |
| MTU/帧长协商异常 | 帧长取 CAPS 值，缺省 120；累积器按协商值切帧，不写死 |

## 9. 许可证说明

MiVibe-Remote 为 GPL-3.0，本项目为 Apache-2.0。本设计只采用**协议事实**（UUID、opcode、会话流程、IMA ADPCM 这一 1992 年公开标准算法），全部代码按本文档规范重新实现，不复制 MiVibe-Remote 源码结构与表达。文档中保留出处致谢。
