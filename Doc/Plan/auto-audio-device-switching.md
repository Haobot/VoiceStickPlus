# 自动音频设备切换（降低会议软件干扰）

## 1. 背景与目标

### 1.1 背景

当前 Windows 端 `wechat_input_method` 输出模式依赖第三方 VB-CABLE 虚拟音频驱动：

```text
BLE audio_tx → Coordinator → OpusDecoder → PcmRingBuffer
  → WasapiVirtualMicRenderer → CABLE Input（WASAPI eRender，按设备名子串匹配）
  → [Windows 音频栈] → CABLE Output（系统默认录音设备）
  → 微信输入法取音 → 云端 ASR → 上屏
```

为让微信输入法取到音，用户必须把**系统默认录音设备**设为 CABLE Output。痛点由此产生：

- **持续干扰**：默认设备常驻 CABLE Output，会议软件（Zoom/腾讯会议/Teams）若用默认设备，平时说话会议收不到真实麦克风（CABLE Output 静音），VoiceStick 录音时还会把语音灌进会议。
- **手动配置门槛**：非技术用户难以独立完成"切默认设备"。

### 1.2 目标

把默认录音设备的指向**动态化**：平时指向真实麦克风（会议正常），仅 VoiceStick 录音那几秒指向 CABLE Output（微信输入法取音），录音结束切回。把对会议软件的干扰从"持续"压到"仅 VoiceStick 录音期间"，并进一步用**设备角色分离**把通信类会议软件的干扰压到零。

### 1.3 与既有文档的关系

`Doc/Plan/windows-vbcable-as-is-bundling.md` 第 7–8 节已设计"录音期临时切默认设备"的框架（`DefaultAudioDeviceController` 接口、时序候选、清理分支、残留自愈、真机验证项）。本方案是其**独立化 + 角色分离增强**：

- **独立化**：把"自动切换"从"VB-CABLE 随包携带/首启安装"大方案中拆出，作为可单独实施的功能——用户已手动装好 VB-CABLE 即可受益，不依赖驱动携带/提权安装那套合规工程。
- **角色分离增强**：既有文档只说"切默认设备"，未展开 `eConsole`/`eCommunications` 角色分离。本方案以角色分离为核心，把 Teams/Skype 等通信类会议软件的干扰降到零。
- **现状对齐**：既有文档写于实现前，此后代码仍未落地。本方案重新核对代码现状（`auto_switch_default_recording_device` 字段已预留、协调器从未读取、无 `IPolicyConfig` 实现），据此给出可直接进入 TDD 的实施路径。

## 2. 现状分析（代码已核对）

### 2.1 已就绪

| 项 | 位置 | 状态 |
|---|---|---|
| 配置字段 `auto_switch_default_recording_device` | `app_config.h:108`（默认 false） | ✅ |
| TOML 序列化/反序列化 | `app_config.cc:366 / 515-516 / 659-660` | ✅ |
| 配置模板占位 | `config.template.toml:90-91` | ✅ |
| 字段往返单测 | `core_tests.cc:2420 TestWechatInputMethodConfigRoundTrip` | ✅ |
| WASAPI 写 CABLE Input | `wasapi_virtual_mic_renderer.cc`（按 `virtual_mic_playback_name` 子串匹配 eRender 设备） | ✅ |
| 会话启停收敛点 | `StartWechatInputMethodSession` / `StopWechatInputMethodSession`（`voice_stick_coordinator.cc:482 / 543`） | ✅ |
| COM 依赖 | `voicestick_core` 已链接 `mmdevapi ole32 uuid`（`CMakeLists.txt:83`） | ✅ |

### 2.2 未实现（本方案要补的空白）

- 协调器 `StartWechatInputMethodSession` / `StopWechatInputMethodSession` **从未读取 `auto_switch_default_recording_device`**，无任何切换调用。
- 全仓库无 `IPolicyConfig` / `SetDefaultEndpoint` 实现。
- 设置对话框只暴露 `hotkey`、`virtual_mic_playback_name`（`settings_dialog.cc:664-686`），**无 `auto_switch_default_recording_device` 勾选框**。
- 切换目标 `CABLE Output`（录音端）的设备名无配置项——现有 `virtual_mic_playback_name` 是播放端 `CABLE Input`，二者是不同设备。

## 3. 已确认决策

| 决策 | 选择 | 含义 |
|---|---|---|
| 切换角色范围 | **角色分离** | 仅动态切 `eConsole`（微信输入法用），`eCommunications`（会议软件用）固定真实麦不动 |
| 真实麦克风来源 | **始终自动记录** | 每次 Start 前读当前 `eConsole` 默认设备 ID，Stop 切回；不新增物理麦配置项 |

### 3.1 角色分离的干扰模型

Windows 区分两类默认录音设备：

| 角色 | 用途 | 本方案取值 |
|---|---|---|
| `eConsole`（默认设备） | 通用应用、输入法 | 动态：平时真实麦 ↔ 录音期 CABLE Output |
| `eCommunications`（默认通信设备） | 通信类应用（Teams/Skype/电话） | **固定真实麦，不碰** |

效果分级：

- Teams/Skype 等遵循 `eCommunications` 的会议软件 → **全程零干扰**（VoiceStick 录音期间也不受影响，因为只切了 eConsole）。
- Zoom/腾讯会议等若配置为用"默认设备"(eConsole) 而非"通信设备" → 平时正常，仅 VoiceStick 录音那几秒被切走。仍比现状（默认常驻 CABLE、全程干扰）大幅改善。

### 3.2 "始终自动记录"的前提与退化

选"始终自动记录"隐含前提：**用户平时默认(eConsole)设备已是真实麦克风**。若用户保持现状默认=CABLE Output，则 Start 时记录的 saved_id 就是 CABLE，切回等于没切——功能退化但不报错。

应对：首次启用 `auto_switch` 时做**退化检测**（第 9 节），若当前默认已是 CABLE Output，提示用户先把平时默认设备改为真实麦。同时建议把 `eCommunications` 也设为真实麦（一次性，会议零干扰基线）。

## 4. 总体设计

### 4.1 数据流

```text
按下主键（wechat 模式 + auto_switch=true）
  ├─ 1. 读当前 eConsole 默认设备 ID → saved_id（自动记录）
  ├─ 2. 枚举 eCapture 找 "CABLE Output" → cable_id
  ├─ 3. SetDefaultCapture(cable_id, {eConsole})  ← 只切 eConsole
  ├─ 4. 持久化状态文件（switched=true, saved_id）  ← 供崩溃自愈
  ├─ 5. hotkey->SendDown()（微信弹框，此时默认已是 CABLE → 取到 VoiceStick 音频）
  └─ 6. renderer->Start()（写 PCM 到 CABLE Input）

松开主键 / audio_end / 断连 / 取消 / Shutdown
  └─ StopWechatInputMethodSession()
       ├─ hotkey->SendUp()
       ├─ renderer->Stop()（已同步等 drain 完成）
       ├─ SetDefaultCapture(saved_id, {eConsole})  ← 切回，仍只 eConsole
       ├─ saved_id.reset()
       └─ 清状态文件
```

### 4.2 关键不变量

- **只切 eConsole**：`SetDefaultCapture` 调用始终只传 `{DeviceRole::kConsole}`，永不传 `kCommunications`。这是角色分离策略的工程保证。
- **切回在 drain 之后**：`renderer->Stop()` 已同步等 drain（memory `button-up-notify-overtakes-audio-drain` 修复），切回在其后调用，避免尾音丢失。
- **renderer 生命周期不变**：切的是"系统默认设备指向"，`WasapiVirtualMicRenderer` 仍往 CABLE Input 写（CABLE Input 设备本身不变）。`wasapi-renderer-reuse-invariant` 不受影响，但测试须覆盖 Start→切→Stop→切回→再 Start。
- **收敛到 `StopWechatInputMethodSession`**：切回放此函数末尾，断连/取消/Shutdown 路径均经此收敛，四处清理自动覆盖。

## 5. 技术方案

### 5.1 新增组件 `default_audio_device_controller`（voicestick_core）

接口放 core 层可注入 Fake 单测；真实 COM 实现同文件。

```cpp
// desktop/windows/src/default_audio_device_controller.h
namespace voicestick {

enum class DeviceRole { kConsole, kCommunications, kMultimedia };

struct AudioDeviceInfo {
  std::wstring id;            // IMMDevice::GetId() 返回的 endpoint id
  std::wstring friendly_name;  // PKEY_Device_FriendlyName
};

// 默认录音设备切换器：读取/设置 eCapture 端默认设备指向。
class IDefaultAudioDeviceController {
 public:
  virtual ~IDefaultAudioDeviceController() = default;
  // 读取当前 eCapture 默认设备（指定角色）。
  virtual std::optional<AudioDeviceInfo> GetDefaultCapture(DeviceRole role) = 0;
  // 枚举 eCapture ACTIVE 设备，friendly name 子串匹配（大小写不敏感）。
  virtual std::optional<AudioDeviceInfo> FindCaptureByName(
      std::wstring_view name_substring) = 0;
  // 把指定设备设为默认录音设备的指定角色集合。返回是否全部成功。
  virtual bool SetDefaultCapture(const std::wstring& device_id,
                                std::vector<DeviceRole> roles) = 0;
};

class DefaultAudioDeviceController : public IDefaultAudioDeviceController {
  // COM 实现：IMMDeviceEnumerator 读/枚举，IPolicyConfig::SetDefaultEndpoint 写。
};

}  // namespace voicestick
```

### 5.2 IPolicyConfig COM 声明

`IPolicyConfig` 未在 SDK 头文件公开，需手动声明 CLSID/IID 与接口（参考 SoundSwitch `PolicyConfig.h`、社区 `AudioEndPointLibrary`，按 Win10/11 GUID 移植）。关键方法：

```cpp
HRESULT SetDefaultEndpoint(LPCWSTR device_id, ERole role);  // 单角色
```

读取/枚举用公开的 `IMMDeviceEnumerator`（`GetDefaultAudioEndpoint(eCapture, role, &dev)` / `EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll)`），无需 IPolicyConfig。仅"设置"需 IPolicyConfig。

> GUID 与签名以 Win10 1903+（项目最低支持）为准，实现阶段移植时核对。不臆测具体 GUID 值。

### 5.3 协调器接入

构造函数新增注入点（对齐现有 `wechat_renderer_factory` / `wechat_hotkey_factory`）：

```cpp
// voice_stick_coordinator.h
std::function<std::unique_ptr<IDefaultAudioDeviceController>()>
    wechat_device_switcher_factory_;
std::unique_ptr<IDefaultAudioDeviceController> wechat_device_switcher_;
// Start 时记录的 eConsole 原默认设备 ID（有值=当前处于"已切到 CABLE"状态）。
std::optional<std::wstring> saved_default_capture_id_;
```

`StartWechatInputMethodSession` 头部（`hotkey->SendDown()` 之前）插入切换：

```cpp
if (config_.wechat_input_method.auto_switch_default_recording_device) {
  if (!wechat_device_switcher_) {
    wechat_device_switcher_ = wechat_device_switcher_factory_
        ? wechat_device_switcher_factory_()
        : std::make_unique<DefaultAudioDeviceController>();
  }
  auto saved = wechat_device_switcher_->GetDefaultCapture(DeviceRole::kConsole);
  auto cable = wechat_device_switcher_->FindCaptureByName(
      config_.wechat_input_method.virtual_mic_capture_name);
  if (saved && cable &&
      wechat_device_switcher_->SetDefaultCapture(cable->id, {DeviceRole::kConsole})) {
    saved_default_capture_id_ = saved->id;
    PersistDeviceSwitchState(true, saved->id, saved->friendly_name);  // 崩溃自愈用
  } else {
    LogCoordinatorLine("auto_switch failed: saved=" + ... + " cable=" + ...);
    // 不阻断会话：renderer.Start 会自行报错或正常，保持现有错误路径。
  }
}
```

`StopWechatInputMethodSession` 末尾（`renderer->Stop()` 之后）切回：

```cpp
if (saved_default_capture_id_.has_value()) {
  wechat_device_switcher_->SetDefaultCapture(*saved_default_capture_id_,
                                             {DeviceRole::kConsole});
  saved_default_capture_id_.reset();
  PersistDeviceSwitchState(false, {}, {});
}
```

### 5.4 时序（按下到弹框）

当前时序（`voice_stick_coordinator.cc:515-520`）：`SendDown` → `renderer.Start`。插入切换后：

```
button_down → 切 eConsole→CABLE（同步）→ SendDown → renderer.Start
```

- 切换必须在 `SendDown` 之前完成：微信弹框即从默认设备取音，未切好会取到真实麦（漏音/错误源）。
- `IPolicyConfig::SetDefaultEndpoint` 是同步调用，返回时通常已生效，但 Windows 广播设备变更、应用重建流有延迟——**须真机验证按下到弹框延迟增量**（对齐既有文档验证项 3、4，memory `wechat-press-to-popup-latency-optimization`）。
- 若延迟不可接受，退路见第 13 节（预切候选 C，权衡后定）。

### 5.5 切回时序（松开）

```
button_up → StopWechatInputMethodSession
  → hotkey.SendUp → renderer.Stop（同步等 drain）→ SetDefaultCapture(saved, {eConsole})
```

切回严格在 `renderer.Stop` 之后：drain 期间 renderer 仍往 CABLE Input 写，若提前切回，微信取音源错乱、加剧尾音丢失（memory `button-up-notify-overtakes-audio-drain`）。

## 6. 清理分支与残留自愈

依据 memory `wechat-output-mode-disconnect-cleanup`：新增状态须查所有清理分支。切回收敛在 `StopWechatInputMethodSession`，下列路径均经此，自动覆盖：

| 分支 | 触发 | 收敛点 |
|---|---|---|
| 正常结束 | button_up + audio_end | `StopWechatInputMethodSession`（button_up 路径 `coordinator.cc:419` / audio_end 路径 `:477`） |
| 断连 | BLE 断连 | `coordinator.cc:1702` 专用停止路径 → `StopWechatInputMethodSession` |
| 取消识别 | 侧键取消 | `coordinator.cc:1675` → `StopWechatInputMethodSession` |
| 进程退出 | Shutdown | 析构须确保调用 `StopWechatInputMethodSession`（幂等，已切回则 `saved_id` 无值跳过） |

**残留自愈**（进程崩溃未切回）：启动时读状态文件，若 `switched=true` 且当前无活跃会话 → `SetDefaultCapture(saved_id, {eConsole})` + 清文件。saved_id 失效（设备已拔）则 Restore 失败记日志、清文件、不阻断。

## 7. 状态持久化

`%APPDATA%\VoiceStick\default_device_switch_state.json`（便携模式 exe 同级，复用 `AppConfig::ConfigDirectory()`）：

```json
{
  "switched": true,
  "saved_default_capture_id": "{0.0.1.00000000}.{xxx}",
  "saved_default_capture_name": "麦克风(Realtek Audio)",
  "switched_at": "2026-07-08T12:34:56"
}
```

读写放 `voicestick_core`（复用 app_config 序列化范式，可单测）。仅用于崩溃自愈，正常运行期 Start 写、Stop 清。

## 8. 配置与 UI

### 8.1 新增配置项

```toml
[wechat_input_method]
hotkey = "ctrl+win"
virtual_mic_playback_name = "CABLE Input"      # 现有：VoiceStick 写入的播放端
virtual_mic_capture_name = "CABLE Output"      # 新增：切换目标的录音端
auto_switch_default_recording_device = false   # 现有字段，本次启用
```

`WechatInputMethodConfig`（`app_config.h:102`）新增 `std::string virtual_mic_capture_name = "CABLE Output";`。`virtual_mic_playback_name` 与 `virtual_mic_capture_name` 分离，因 VB-CABLE 的播放端/录音端是两个不同 friendly name 的设备，不可相互推断。

### 8.2 设置对话框 UI

`settings_dialog.cc` wechat 区块（现有 `wechat_hotkey_edit_` / `wechat_virtual_mic_edit_` 旁）新增：

- `auto_switch_default_recording_device` 复选框（启用自动切换）。
- `virtual_mic_capture_name` 编辑框（切换目标录音端，默认 "CABLE Output"）。

遵循 memory `windows-dialog-dynamic-layout-pattern` / `windows-ui-layout-declarative-first`：用声明式布局表新增行，预想条件显隐（`auto_switch` 关闭时 capture_name 行可灰显但不隐藏，保持布局稳定）。

## 9. 退化检测与引导

首次启用 `auto_switch`（或开关从 false→true 时）：

1. 读当前 `eConsole` 默认设备 → 若 friendly name 含 `virtual_mic_capture_name`（即默认已是 CABLE Output）→ 提示："自动切换需平时默认录音设备为物理麦克风，当前默认是 CABLE Output，切换将无效果。建议在 Windows 声音设置中把默认设备改为物理麦克风。"
2. 读当前 `eCommunications` 默认设备 → 若非真实麦（含 CABLE/Virtual）→ 建议用户把"默认通信设备"设为真实麦以获得会议零干扰。

引导为提示，不阻断启用。检测逻辑放 `voicestick_core`（可注入 Fake 单测）。

## 10. 测试计划（TDD）

按 PCT 红绿重构，接口注入 Fake，逻辑单测先行；真实 COM 验证留真机。

### 10.1 单测要点（AAA，加 `core_tests.cc` 的 `main()`）

- `FakeDefaultAudioDeviceController`：记录 Get/Set/Find 调用，可注入预设设备列表与默认指向。
- Start 时序：auto_switch=true → 断言先 `GetDefaultCapture(Console)` 记 saved、再 `FindCaptureByName(capture_name)`、再 `SetDefaultCapture(cable, {Console})`，且 `SendDown`/`renderer.Start` 在切换之后。
- 角色分离：断言 `SetDefaultCapture` 的 roles 恒为 `{kConsole}`，不含 `kCommunications`。
- Stop 切回：断言 `renderer.Stop` 之后调 `SetDefaultCapture(saved, {Console})`，saved 清空。
- 切回在 drain 后：Fake renderer 的 Stop 阻塞至 drain 完成，验证切回在其后。
- auto_switch=false：断言无任何 Get/Set 调用，维持现状。
- 四处清理：断言断连/取消/Shutdown 路径都触发切回。
- 残留自愈：状态文件 switched=true → 启动触发 Restore + 清文件；saved_id 失效 → Restore 失败不抛、清文件。
- 退化检测：默认==CABLE Output → 提示触发；eCommunications==CABLE → 建议触发。
- 切换失败不阻断：`SetDefaultCapture` 返回 false → 记日志，会话仍走 SendDown/renderer.Start。

### 10.2 真机验证（单测覆盖不到）

依据 memory `msi-config-verify-real-machine-sha256`：安装落地、时序、清理、自愈须端到端真机走一遍。见第 11 节。

## 11. 真机验证项

| # | 验证项 | 方法 | 关联 memory |
|---|---|---|---|
| 1 | `IPolicyConfig` 在 Win10 1903/Win11 设默认录音延迟 | 真机计时（决定时序是否需预切） | wechat-press-to-popup-latency-optimization |
| 2 | 按下到弹框延迟增量（切设备串在 SendDown 前） | 真机对比 auto_switch on/off | wechat-press-to-popup-latency-optimization |
| 3 | 切回与 drain 时序：松开后尾音是否完整 | 真机录音验证 | button-up-notify-overtakes-audio-drain |
| 4 | 角色分离实测：录音期 Teams/Skype（eCommunications）是否不受影响 | 录音期间开 Teams 通话验证 | — |
| 5 | Zoom/腾讯会议（若用 eConsole）录音期是否被切走、平时是否正常 | 真机验证 | wechat-output-mode-disconnect-cleanup |
| 6 | 四处清理分支：正常/断连/取消/Shutdown 是否都切回 | 模拟各异常路径，核对默认设备还原 | wechat-output-mode-disconnect-cleanup |
| 7 | 残留自愈：模拟崩溃（任务管理器结束进程），重启是否自动 Restore | 真机模拟，核对状态文件与设备 | wechat-output-mode-disconnect-cleanup |
| 8 | 退化场景：平时默认=CABLE Output 时切换是否退化但不报错 | 真机验证 | — |
| 9 | VB-CABLE 未装：FindCaptureByName 失败 → 会话是否优雅降级 | 真机卸载 VB-CABLE 验证 | — |

## 12. 实施阶段（TDD 分步）

每阶段先写失败测试再实现，阶段间可独立提交。

- **阶段 1 · 接口与 Fake**：`IDefaultAudioDeviceController` 接口 + `FakeDefaultAudioDeviceController` + 协调器构造注入点（红→绿）。
- **阶段 2 · 配置**：`virtual_mic_capture_name` 字段 + 序列化 + 模板 + 往返单测（红→绿）。
- **阶段 3 · 切换编排**：Start 切 eConsole→CABLE、Stop 切回，角色分离断言，auto_switch=false 不切（红→绿）。
- **阶段 4 · 清理与自愈**：四处清理覆盖 + 状态文件读写 + 残留自愈（红→绿）。
- **阶段 5 · COM 实现**：`DefaultAudioDeviceController` 真实 IPolicyConfig 实现（真机验证项 1、9）。
- **阶段 6 · UI**：设置对话框勾选框 + capture_name 编辑框（声明式布局）+ 退化检测提示（真机验证项 8）。
- **阶段 7 · 端到端真机验证**：第 11 节全部验证项。

## 13. 风险与权衡

| 风险 | 影响 | 缓解 |
|---|---|---|
| 切换延迟增加按下到弹框时间 | 体验退化 | 真机测延迟；不可接受则改预切（候选 C：进入 wechat 就绪态即预切，模式退出切回，影响窗口从单次录音扩到就绪期，权衡后定） |
| 切回与 drain 竞态 | 尾音丢失 | 切回严格在 `renderer.Stop` 之后 |
| 异常退出未切回 | 系统录音设备永久错乱 | 状态文件 + 残留自愈 + 四处清理收敛 |
| 会议软件用 eConsole（非通信角色） | 录音期被切走 | 角色分离已把通信类降到零；非通信类平时正常、仅录音期受影响（优于现状） |
| 微信输入法实际取音角色未知 | 若用 eCommunications 则只切 eConsole 取不到音 | 真机验证项 4 附带验证微信取音角色；若取不到则退路为全角色切换（见第 14 节） |
| "始终自动记录"退化（平时默认=CABLE） | 切回无效 | 退化检测 + 引导（第 9 节） |
| IPolicyConfig 跨 Windows 版本 GUID 差异 | Win10/11 切换失败 | 移植时按目标版本核对 GUID，失败降级（不阻断会话） |

## 14. 备选方案对比

| 方案 | 会议干扰 | 配置复杂度 | 可靠性 | 取舍 |
|---|---|---|---|---|
| **A. 角色分离（本方案）** | 通信类零干扰，非通信类仅录音期 | 低（零配置自动记录） | 中（依赖微信用 eConsole） | 推荐：干扰最小化，渐进可退 |
| B. 全角色切换 | 录音期全部受影响 | 低 | 高（不依赖微信取音角色） | 若真机验证微信不用 eConsole 则退此 |
| C. 静态设备分离（不动态切） | 通信类零干扰 | 高（需用户分别设默认/通信设备） | 高 | 一次性配置，但非技术用户门槛高，与"自动"目标相悖 |
| D. 现状（不切，默认常驻 CABLE） | 全程干扰 | — | — | 基线，本方案要改善的痛点 |

若真机验证项 4 发现微信输入法实际从 `eCommunications` 取音（而非预期 `eConsole`），则角色分离方案 A 失效，退路为方案 B（全角色切换）。决策依据为真机证据，不臆测。

## 15. 参考

- `Doc/Plan/windows-vbcable-as-is-bundling.md` 第 7–8 节：既有切换框架（本方案独立化来源）
- `Doc/Plan/wechat-input-method-voice-integration.md`：wechat 模式原始方案
- `Doc/Plan/wechat-press-to-popup-latency-optimization.md`：按下到弹框延迟基线
- `Doc/Plan/wechat-virtual-mic-event-driven-renderer.md`：当前渲染器实现
- memory `wechat-output-mode-disconnect-cleanup`：清理分支义务
- memory `button-up-notify-overtakes-audio-drain`：切回须在 drain 后
- memory `wasapi-renderer-reuse-invariant`：renderer 复用不变量
- memory `wechat-press-to-popup-latency-optimization`：延迟敏感指标
- [SoundSwitch PolicyConfig.h（参考实现）](https://github.com/Belphemur/SoundSwitch)
