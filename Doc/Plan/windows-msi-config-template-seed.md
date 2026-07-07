# Windows MSI 内置 config 模板 + 首启复制

## 背景

调试阶段需把预配好的 `config.toml`（含 ASR/LLM 密钥、服务器地址等）随 MSI 分发给测试同事，装完即用，无需手动配置。

## 现状与陷阱

- MSI 是 `perMachine`，装到 `C:\Program Files\VoiceStick`（`installer/VoiceStick.wxs`）。
- `app_config.cc` 便携模式逻辑：exe 同级存在 `config.toml` 就读 exe 目录，否则读 `%APPDATA%\VoiceStick\config.toml`。
- `AppConfig::Save()` 写回 `ConfigPath()`。

直接把 `config.toml` 打包进 MSI 的两个陷阱：

1. **装到 `Program Files\VoiceStick\config.toml`** → 触发便携模式 → `Save()` 尝试写 Program Files，而 VoiceStick 是 asInvoker 普通进程无写权限 → 配置/配对设备**存不下来**。
2. **装到 `%APPDATA%\VoiceStick\config.toml`** → perMachine MSI 里 `AppDataFolder` 只解析到"执行安装的那个账户"的 AppData，其他测试同事各自账户登录后文件不存在。

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
// target 已存在 → 不覆盖返回 false；template 不存在 → 跳过返回 false；
// 复制失败（权限/磁盘）→ 静默返回 false，不阻塞启动（Load 回退 Defaults）。
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

## 验证

- `ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests` 全绿。
- `build_win.bat` 构建通过，核对 exe 时间戳/体积。
- 手动：删 `%APPDATA%\VoiceStick\config.toml` → 启动 VoiceStick → 文件应从模板生成；再次启动不覆盖；改后重启保留改动。
