# 设置/引导界面精简设计（2026-08-01）

## 目标

精简 Windows 设置对话框与首次启动引导（onboarding）的界面：

1. 语音识别区：隐藏 API Key 与资源 ID 字段；服务商下拉框去掉 VoiceStick Cloud。
2. 文本精修区：隐藏 Base URL、API Key、模型名称三个字段。
3. 系统区：隐藏「Windows 启动时自动运行 VoiceStick」与「划词添加热词」两项（与托盘右键菜单功能重复，托盘菜单保留）。

## 原则

- 隐藏而非删除：控件照常创建、照常从 config 加载/保存，只是不 `add()` 进布局表（沿用现有 `kShowAdvancedSettings` 模式）。`config.toml` 手改依然生效，保存时隐藏字段按加载值原样回写。
- 保留 VoiceStick Cloud 底层能力：`AsrProvider::kVoiceStickCloud` 枚举、config 解析、Cloud API 客户端均不动；老用户 `asr_provider = voicestick_cloud` 的配置继续可用。
- 托盘右键菜单的开机自启、划词添加热词两项功能不变。

## 改动点

### `desktop/windows/src/settings_dialog.cc`

- 服务商下拉框默认只加 Volcengine（索引 0）、Tencent Cloud ASR（索引 1）。
- 新增成员 `provider_combo_has_cloud_`：当 `config_.asr_provider == kVoiceStickCloud` 时在索引 0 临时插入 "VoiceStick Cloud"，老用户打开设置仍看到并选中 Cloud。
- 新增集中映射辅助函数 `ProviderAtComboIndex(int)` / `ComboIndexForProvider(AsrProvider)`，替换散落的硬编码索引（`LoadConfigIntoControls`、`SaveConfigFromControls`、`ApplyApiKeyLayout`、CBN_SELCHANGE 处理）。
- 以下行不再 `add()` 进布局：API Key 行（含「申请试用」按钮）、资源 ID 行、LLM Base URL / API Key / 模型三行、系统区标题、开机自启行、划词热词行。控件创建与 config 读写代码保留。
- 系统区其余行本就被 `kShowAdvancedSettings` 隐藏，两项再隐藏后该区为空，故「系统」分区标题一并隐藏。

### `desktop/windows/src/onboarding_dialog.cc`

- 服务商下拉框同样只留 Volcengine / 腾讯；`config_.asr_provider == kVoiceStickCloud` 时同样临时插入 Cloud 项。
- 索引映射逻辑与设置页一致。

### `desktop/windows/src/app_config.h`

- `asr_provider` 默认值由 `kVoiceStickCloud` 改为 `kVolcengine`（app_config.h:135）。

## 不做的事

- 不删除 `AsrProvider::kVoiceStickCloud` 枚举、`voicestick_api_key` 等 config 字段、`voice_stick_cloud_api_win.cc`。
- 不动托盘菜单、不改 config.toml 格式、不改 macOS 端。
- 不删除本地化字符串（未使用的保留，避免无关 diff）。

## 验证

- `build_win.bat` 构建通过；核对 `desktop/windows/build-x64/VoiceStick.exe` 时间戳。
- `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 核心测试通过（重点：config 默认值相关断言若存在需同步更新）。
- 手动目检：设置页三分区、onboarding。
