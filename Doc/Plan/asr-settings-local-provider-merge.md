# 设置页「语音识别」合并本地识别为服务提供方选项

日期：2026-09-09 · 分支：`feat/voice-recognition-option` · 前置：`Doc/Plan/local-mic-mode.md`

## 背景与目标

现状设置页有两处入口管本地识别：「语音识别」区的服务提供方下拉框（云端 Volcengine / Tencent，仅老配置临时显示 VoiceStick Cloud）与独立「本机麦克风」分区（开发者模式可见：开关 / 模型目录 / 按住说话热键）。入口分散、且本机麦克风被埋在开发者模式里。

目标（用户需求原文归纳）：

1. 删除「本机麦克风」分区，把**本地语音识别**并入「语音识别」区的服务提供方下拉框。
2. 选中本地语音识别时才显示**模型目录**行（含浏览与有效性回显）。
3. 原「本机麦克风」的**按住说话热键**并入托盘快捷设置已有的「热键」子菜单。

## 关键设计决策

### D1：本地项是下拉框虚拟项，不新增 `AsrProvider` 枚举值

`AsrProvider` 驱动**设备音频的云端 ASR 客户端选择**（`make_asr` / `ActiveApiKey` / `ActiveWebsocketUrl`），本地识别走的是另一条链路（WASAPI 采集 + SenseVoice，`[local_asr].enabled` 门控），两者本就正交。给枚举加 `kLocal` 会让设备链路拿到一个无法创建客户端的提供方，破坏现有行为。

因此：

- 下拉框末位新增「本地语音识别（离线）」虚拟项，条目序 = `[VoiceStick Cloud(仅老配置)] + Volcengine + Tencent + 本地`。
- 保存映射：选中本地 ⇒ `local_asr.enabled = true`，`asr_provider` **保留原云端值**（设备会话继续云端识别，语义与现状一致）；选中云端某项 ⇒ `local_asr.enabled = false`，`asr_provider = 该项`。
- config.toml **无 schema 变化**，旧配置零迁移。

### D2：模型目录行不再受开发者模式门槛

本地识别成为一等提供方选项后，普通用户选中即可见模型目录行 + ✓/✗ 回显（需求原文“当用户选择本地语音识别，才显示模型目录选项”）。API Key / 资源 ID 行的显隐条件从 `developer_mode_` 改为 `developer_mode_ && 未选本地`（选中本地时两行无意义）。

### D3：按住说话热键移到托盘「热键」子菜单

托盘菜单「热键」子菜单（启用开关 + 三个预设 + 自定义）末尾新增「按住说话热键」项，常驻显示当前键名。点击打开 `HotkeySettingsDialog` 新增的 **PTT 模式**：单键捕获（`require_modifier=false, allow_modifier_as_key=true`，与原设置页录入同一口径），确认后写 `local_asr.push_to_talk_key` 并经 `SaveInputOptions()` 热更（内部已含 `ApplyUpdatedConfig → SyncLocalMicRuntime`，钩子即时重装）。全局热键模式行为不变（默认模式）。

设置页原 PTT 热键行、录入定时器（UIPI 提示）随分区一并删除，UIPI 提示逻辑由 HotkeySettingsDialog 既有同款机制承接。

### D4：下拉框索引映射抽纯函数 `provider_combo`

`ProviderAtComboIndex` / `ComboIndexForProvider` 原是 SettingsDialog 私有方法，加虚拟项后逻辑变复杂且不可单测。抽为 `voicestick_core` 自由函数（`src/provider_combo.h/cc`），TDD 覆盖：

- `ProviderComboCloudCount(has_cloud)` / `ProviderComboLocalIndex(has_cloud)`：本地项固定末位。
- `ProviderComboIsLocal(index, has_cloud)`：是否选中本地项。
- `ProviderComboCloudAt(index, has_cloud)` / `ProviderComboCloudIndexOf(provider, has_cloud)`：云端段映射。

onboarding 向导下拉框保持仅云端项，不动。

## 改动清单

| 文件 | 改动 |
|---|---|
| `src/provider_combo.h/cc`（新） | 下拉框索引映射纯函数，入 `voicestick_core` |
| `src/settings_dialog.h/cc` | 删本机麦克风分区与 PTT 热键录入；下拉框加本地项；API Key/资源 ID/模型目录行显隐重写；保存映射按 D1 |
| `src/hotkey_settings_dialog.h/cc` | 加 `Mode::kPushToTalk`（标题/提示/捕获选项/校验/文案模式化） |
| `src/win32_app.cc` | 托盘热键子菜单加「按住说话热键」项 + 处理器 |
| `src/localization.h/cc` | 增 `kSettingsProviderLocal`、`kMenuLocalMicHotkey`；删 `kSettingsSectionLocalMic`、`kSettingsLocalMicEnable` |
| `tests/core_tests.cc` | `TestProviderComboMapping`（先红后绿） |
| `Doc/Ref/desktop-config.md` | `[local_asr]` 段设置入口描述更新 |

## 边界与不做

- 设备（StickS3/小米）音频仍走云端 ASR：本地识别暂只服务本机麦克风按住说话链路（设备音频本地识别是另一特性，另行立项）。
- onboarding 向导不提供本地项；`NeedsAsrStep` 逻辑不变。
- 热词区块在选中本地时仍显示（设备云端链路继续消费热词）。

## 验收

1. `ctest` 全绿（新增映射测试 + 既有回归）。
2. 构建通过并重启 VoiceStick.exe：设置页无「本机麦克风」分区；选「本地语音识别」→ 模型目录行出现、API Key/资源 ID 行隐藏；切回云端项反之。
3. 托盘「热键 → 按住说话热键」可录入单键（如 right ctrl），保存后热键即时生效（选中本地时按住说话可用）。
4. 旧配置（`local_asr.enabled=true`）打开设置页，下拉框正确显示「本地语音识别」。
