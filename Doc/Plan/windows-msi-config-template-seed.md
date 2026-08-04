# Windows MSI 内置 config 模板 + 首启复制

## 背景

调试阶段需把预配好的 `config.toml`（含 ASR/LLM 密钥、服务器地址等）随 MSI 分发给测试同事，装完即用，无需手动配置。

## 现状与陷阱

- MSI 是 `perMachine`，装到 `C:\Program Files\VoiceStick`（`installer/VoiceStick.wxs`）。
- `app_config.cc` 便携模式逻辑：exe 同级存在 `config.toml` 就读 exe 目录，否则读 `%APPDATA%\VoiceStick\config.toml`。
- `AppConfig::Save()` 写回 `ConfigPath()`。

直接把 `config.toml` 打包进 MSI 的两个陷阱：

1. **装到 `Program Files\VoiceStick\config.toml`** -> 触发便携模式 -> `Save()` 尝试写 Program Files，而 VoiceStick 是 asInvoker 普通进程无写权限 -> 配置/配对设备**存不下来**。
2. **装到 `%APPDATA%\VoiceStick\config.toml`** -> perMachine MSI 里 `AppDataFolder` 只解析到"执行安装的那个账户"的 AppData，其他测试同事各自账户登录后文件不存在。

## 方案

MSI 内置**模板** `config.template.toml`（注意文件名不是 `config.toml`，避免误触发便携模式）到 `Program Files\VoiceStick\`。程序首启时，若非便携模式且 `%APPDATA%\VoiceStick\config.toml` 不存在，从 exe 同级模板复制一份到 `%APPDATA%\VoiceStick\config.toml`。已存在则不覆盖，保护用户改动。

## 改动清单

| 文件 | 改动 |
|---|---|
| `desktop/windows/resources/config.template.toml` | 新增占位脱敏模板，结构完整、密钥占位 |
| `desktop/windows/src/app_config.h` | 声明 `static bool SeedConfigFromTemplate(...)` |
| `desktop/windows/src/app_config.cc` | 实现纯函数 + `Load()` 非便携模式时调用 |
| `desktop/windows/CMakeLists.txt` | POST_BUILD `copy_if_different` 模板到 build dir（对齐 WinSparkle.dll 复制方式） |
| `desktop/windows/installer/VoiceStick.wxs` | INSTALLFOLDER 下加 `<Component>` 装模板，`<ComponentRef>` 进 Main Feature |
| `desktop/windows/tests/core_tests.cc` | `TestConfigTemplateSeeding()` 4 场景，加入 `main()` |
| `scripts/build-msi.bat` | 检测 `VOICESTICK_CONFIG_TEMPLATE` 环境变量，构建前覆盖 build dir 模板 |

## `SeedConfigFromTemplate` 契约

```cpp
// 返回是否执行了复制。
// target 已存在 -> 不覆盖返回 false；template 不存在 -> 跳过返回 false；
// 复制失败（权限/磁盘）-> 静默返回 false，不阻塞启动（Load 回退 Defaults）。
bool SeedConfigFromTemplate(const std::filesystem::path& template_path,
                            const std::filesystem::path& target_path);
```

## `Load()` 集成

```cpp
AppConfig AppConfig::Load() {
    if (!IsPortableMode()) {
        SeedConfigFromTemplate(PortableBaseDirectory() / L"config.template.toml", ConfigPath());
    }
    AppConfig config = Load(ConfigPath());
    config.portable_mode = IsPortableMode();
    return config;
}
```

## 模板来源（密钥不进仓库）

仓库提交**占位脱敏模板**（密钥留空）。调试分发真实配置时：

```bat
set VOICESTICK_CONFIG_TEMPLATE=%APPDATA%\VoiceStick\config.toml
scripts\build-msi.bat
```

`build-msi.bat` 检测到该环境变量就用它覆盖 build dir 的模板，密钥只在本机构建时临时注入，**不进 git 历史**。未设环境变量时用仓库占位模板（构建仍能通过，但 MSI 内是脱敏配置）。

## 升级行为

`MajorUpgrade` 不覆盖 `%APPDATA%` 已有配置（首启复制仅目标不存在时执行）。需强制刷新模板时，测试同事删除 `%APPDATA%\VoiceStick\config.toml` 后重启 VoiceStick 即可重新从模板生成。

## 内置 key 的向导行为（v2.2.x 新增）

内置 key 分发场景下，测试同事装完 MSI 首启时 `config.toml` 已含 ASR/LLM 凭据，但尚未配对设备。`OnboardingDialog` 仍会弹出（`NeedsOnboarding` 判 `paired_device_ids.empty() || ActiveApiKey().empty()`，无设备即弹），但向导只走两步：

1. `kDevice`：引导配对设备。
2. 配对成功后 `GoNext` 判 `NeedsAsrStep(config_)`——`ActiveApiKey()` 非空返回 false——**跳过 `kAsr` 直接进入 `kReady`**。

公开版（无内置 key）仍走三步（kDevice -> kAsr -> kReady），`kAsr` 步要求用户填写 API Key。`NeedsAsrStep` 已下沉到 `voicestick_core`（`app_config.h/.cc`，自由函数 `bool NeedsAsrStep(const AppConfig&)`），供单元测试覆盖；`onboarding_dialog.cc` 经 `#include "app_config.h"` 可见声明。

## 运行时 Save 保留磁盘凭据（v2.2.x 新增）

**根因**：设置对话框/托盘菜单改主题、输出目标、编码器、交互设置等非凭据字段时，`win32_app.cc` 此前直接 `config_.Save()`。`config_` 是进程启动时 `Load()` 的内存副本，若用户在运行期手改了 `%APPDATA%\VoiceStick\config.toml` 的密钥（或用内置 key 模板覆盖后首启 `Load` 拿到 key、随后某次内存副本的 key 被清空），`Save()` 会用内存里的过期值全截断重写磁盘，**把真实 key 覆盖成空**，ASR/精修/热词全失效。这是"替换 config 后 ASR 仍不工作"的主因之一。

**修复**：新增 `AppConfig::SavePreservingDiskCredentials()`（`app_config.h/.cc`）。运行时 Save 点先 `Load(path)` 重读磁盘最新凭据，覆盖到内存副本的对应字段再写回：

- 保留字段：`voicestick_api_key`/`voicestick_cloud_url`、`volcengine_api_key`/`volcengine_boosting_table_id`/`volcengine_correct_table_id`、`tencent_secret_id`/`tencent_secret_key`/`tencent_appid`/`tencent_engine_model_type`/`tencent_hotword_id`、`llm_base_url`/`llm_api_key`/`llm_model`/`refine_prompt`/`hotword_process_prompt`。
- `win32_app.cc` 10 处运行时 `config_.Save()` 全部改用 `SavePreservingDiskCredentials()`（主题/输出/编码器/交互/鼠标调优等非凭据保存点）。
- onboarding 与设置对话框里**用户刚输入 key 的保存仍用普通 `Save()`**（需写入新 key，不能保留磁盘旧值）。

`TestSavePreservingDiskCredentials` 覆盖：磁盘有真实 volcengine/tencent/llm key + `auto_enter=false`，内存副本 key 清空 + `auto_enter=true`，调用后断言磁盘 key 保留、`auto_enter` 等非凭据字段取内存新值。

## 分发示例 config.template.toml

仓库提交的 `desktop/windows/resources/config.template.toml` 是占位脱敏模板（密钥全空）。内测分发时用 `VOICESTICK_CONFIG_TEMPLATE` 环境变量指向一份**填好真实 key** 的 config，`build-msi.bat` 构建前覆盖 build dir 模板。完整字段示例（密钥为占位符，构建机替换）：

```toml
asr_provider = "volcengine"          # 或 "tencent" / "voicestick_cloud"

# 火山引擎 ASR
volcengine_api_key = "REAL_VOLCENGINE_KEY"
volcengine_boosting_table_id = ""
volcengine_correct_table_id = ""

# 腾讯云 ASR（asr_provider="tencent" 时用）
tencent_secret_id = "REAL_TENCENT_ID"
tencent_secret_key = "REAL_TENCENT_KEY"
tencent_appid = "REAL_APPID"
tencent_engine_model_type = "16k_zh"
tencent_hotword_id = ""

# DeepSeek（OpenAI 兼容 LLM，用于翻译/精修/热词精修）
llm_base_url = "https://api.deepseek.com/v1"
llm_api_key = "REAL_DEEPSEEK_KEY"
llm_model = "deepseek-chat"
refine_enabled = true
hotword_process_enabled = false      # 内测可按需开
```

`build-msi.bat` 调用示例：

```bat
set VOICESTICK_CONFIG_TEMPLATE=%APPDATA%\VoiceStick\config.toml
scripts\build-msi.bat
```

密钥只在本机构建时临时注入，**不进 git 历史**；未设环境变量时用仓库占位模板（MSI 内脱敏配置，装完仍需用户填 key）。带内置 key 的 MSI **仅供内测**，不得上传公开发布渠道（GitHub Release / 阿里云 OSS / appcast）。

## 故障排查：替换 config 后 ASR 不工作

用户报告"替换了 `config.toml` 后 ASR 仍无法启用"，按以下路径排查：

1. **便携模式误触发**：若把 `config.toml` 放到 exe 同级目录（`C:\Program Files\VoiceStick\config.toml` 或便携版 exe 目录），`IsPortableMode()` 返回 true，程序读 exe 目录的 config，**不读 `%APPDATA%`**。诊断：看 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 是否记录 `portable mode`。解决：删 exe 同级的 `config.toml`，只保留 `%APPDATA%\VoiceStick\config.toml`。
2. **运行时 Save 覆盖 key**（上一节修复的根因）：替换 config 后若未重启 VoiceStick，进程内存仍是旧副本（key 可能为空）；此时触发任意运行时 Save（改主题/输出/编码器等）会用内存空 key 全截断重写磁盘，覆盖刚替换的真实 key。解决：**替换 config 后重启 VoiceStick**；升级到含 `SavePreservingDiskCredentials` 的版本后，运行时 Save 不再覆盖磁盘 key。
3. **asr_provider 与 key 字段不匹配**：`ActiveApiKey()` 按 `asr_provider` 取对应字段——`volcengine` 取 `volcengine_api_key`、`tencent` 取 `tencent_secret_id`、`voicestick_cloud` 取 `voicestick_api_key`。替换 config 时若改了 `asr_provider` 但没填对应字段，`ActiveApiKey()` 返回空，向导 `kAsr` 步仍弹。诊断：核对 `asr_provider` 与对应 key 字段。
4. **向导未完成**：装完首启若 `NeedsOnboarding`（无配对设备或无 key）弹向导，用户没走完向导（没配对设备或没到 `kReady`），配置流程未完成。解决：走完向导（内置 key 版配对设备后自动跳 `kAsr` 到 `kReady`）。

## 验证

- `ctest --test-dir desktop\windows\build-x64 --output-on-failure` 全绿（含 `TestNeedsAsrStep`、`TestSavePreservingDiskCredentials`、`TestConfigTemplateSeeding`）。
- `build_win.bat` 构建通过，核对 exe 时间戳/体积。
- 手动：删 `%APPDATA%\VoiceStick\config.toml` -> 启动 VoiceStick -> 文件应从模板生成；再次启动不覆盖；改后重启保留改动。
- 内置 key 真机：装内置 key MSI -> 首启向导配对设备后跳 `kAsr` 直达 `kReady` -> 录音 ASR/精修/热词可用；运行期改非凭据设置后重启，key 仍保留。
