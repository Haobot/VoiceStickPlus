# Windows 内置 ASR API Key 方案

## 背景与根因

**现象**：首次安装 MSI 的 Windows 设备仍提示填写 ASR API Key。

**根因**：现有种子机制本身健全，但构建时未注入真实 key：

1. `resources/config.template.toml` 是占位脱敏版（`volcengine_api_key = ""`）。
2. `build-msi.bat:134-146` 支持 `VOICESTICK_CONFIG_TEMPLATE` 环境变量把真实配置覆盖到 build 目录的 `config.template.toml`，但**构建时未设置该变量**，MSI 装的是空 key 模板。
3. 新用户首启 `AppConfig::Load()` → `SeedConfigFromTemplate`（`app_config.cc:545-547`）把空 key 模板复制到 `%APPDATA%\VoiceStick\config.toml`。
4. `ActiveApiKey()` 返回空 → `NeedsOnboarding` 为 true（`onboarding_dialog.cc:84`）→ onboarding 弹出 → `NeedsAsrStep` 为 true（`app_config.cc:933`）→ kAsr 步要求填写。

onboarding 的 kAsr 跳过逻辑（`onboarding_dialog.cc:496`）已存在，只要 `ActiveApiKey()` 非空就跳过。问题在于 key 没进分发包。

## 目标

exe 内置 volcengine API Key（编译期注入），新用户首启 `ActiveApiKey()` 回退内置 key，onboarding 自动跳过 kAsr 步，无需填写。构建 MSI 时自动从本机 `config.toml` 提取 key 注入,无需手动设环境变量。

## 方案：编译期内置 key + ActiveApiKey 回退

### 1. 内置 key 头文件（编译期注入）

新增 `desktop/windows/src/builtin_secrets.h.in`：

```cpp
#pragma once

namespace voicestick {

// 编译期内置 ASR API Key。预配置 MSI 分发场景下作为新用户首启回退 key，
// 避免新用户被要求填写。公开构建留空（ActiveApiKey 回退空，正常读用户配置）。
// 安全：key 编译进二进制可被逆向提取，仅用于内测分发，勿用于公开发布。
constexpr const char* kBuiltinAsrApiKey = "@VOICESTICK_BUILTIN_API_KEY@";

}  // namespace voicestick
```

CMake `configure_file(... @ONLY)` 生成到 `${CMAKE_CURRENT_BINARY_DIR}/builtin_secrets.h`。

### 2. CMakeLists.txt

- 增加 cache 变量 `VOICESTICK_BUILTIN_API_KEY`（默认空字符串）。
- `configure_file(src/builtin_secrets.h.in ${CMAKE_CURRENT_BINARY_DIR}/builtin_secrets.h @ONLY)`。
- `voicestick_core` 的 `target_include_directories` 追加 `${CMAKE_CURRENT_BINARY_DIR}`（当前仅 `VoiceStickApp` 有该目录）。
- 校验：key 含 `"` 或 `\` 会在生成的头文件里破坏字符串字面量，CMake 层加 `string(REGEX MATCH ...)` 检测并报错（火山 key 通常 UUID 风格，正常不会触发）。

### 3. app_config 回退逻辑

`app_config.h` 新增两个函数：

```cpp
// 返回编译期内置 ASR API Key（公开构建为空）。
std::string BuiltinApiKey();

// 解析当前生效 key：配置文件 key 优先；volcengine 模式空则回退内置 key。
// 抽成纯函数便于单元测试（不依赖编译期常量）。
std::string ResolveActiveApiKey(AsrProvider provider,
                                std::string_view voicestick_key,
                                std::string_view volcengine_key,
                                std::string_view tencent_id,
                                std::string_view builtin_key);
```

`app_config.cc` 改动：

```cpp
#include "builtin_secrets.h"

std::string BuiltinApiKey() {
    return kBuiltinAsrApiKey;
}

std::string ResolveActiveApiKey(AsrProvider provider,
                                std::string_view voicestick_key,
                                std::string_view volcengine_key,
                                std::string_view tencent_id,
                                std::string_view builtin_key) {
    switch (provider) {
        case AsrProvider::kVoiceStickCloud: return std::string(voicestick_key);
        case AsrProvider::kVolcengine:
            // 配置文件 key 优先；空则回退编译期内置 key（预配置 MSI 分发场景）。
            return !volcengine_key.empty() ? std::string(volcengine_key)
                                           : std::string(builtin_key);
        case AsrProvider::kTencent: return std::string(tencent_id);
    }
    return {};
}

std::string AppConfig::ActiveApiKey() const {
    return ResolveActiveApiKey(asr_provider, voicestick_api_key, volcengine_api_key,
                               tencent_secret_id, BuiltinApiKey());
}
```

`NeedsAsrStep` / `NeedsOnboarding` 逻辑不变，已基于 `ActiveApiKey()`，自动受益。

### 4. build-msi.bat 自动提取

在 CMake configure 前从本机 `%APPDATA%\VoiceStick\config.toml` 提取 `volcengine_api_key`，传给 CMake：

```bat
:: 内置 key：优先环境变量 VOICESTICK_BUILTIN_API_KEY；未设则从本机 config.toml 提取 volcengine_api_key。
if not defined VOICESTICK_BUILTIN_API_KEY (
    if exist "%APPDATA%\VoiceStick\config.toml" (
        for /f "usebackq delims=" %%k in (`powershell -NoProfile -Command "$t=Get-Content -Raw '%APPDATA%\VoiceStick\config.toml'; if($t -match 'volcengine_api_key\s*=\s*\"([^\"]+)\"'){$matches[1]}"`) do set "VOICESTICK_BUILTIN_API_KEY=%%k"
    )
)
if not defined VOICESTICK_BUILTIN_API_KEY (
    echo WARNING: 未提取到 volcengine_api_key，exe 无内置 key，新用户仍需填写。
) else (
    echo Injecting built-in ASR API key into VoiceStick.exe
)
cmake -S "%WINDOWS_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVOICESTICK_BUILTIN_API_KEY="%VOICESTICK_BUILTIN_API_KEY%"
```

保留 `VOICESTICK_CONFIG_TEMPLATE` 文件注入机制（兼容，可整体覆盖 config）。

### 5. 测试（TDD）

`desktop/windows/tests/core_tests.cc` 增加 `TestActiveApiKeyBuiltinFallback`，覆盖：

- volcengine + 空 key + 内置非空 → 回退内置 key
- volcengine + 配置 key 非空 → 配置 key 优先（不回退）
- volcengine + 空 key + 空内置 → 空
- tencent → 返回 `tencent_secret_id`（不回退内置）
- cloud → 返回 `voicestick_api_key`（不回退内置）

在 `main()` 的 `TestNeedsAsrStep()` 调用附近注册。

### 6. 保留项

- `config.template.toml` 种子机制保留（兼容现有首启复制 + `VOICESTICK_CONFIG_TEMPLATE` 文件注入）。
- onboarding `NeedsAsrStep` / `NeedsOnboarding` 不改（已基于 `ActiveApiKey`）。
- 设置对话框 `LoadConfigIntoControls` 不改（直接读配置文件 key；内置 key 生效时字段空，但新用户走 onboarding 不进设置，老用户主动进设置看到空字段是可接受次要影响）。

## 边界与安全

- **仅 volcengine 模式回退**内置 key；tencent / cloud 不回退（内置 key 是 volcengine 的，不能跨 provider 用）。
- **配置文件 key 优先**于内置 key：用户在 onboarding/设置填自己的 key 后覆盖内置。
- **内置 key 不写回 `%APPDATA%`**：仅内存回退，比现有 config.template 文件注入更安全（key 不落盘用户目录）。
- **key 编译进 exe 可被逆向提取**，与 MSI 内明文 `config.template.toml` 同级别风险；仅用于内测分发，勿公开发布。
- 本机 config 无 `volcengine_api_key` 时内置 key 空，回退不生效（`build-msi.bat` 警告）。

## 实现顺序（TDD）

1. 🔴 在 `core_tests.cc` 写 `TestActiveApiKeyBuiltinFallback`，断言 `ResolveActiveApiKey` 各分支（编译失败，因函数未声明）。
2. 🟢 在 `app_config.h/cc` 实现 `BuiltinApiKey` + `ResolveActiveApiKey` + 改 `ActiveApiKey`；新增 `builtin_secrets.h.in`；CMakeLists 加 cache 变量 + configure_file + include 目录。测试通过。
3. 🔵 `build-msi.bat` 加自动提取与注入逻辑。
4. 构建验证 + 真机验证。

## 验证清单

1. `ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests`：`TestActiveApiKeyBuiltinFallback` 通过，无回归。
2. `build_win.bat` 构建通过（exe 时间戳/体积核对）。
3. `build-msi.bat` 构建签名 MSI，日志出现 `Injecting built-in ASR API key`（本机 config 有 key 时）。
4. 真机：全新机器装 MSI，首启 onboarding 仅弹设备步，配对后 `GoNext` 跳过 kAsr 直达 kReady，ASR 可用。
