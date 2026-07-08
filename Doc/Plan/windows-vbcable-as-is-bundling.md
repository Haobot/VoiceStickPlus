# Windows 端随包携带 VB-CABLE 的 AS IS 合规分发流程

## 1. 背景与目标

### 1.1 背景

当前 `wechat_input_method` 输出模式依赖第三方 VB-CABLE 虚拟音频驱动：VoiceStick 把 Opus 解码为 PCM 后经 WASAPI 写入 CABLE Input，微信输入法从 CABLE Output（系统默认录音设备）取音。完整方案见 `Doc/Plan/wechat-input-method-voice-integration.md`。

用户痛点：**需额外安装 VB-CABLE 并手动把 CABLE Output 设为默认录音设备**，极不方便。尤其"手动切默认设备"一步，非技术用户几乎无法独立完成。

### 1.2 目标

把"用户手动装驱动 + 手动切默认设备"自动化为 VoiceStick 首启自动完成，消除全部手动步骤。

### 1.3 合规前提（必须先读）

VB-CABLE 是 VB-Audio 的 **donationware（捐赠软件）**，非 VoiceStick 组件。VB-Audio 官方对"分销商/集成商/商业分发"要求走《一般使用条款》第 3.4 节单独授权（见各商店页脚 "For distributors and integrators please read the specific conditions in section 3.4 of the general terms of use"）。

本方案当前**仅适用于小范围技术内测**，合规边界如下：

- 不收费、不公开发布、内测用户数量可控
- VB-CABLE 原始安装包**原样携带，不做任何修改**（AS IS, without any modification）
- 保留 VB-Audio 全部版权与许可声明
- 首启明确告知用户 VB-CABLE 为第三方捐赠软件，用户可选择跳过

> ⚠️ **退出条件**：内测结束、准备公开发布时，必须改走 B2-下载（首启从 vb-cable.com 官方源拉取）或联系 VB-Audio 获取商业分发授权。**不得将"随包携带 zip"模式直接用于公开发布。** 本文件第 14 节列出的退出检查未全部完成前，禁止公开发布。

## 2. 现状与时序基线

### 2.1 现有音频链路（已实现，一行不改）

```text
BLE audio_tx → Coordinator.HandleAudioFrame → OpusDecoder → PcmRingBuffer
  → WasapiVirtualMicRenderer → CABLE Input (WASAPI render)
  → [Windows 音频栈] → CABLE Output (系统默认录音设备)
  → 微信输入法取音 → 云端 ASR → 上屏
```

`WasapiVirtualMicRenderer`（`wasapi_virtual_mic_renderer.cc`）已实现向指定播放设备名（`config_.wechat_input_method.virtual_mic_playback_name`，coordinator.cc:499）渲染 PCM。**本方案不改渲染层**，只补"自动装驱动 + 自动切默认设备"两段。

### 2.2 现有 wechat 按下时序（临时切必须对齐）

`StartWechatInputMethodSession`（`voice_stick_coordinator.cc:511-545`）当前时序：

| 步 | 动作 | 代码位置 |
|---|---|---|
| ⑦ | `renderer->Start()`（WASAPI 开设备+起线程，几十 ms 同步阻塞） | coordinator.cc:511 |
| ⑧ | `hotkey_->SendDown()`（SendInput 注入 Ctrl+Win，触发微信弹框） | coordinator.cc:519 |

依据 `Doc/Plan/wechat-press-to-popup-latency-optimization.md`，按下到弹框延迟已是敏感指标（固件段 380~780ms + 桌面段几十 ms）。**临时切设备若串在 SendDown 之前，会直接增加按下到弹框延迟**——这是本方案最关键的待验证点（见第 8 节）。

### 2.3 现有 config.template 首启范式（携带流程对齐）

VoiceStick v1.9.0 已有成熟的"MSI 装到 ProgramFiles + 首启复制到 %APPDATA%"机制（见 memory: msi-config-template-seed-mechanism）：

- CMake POST_BUILD 把 `config.template.toml` 复制到 build 目录（`CMakeLists.txt:153-157`）
- WiX 装入 `[INSTALLFOLDER]`（`installer/VoiceStick.wxs:41-43`）
- `AppConfig::SeedConfigFromTemplate`（`app_config.cc:263-275`）首启从 exe 同级复制到 `%APPDATA%\VoiceStick\config.toml`，已有不覆盖
- 便携模式直接读 exe 目录

VB-CABLE 携带流程对齐此范式，但**本质差异**：config.template 是文件复制（不需提权），VB-CABLE 是内核驱动安装（必须提权 + 重启）。

## 3. 总体架构与数据流

```text
官方源 vb-cable.com
   │  VBCABLE_Driver_Pack45.zip（官方原装，~1.3MB，WHQL 签名）
   ▼  下载一次，固化入仓
third_party/vbcable/
   ├─ VBCABLE_Driver_Pack45.zip   （原始包，不解压不修改）
   └─ vbcable.sha256               （SHA256 + 版本 + 官方 URL + 下载日期）
   ▼  CMake 构建时校验 SHA256 → POST_BUILD 复制（对齐 config.template 范式）
build-x64/vbcable/VBCABLE_Driver_Pack45.zip
   ▼  WiX 装入 [INSTALLFOLDER]vbcable\  /  package-portable.bat 带入便携包
ProgramFiles\VoiceStick\vbcable\VBCABLE_Driver_Pack45.zip  （AS IS）
   ▼  首启：校验 SHA256 → 检测未装 → 告知 → 用户同意 → runas 提权
VBCABLE_Setup_x64.exe（官方安装器原样运行，不二次封装不二次签名）
   ▼  静默装驱动 → 提示重启（VB-Audio 强制）
Windows 音频栈：CABLE Output（虚拟录音设备）
   ▼  录音期间临时切默认设备（见第 8 节）
现有 WasapiVirtualMicRenderer → CABLE Input（一行不改）
   ▼
微信输入法从默认录音设备取音
```

**AS IS 核心保证**：从仓库到用户机器，zip 始终是同一字节序列，SHA256 贯穿全程校验。VoiceStick 只做便利层（检测/装/切设备），**不碰包内容**。

## 4. AS IS 完整性保护（合规核心）

| 环节 | 机制 | 防的风险 |
|---|---|---|
| 入仓 | 下载官方 zip + 记录 SHA256 到 `vbcable.sha256` | 固化官方发布版本 |
| 构建 | CMake `file(SHA256)` 校验仓库 zip == 清单，不符即失败 | 防任何人误改 zip |
| 打包 | POST_BUILD `copy_if_different` 原 zip（不解压、不重打包） | 防构建流程篡改 |
| 安装 | WiX `<File Source=...zip>` 原样装入 ProgramFiles | 防 MSI 压缩篡改 |
| 运行 | 启动时校验 ProgramFiles 下 zip SHA256 == 入仓清单，不符拒绝安装 | 防运行时被替换 |
| 安装器 | `ShellExecute(runas)` 官方 `VBCABLE_Setup_x64.exe`，**不二次签名、不封装** | 保官方 WHQL 签名有效 |

任一环节改动 zip，SHA256 校验中断流程——这是 "without any modification" 的**工程证据**而非口头声明。

### 4.1 SHA256 校验设计

`vbcable.sha256` 清单格式（人可读 + 机器可校验）：

```text
version=VBCABLE_Driver_Pack45
sha256=<64 位十六进制>
official_url=https://vb-audio.com/Services/Cable.htm
downloaded_at=2026-07-08
notes=官方原装，AS IS 携带，禁止修改。见 Doc/Plan/windows-vbcable-as-is-bundling.md
```

校验逻辑放 `voicestick_core`（可单测）：输入 zip 路径 + 清单路径 → 比对 SHA256 → 返回 `kOk / kHashMismatch / kFileMissing`。运行时（首启）与构建时（CMake）复用同一份校验实现。

## 5. 构建与打包集成

### 5.1 CMake 集成（`desktop/windows/CMakeLists.txt`）

在现有 config.template POST_BUILD（CMakeLists.txt:153-157）后新增：

```cmake
# VB-CABLE 原始包随包携带（AS IS，不修改）。构建时校验 SHA256 确保与入仓一致。
# 详见 Doc/Plan/windows-vbcable-as-is-bundling.md。内测阶段专用，公开发布前须改走下载模式。
set(VBCABLE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vbcable")
set(VBCABLE_ZIP "${VBCABLE_SRC_DIR}/VBCABLE_Driver_Pack45.zip")
set(VBCABLE_MANIFEST "${VBCABLE_SRC_DIR}/vbcable.sha256")

# 构建时校验 SHA256（file(SHA256) 读清单期望值 + 实算 zip 比对，不符报错）
# TODO: 实现校验 cmake 脚本，不符即 message(FATAL_ERROR)

add_custom_command(TARGET VoiceStickApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VBCABLE_ZIP}"
            "$<TARGET_FILE_DIR:VoiceStickApp>/vbcable/VBCABLE_Driver_Pack45.zip"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VBCABLE_MANIFEST}"
            "$<TARGET_FILE_DIR:VoiceStickApp>/vbcable/vbcable.sha256"
)
```

### 5.2 WiX 集成（`installer/VoiceStick.wxs`）

在 `INSTALLFOLDER` 下新增 `vbcable` 子目录 Component（对齐 `ConfigTemplate` 组件，VoiceStick.wxs:41-43）：

```xml
<Directory Id="INSTALLFOLDER" Name="VoiceStick">
  <!-- ...现有 Component... -->
  <Directory Id="VbcableDir" Name="vbcable">
    <Component Id="VbcablePackage" Guid="*" Bitness="always64">
      <File Id="VbcableZipFile"
            Source="$(var.BuildDir)\vbcable\VBCABLE_Driver_Pack45.zip" KeyPath="yes" />
      <File Id="VbcableManifestFile"
            Source="$(var.BuildDir)\vbcable\vbcable.sha256" />
    </Component>
  </Directory>
</Directory>
<!-- Feature Main 增 <ComponentRef Id="VbcablePackage" /> -->
```

### 5.3 便携版（`scripts/package-portable.bat`）

便携包直接把 `build-x64/vbcable/` 带入便携目录。便携模式首启从 exe 同级 `vbcable\` 读取（对齐 config.template 便携读取范式）。

## 6. 首启检测与提权安装

### 6.1 检测 VB-CABLE 是否已装

枚举音频设备找 "CABLE Output"（`IMMDeviceEnumerator` + `IMMDeviceCollection`，按 friendly name 子串匹配）。检测逻辑放 `voicestick_core`（枚举部分可注入 Fake 单测）。

> 不用注册表检测（`HKLM\SYSTEM\CurrentControlSet\Enum\SWD\VB-Audio`），因为音频设备枚举更能反映"用户可用"状态（驱动装了但设备被禁用也算未就绪）。

### 6.2 告知与同意（首启 UI）

未装时弹窗（沿用 onboarding/settings 对话框范式）：

```
VoiceStick 微信输入法模式需要 VB-CABLE 虚拟音频驱动。
VB-CABLE 是 VB-Audio 的第三方捐赠软件，版权归原作者所有，非 VoiceStick 组件。
VoiceStick 将原样（不修改）安装官方 VB-CABLE 驱动，并需要管理员权限。

[安装 VB-CABLE]    [跳过（微信输入法模式将不可用）]
```

跳过 → wechat 模式标记不可用，配置里给出提示；安装 → 进 6.3。

### 6.3 提权安装（runas）

VoiceStick 应用清单为 asInvoker（见 memory: windows-uipi-weixin-injection，2026-07-06 撤销 requireAdministrator）。asInvoker 进程无权装内核驱动，必须弹 UAC 提权：

- `ShellExecute(NULL, L"runas", setup_exe_path, silent_args, ...)` 启动官方 `VBCABLE_Setup_x64.exe`
- **不二次封装、不二次签名**——直接跑官方安装器，保 WHQL 签名
- 静默参数：查到 VBCABLE_Setup 支持静默部署 INF 驱动，但**确切参数待真机验证**（见第 12 节）。无静默参数则走官方交互 UI（内测可接受）
- 安装完成 → 提示重启（VB-Audio 官方强制要求，`VBCABLE_Driver_Pack45` 安装后必须重启）
- 重启后再次启动 VoiceStick → 检测已装 → 进第 8 节临时切逻辑

### 6.4 安装编排归属

| 职责 | 归属 | 理由 |
|---|---|---|
| SHA256 校验 | `voicestick_core` | 可单测 |
| VB-Cable 已装检测（设备枚举） | `voicestick_core` | 逻辑可注入 Fake 单测 |
| runas 启动安装器 / 告知 UI / 重启提示 | `VoiceStickApp` | 平台 UI |
| 首启编排（检测→告知→提权→装→切设备） | `VoiceStickApp` | 平台胶水 |

## 7. 临时切设备策略概述

**决策**：录音期间临时切默认设备为 CABLE Output，松开/drain 完成后切回原设备。

理由：永久切默认会持续影响会议软件等依赖默认设备的应用（`Doc/Plan/wechat-input-method-voice-integration.md:43-47` 已识别此权衡）。临时切把影响窗口缩到录音期间。

代价与风险见第 8 节，**必须真机验证延迟后定最终时序**。

## 8. 默认设备临时切换（重点章节）

### 8.1 切换 API

`IPolicyConfig` COM 接口（未文档化但广泛使用，见 memory 调研：AudioEndPointLibrary / com-policy-config）。封装为 `voicestick_core` 中的 `DefaultAudioDeviceController`：

```cpp
// 设备角色
enum class DeviceRole { Console, Communications, Multimedia };

class DefaultAudioDeviceController {
 public:
  // 把指定设备设为默认录音设备（eCapture），可指定角色。
  // 返回切换前的原默认设备 ID（供 Restore 用）。
  virtual std::optional<std::wstring> SetDefaultCapture(
      const std::wstring& device_id_substring,
      std::vector<DeviceRole> roles) = 0;

  // 还原为指定设备 ID。
  virtual bool RestoreDefaultCapture(
      const std::wstring& saved_device_id,
      std::vector<DeviceRole> roles) = 0;

  // 枚举录音设备，返回 {id, friendly_name} 列表（供匹配 CABLE Output）。
  virtual std::vector<AudioDeviceInfo> EnumerateCaptureDevices() = 0;
};
```

实现参考 `com-policy-config`（Rust）与 `AudioEndPointLibrary`（C#）的 `PolicyConfig.h` 手工移植。放 core 层，枚举/匹配逻辑可注入 Fake 单测；真实 COM 调用放平台实现。

### 8.2 状态持久化

原默认设备 ID 存 `%APPDATA%\VoiceStick\vbcable_state.json`（MSI 模式）/ exe 同级（便携模式）：

```json
{
  "original_default_capture_id": "{0.0.1.00000000}.{xxx}",
  "original_default_capture_name": "麦克风(Realtek Audio)",
  "switched": true,
  "switched_at": "2026-07-08T12:34:56"
}
```

### 8.3 时序约束（核心）

临时切必须满足：**设备切换完成 → SendDown 触发微信取音**。若微信在设备切换完成前开始取音，会取到原设备音频（漏音/错误源）。

当前 wechat 按下时序（第 2.2 节）插入临时切后，候选时序：

**候选 A：串行切（最安全，延迟最大）**
```
button_down → 切设备(同步等完成) → renderer.Start → SendDown
```
- 风险：IPolicyConfig 切换延迟（待测，可能 50ms+）直接推后 SendDown
- 与 memory 教训冲突：`wechat-press-to-popup-latency-optimization` 指出延迟已是敏感指标

**候选 B：并行切（有竞态）**
```
button_down → 异步发起切设备 + 立即 renderer.Start + SendDown
```
- 风险：微信可能在设备切完前取音，前几帧取到原设备
- 可缓解：微信面板有缓冲，前几十 ms 丢帧可能不影响识别（待真机验证）

**候选 C：预切 + 快速切回**
```
进入 wechat 就绪态(配对/模式激活) → 预切默认设备 → 按下即录音 → 松开 → 不立即切回，模式退出时切回
```
- 风险：接近"永久切"，影响窗口从单次录音扩大到模式激活期间
- 折中：影响仍小于永久切

> ⚠️ **候选时序须真机验证 IPolicyConfig 延迟后定**。无真机数据前不锁定时序（disciplined-execution：不臆测）。文档先给框架，决策留给第 12 节验证项。

### 8.4 切回时序与 button_up 抢跑风险

松开时序当前：`button_up` → `StopWechatInputMethodSession`（renderer.Stop + hotkey.SendUp）→ 等 `audio_end`。

依据 memory `button-up-notify-overtakes-audio-drain`：button_up 走 state_tx notify 可能先于 drain 帧到达桌面端，导致会话提前结束丢尾音。**临时切回若发生在 drain 完成前，会进一步加剧尾音丢失**（renderer 还在写 CABLE Input，但默认设备已切回原设备，微信取音源错乱）。

设计约束：**切回必须在 renderer.Stop 完成（drain 结束）之后**。`StopWechatInputMethodSession` 已同步等 drain（见 memory 修复），在此之后调 `RestoreDefaultCapture`。

### 8.5 异常清理义务（必须全覆盖）

依据 memory `wechat-output-mode-disconnect-cleanup`：新增模式/状态须查所有清理分支。临时切引入"默认设备被改"状态，**任一异常退出路径都必须切回**，否则用户系统录音设备永久错乱。须覆盖四处：

| 分支 | 触发 | 清理动作 |
|---|---|---|
| 正常结束 | button_up + audio_end | drain 完成后 RestoreDefaultCapture |
| 断连 | BLE 断连 | `StopWechatInputMethodSession`（coordinator.cc:1700 已有专用停止路径）须补 Restore |
| 取消识别 | 侧键取消 | 取消路径须补 Restore |
| 进程退出/Shutdown | 应用关闭 | 析构须补 Restore（哪怕会话已结束，幂等无害） |

残留自愈：若上次 button_up/audio_end 都丢（voice_stick_coordinator.cc:390 已识别的 wechat_active 残留场景），启动时检测 `vbcable_state.json` 的 `switched=true` 且无活跃会话 → 主动 Restore。

### 8.6 renderer 复用不变量

依据 memory `wasapi-renderer-reuse-invariant`：`WasapiVirtualMicRenderer` 在 session 间复用，Stop 不能 `sink_.reset()`。临时切设备不改 renderer 生命周期——切的是"系统默认设备指向"，renderer 仍往 CABLE Input 写（CABLE Input 设备本身不变）。此不变量不受临时切影响，但测试须覆盖 Start→切设备→Stop→切回→再 Start 全链路。

## 9. 卸载与异常清理

### 9.1 VoiceStick 卸载（WiX 自定义动作）

- 读 `vbcable_state.json` → 若 `switched=true` → `RestoreDefaultCapture` 还原原默认设备
- **不卸载 VB-CABLE 驱动**：用户可能装了其他依赖 VB-CABLE 的软件，VoiceStick 卸载不应连带卸载第三方驱动。用户可用官方 `VBCABLE_Setup_x64.exe` 自行卸载

### 9.2 异常进程退出

`Win32App` 析构（Shutdown 路径）须补 RestoreDefaultCapture，幂等。

### 9.3 残留检测

VoiceStick 启动时读 `vbcable_state.json`，若 `switched=true` 但当前无活跃 wechat 会话（上次异常退出未切回）→ 主动 Restore 并清状态文件。这覆盖 memory 记录的"button_up 丢失致状态残留"类问题。

## 10. 代码归属与测试策略

### 10.1 归属

| 模块 | 归属 | 测试 |
|---|---|---|
| SHA256 校验 | `voicestick_core`（新 `vbcable_integrity.cc`） | ✅ 单测（正确/篡改/缺失） |
| VB-Cable 已装检测（设备枚举） | `voicestick_core`（注入 Fake enumerator） | ✅ 单测 |
| 默认设备切换 `DefaultAudioDeviceController` | `voicestick_core` 接口 + 平台实现 | ✅ 逻辑单测（Fake）/真机验证 COM |
| runas 启动安装器 / 告知 UI / 重启提示 | `VoiceStickApp` | 真机验证 |
| 首启编排 / 临时切编排 / 清理分支 | `VoiceStickApp` + coordinator 协作 | 真机验证 |
| `vbcable_state.json` 读写 | `voicestick_core`（复用 app_config 序列化范式） | ✅ 单测 |

### 10.2 单测要点（AAA 模式）

- SHA256 校验：正确包通过 / 篡改一字节失败 / 文件缺失返回对应枚举
- 设备枚举：Fake enumerator 返回含 "CABLE Output" → 检测已装；不含 → 未装
- 设备切换：Fake controller 记录 Set/Restore 调用 → 断言切到 CABLE Output、切回原 ID
- 状态文件：写入 switched=true → 重启读取 → 触发残留自愈 Restore

### 10.3 真机验证（单测覆盖不到）

依据 memory `msi-config-verify-real-machine-sha256`：安装落地、首启触发、不覆盖语义、AS IS 完整性须端到端真机走一遍，用 SHA256 核对而非肉眼对比。本方案的真机验证项见第 12 节。

## 11. 合规护栏清单（内测特化）

- [ ] 原始 zip SHA256 贯穿入仓 → 构建 → 运行校验
- [ ] 不解压、不重打包、不剥离文件、不二次签名
- [ ] 首启告知用户 VB-Cable 为第三方捐赠软件 + 著作权归属 + 可跳过
- [ ] 内测用户清单 + 数量控制，不公开发布
- [ ] 保留官方原始版权/许可文件（zip 内自带，不剥离）
- [ ] `vbcable.sha256` 记录官方 URL + 版本 + 下载日期，可溯源

## 12. 待真机验证项（不凭感觉）

| # | 验证项 | 方法 | 关联 memory |
|---|---|---|---|
| 1 | VBCABLE_Setup_x64.exe 静默安装参数 | 真机跑 `/?` 查参数，或 ProcMon 监控 | — |
| 2 | 安装后重启是否强制 | 真机装一次观察 | — |
| 3 | IPolicyConfig 在 Win11 24H2 设默认录音延迟 | 真机计时（决定第 8.3 候选时序） | wechat-press-to-popup-latency-optimization |
| 4 | 候选 A/B/C 时序下按下到弹框延迟 | 真机对比，选延迟与竞态可接受者 | wechat-press-to-popup-latency-optimization |
| 5 | 临时切回与 drain 的时序：button_up 后切回是否丢尾音 | 真机录音验证尾音完整 | button-up-notify-overtakes-audio-drain |
| 6 | 四处清理分支：正常/断连/取消/Shutdown 是否都切回 | 真机模拟各异常路径，核对默认设备还原 | wechat-output-mode-disconnect-cleanup |
| 7 | 残留自愈：模拟 button_up 丢失，重启是否自动 Restore | 真机模拟，核对状态文件与设备 | wechat-output-mode-disconnect-cleanup |
| 8 | 端到端：首启装 → 重启 → 切设备 → 微信取音 → 卸载还原 | 全链路 SHA256 核对 + 设备状态核对 | msi-config-verify-real-machine-sha256 |
| 9 | 默认设备临时切对会议软件影响 | 录音期间开会议软件验证 | wechat-input-method-voice-integration |

## 13. 实施计划（TDD 分阶段）

按 PCT 工作流，每阶段先写失败测试再实现。阶段间可独立提交。

- **阶段 0 · 固化入仓**：下载官方 zip + 生成 `vbcable.sha256`，入仓 `third_party/vbcable/`
- **阶段 1 · 完整性校验**：`voicestick_core` 新增 SHA256 校验（红→绿），CMake 构建时校验集成
- **阶段 2 · 构建打包**：CMake POST_BUILD + WiX Component + 便携版带入（验证 zip 进包且 SHA256 一致）
- **阶段 3 · 已装检测**：`voicestick_core` 设备枚举检测（红→绿）
- **阶段 4 · 首启安装编排**：告知 UI + runas 提权 + 安装 + 重启提示（真机验证项 1、2）
- **阶段 5 · 默认设备切换器**：`DefaultAudioDeviceController` 接口 + COM 实现 + 状态文件（红→绿）
- **阶段 6 · 临时切编排**：接入 coordinator wechat 时序，四处清理分支 + 残留自愈（真机验证项 3-7）
- **阶段 7 · 卸载还原**：WiX 自定义动作 Restore（真机验证项 8）
- **阶段 8 · 端到端真机验证**：第 12 节全部验证项

## 14. 风险与退出条件

### 14.1 已识别风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 临时切延迟增加按下到弹框时间 | 体验退化 | 真机测延迟，必要时改预切(候选 C) |
| 切回时序与 drain 竞态 | 尾音丢失 | 切回严格在 renderer.Stop 后 |
| 异常退出未切回 | 用户系统录音设备永久错乱 | 四处清理 + 残留自愈 |
| 静默安装参数不明 | 首启弹官方 UI | 真机查参数，无则内测接受交互 UI |
| 默认设备临时切影响会议软件 | 录音期间会议软件取不到物理麦 | 已识别权衡，文档告知 |
| VB-Audio 条款 3.4 节未取到一手原文 | 公开发布合规风险 | 内测限定，公开发布前必须取原文或改走下载 |

### 14.2 公开发布退出条件（全部满足才可发布）

- [ ] 第 12 节 9 项真机验证全部通过
- [ ] VB-Audio《一般使用条款》第 3.4 节原文已取得并确认允许 AS IS 携带，**或**改走 B2-下载（首启从官方源拉取）
- [ ] 内测用户反馈收敛，无设备错乱类严重问题
- [ ] 若走携带模式公开发布，须有 VB-Audio 书面授权

> 在以上条件全部满足前，本方案仅限内测范围使用。

## 15. 参考

- `Doc/Plan/wechat-input-method-voice-integration.md`：wechat 模式原始方案，2.3 节明确虚拟麦克风需内核驱动
- `Doc/Plan/wechat-press-to-popup-latency-optimization.md`：按下到弹框延迟基线，临时切须对齐
- `Doc/Plan/wechat-doubleclick-enter-and-stale-recovery.md`：wechat_active 残留自愈范式
- memory `msi-config-template-seed-mechanism`：首启种子范式
- memory `msi-config-verify-real-machine-sha256`：真机 SHA256 核对方法
- memory `wechat-output-mode-disconnect-cleanup`：新增模式清理分支义务
- memory `button-up-notify-overtakes-audio-drain`：button_up 抢跑 drain
- memory `wasapi-renderer-reuse-invariant`：renderer 复用不变量
- memory `windows-uipi-weixin-injection`：asInvoker 与提权
- [Microsoft Learn:音频驱动程序示例（SYSVAD/MSVAD）](https://learn.microsoft.com/zh-cn/windows-hardware/drivers/audio/sample-audio-drivers)
- [VB-Audio 商店条款页（含 3.4 节引用）](https://shop.vb-audio.com/en/content/3-Terms-of-use)
