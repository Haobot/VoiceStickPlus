# 热词处理（Hotword Processing）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Windows 端划词加词链路中插入可选的 LLM 热词提炼：启用后点击「添加到热词」不再直接写入原文，而是提炼出热词去重后入表，并在浮窗展示结果 3 秒。

**Architecture:** `voicestick_core` 新增 `HotwordExtractor`（复用 `LLMChatClient` 网络层，提示词构建/结果解析/去重为可单测静态函数）；`Win32App` 的 `on_add_hotword` 回调按 `hotword_process_enabled` 分支；`OverlayWindow` 新增 `ShowTimedMessage` + `Mode::kInfo`；设置对话框复刻文本精修区块形态。

**Tech Stack:** C++20 / Win32 / CMake+Ninja+MSVC 2022 / CTest（assert 单测）。

**Spec:** `docs/superpowers/specs/2026-07-28-hotword-processing-design.md`

**通用注意事项（每个 commit 步骤都适用）：**
- `.gitignore` 整体忽略 `desktop/windows/`，提交 Windows 端源码必须 `git add -f`。
- 增量构建命令（Git Bash 中执行）：
  ```bash
  cmd //c '@echo off && call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build desktop\windows\build-x64'
  ```
  （若 `build-x64` 不存在，先加 `cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja`。）
- 单测命令：
  ```bash
  cmd //c 'ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests'
  ```

---

### Task 1: 配置字段 `hotword_process_enabled` / `hotword_process_prompt`

**Files:**
- Modify: `desktop/windows/src/app_config.h`（在 `refine_prompt` 声明后，`app_config.h:150` 附近）
- Modify: `desktop/windows/src/app_config.cc:369-370`（KV 解析）、`:509-510`（TOML 表解析）、`:668-669`（序列化）
- Modify: `desktop/macos/Config/config.example.toml:19`（refine 行之后）
- Test: `desktop/windows/tests/core_tests.cc`（新增 `TestHotwordProcessConfig`，注册进 `main()`，注册点在 `core_tests.cc:5324` 附近）

- [x] **Step 1: 写失败测试**

在 `core_tests.cc` 中 `TestLlmRefinePromptAndPayload` 函数（`:964`）之后新增：

```cpp
void TestHotwordProcessConfig() {
    // 默认关闭、prompt 默认空（空 = 使用内置默认提示词）。
    assert(AppConfig::Defaults().hotword_process_enabled == false);
    assert(AppConfig::Defaults().hotword_process_prompt.empty());

    // TOML 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_process_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.hotword_process_enabled = true;
    config.hotword_process_prompt = "自定义提炼提示词\n第二行";
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.hotword_process_enabled == true);
    assert(loaded.hotword_process_prompt == config.hotword_process_prompt);
    std::filesystem::remove(temp);
}
```

在 `main()` 中 `TestLlmRefinePromptAndPayload();`（`:5325`）之后注册：

```cpp
    TestHotwordProcessConfig();
```

- [x] **Step 2: 运行测试确认失败**

跑单测命令。预期：编译失败，`hotword_process_enabled` 不是 `AppConfig` 成员。

- [x] **Step 3: 实现配置字段**

`app_config.h`：在 `refine_prompt` 声明（`:150`）之后添加：

```cpp
    // 热词处理：划词加词时用 LLM 从选中长文中提炼热词，只把提炼结果写入热词表。
    // 复用 llm_base_url/llm_api_key/llm_model。默认关闭。
    bool hotword_process_enabled = false;
    // 热词提炼 system prompt 覆盖；为空时使用内置默认 prompt。
    std::string hotword_process_prompt;
```

`app_config.cc` KV 解析（`:370` 之后）：

```cpp
    if (key == "hotword_process_enabled") config.hotword_process_enabled = BoolValue(value, config.hotword_process_enabled);
    if (key == "hotword_process_prompt") config.hotword_process_prompt = value;
```

`app_config.cc` TOML 表解析（`:510` 之后）：

```cpp
        if (auto value = TomlBool(table, "hotword_process_enabled")) config.hotword_process_enabled = *value;
        if (auto value = TomlString(table, "hotword_process_prompt")) config.hotword_process_prompt = *value;
```

`app_config.cc` 序列化（`:669` 之后）：

```cpp
    output << "hotword_process_enabled = " << (hotword_process_enabled ? "true" : "false") << "\n";
    output << "hotword_process_prompt = \"" << TomlEscape(hotword_process_prompt) << "\"\n";
```

`desktop/macos/Config/config.example.toml` 在 `# refine_prompt = ...` 行（`:19`）之后添加：

```toml
hotword_process_enabled = false
# hotword_process_prompt = ""  # 留空使用内置默认热词提炼 prompt（Windows-only）
```

- [x] **Step 4: 构建并运行测试确认通过**

跑增量构建 + 单测命令。预期：PASS。

- [x] **Step 5: Commit**

```bash
git add -f desktop/windows/src/app_config.h desktop/windows/src/app_config.cc desktop/windows/tests/core_tests.cc
git add desktop/macos/Config/config.example.toml
git commit -m "feat(config): 新增热词处理配置项 hotword_process_enabled/prompt"
```

---

### Task 2: 核心组件 `HotwordExtractor`

**Files:**
- Create: `desktop/windows/src/hotword_extractor.h`
- Create: `desktop/windows/src/hotword_extractor.cc`
- Modify: `desktop/windows/CMakeLists.txt:66`（voicestick_core 源列表）
- Test: `desktop/windows/tests/core_tests.cc`（新增 `TestHotwordExtractorPromptAndParse`，注册进 `main()`）

- [x] **Step 1: 写失败测试**

在 `core_tests.cc` 中 `TestHotwordProcessConfig` 之后新增：

```cpp
void TestHotwordExtractorPromptAndParse() {
    // 内置默认提示词含提取语义关键词；覆盖值 Trim 后原样返回。
    const auto prompt = HotwordExtractor::BuildExtractPrompt("");
    assert(prompt.find("热词") != std::string::npos);
    assert(prompt.find("专有名词") != std::string::npos);
    const auto custom = HotwordExtractor::BuildExtractPrompt("  my extract prompt  ");
    assert(custom == "my extract prompt");

    // 解析：换行/逗号切分、Trim、去重（复用 ParseHotwordList 语义）。
    const auto words = HotwordExtractor::ParseExtractResult("小智\nVoiceStick\r\n小智\n豆包,AGI");
    assert((words == std::vector<std::string>{"小智", "VoiceStick", "豆包", "AGI"}));

    // 空输入 / 纯空白 → 空结果。
    assert(HotwordExtractor::ParseExtractResult("").empty());
    assert(HotwordExtractor::ParseExtractResult("  \n \n").empty());

    // 单词超过 64 字符被过滤。
    const std::string long_word(65, 'x');
    assert(HotwordExtractor::ParseExtractResult(long_word).empty());

    // 总量截断到 20 个。
    std::string many;
    for (int i = 0; i < 25; ++i) {
        many += "w" + std::to_string(i);
        many += "\n";
    }
    assert(HotwordExtractor::ParseExtractResult(many).size() == 20);

    // DiffNewHotwords：保序、剔除已存在词、自身去重。
    const auto diff = HotwordExtractor::DiffNewHotwords({"a", "b", "c", "a"}, {"b"});
    assert((diff == std::vector<std::string>{"a", "c"}));
    assert(HotwordExtractor::DiffNewHotwords({"b"}, {"b"}).empty());
}
```

在 `core_tests.cc` 顶部 include 区（参照已有 `#include "llm_refinement_client.h"`）添加：

```cpp
#include "hotword_extractor.h"
```

在 `main()` 中 `TestHotwordProcessConfig();` 之后注册：

```cpp
    TestHotwordExtractorPromptAndParse();
```

- [x] **Step 2: 运行测试确认失败**

跑单测命令。预期：编译失败，找不到 `hotword_extractor.h`。

- [x] **Step 3: 实现 HotwordExtractor**

创建 `desktop/windows/src/hotword_extractor.h`：

```cpp
#pragma once

#include "app_config.h"
#include "llm_chat_client.h"

#include <functional>
#include <string>
#include <vector>

namespace voicestick {

// 热词提炼器：复用 LLMChatClient 网络层，从划词长文中提取适合作为语音识别热词的
// 词汇（专有名词/人名/术语等）。提示词构建、结果解析、去重 diff 均为可单测静态函数。
class HotwordExtractor : public LLMChatClient {
public:
    // 基类构造为 protected（见 LLMRefinementClient 同款注释），显式 public 转发。
    explicit HotwordExtractor(AppConfig config) : LLMChatClient(std::move(config)) {}

    // 异步提炼：detached 线程完成后回调 completion(ok, 原始响应文本或错误信息)。
    // 调用方负责解析响应与回切 UI 线程。
    void Extract(std::string text,
                 std::string prompt_override,
                 std::function<void(bool, std::string)> completion) const;

    // 可单测纯函数：override 非空（去空白后）返回 override，否则返回内置默认提炼 prompt。
    static std::string BuildExtractPrompt(const std::string& prompt_override);

    // 可单测纯函数：解析 LLM 响应为词列表。按换行/逗号切分、Trim、去重
    // （复用 ParseHotwordList），过滤超长词（>kMaxWordLen），总量截断到 kMaxWordCount。
    static std::vector<std::string> ParseExtractResult(const std::string& response);

    // 可单测纯函数：返回 extracted 中不在 existing 里的新词（保序、结果内去重）。
    static std::vector<std::string> DiffNewHotwords(const std::vector<std::string>& extracted,
                                                    const std::vector<std::string>& existing);

    static constexpr std::size_t kMaxWordLen = 64;    // 单词限长（字符）
    static constexpr std::size_t kMaxWordCount = 20;  // 单次提炼限量
};

} // namespace voicestick
```

创建 `desktop/windows/src/hotword_extractor.cc`：

```cpp
#include "hotword_extractor.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace voicestick {

namespace {

std::string Trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

} // namespace

void HotwordExtractor::Extract(std::string text,
                               std::string prompt_override,
                               std::function<void(bool, std::string)> completion) const {
    ChatAsync(BuildExtractPrompt(prompt_override), std::move(text), std::move(completion));
}

std::string HotwordExtractor::BuildExtractPrompt(const std::string& prompt_override) {
    const auto trimmed = Trim(prompt_override);
    if (!trimmed.empty()) return trimmed;
    return
        "你是一个热词提取器。\n"
        "从用户给出的文本中提取适合作为语音识别热词的词汇：专有名词、人名、地名、组织名、品牌名、产品名、专业术语等。\n"
        "\n"
        "• 每行输出一个词，只输出词本身。\n"
        "• 不要编号、解释、标点或 Markdown 格式。\n"
        "• 普通常用词不要提取。\n"
        "• 若没有值得提取的词，直接输出空。";
}

std::vector<std::string> HotwordExtractor::ParseExtractResult(const std::string& response) {
    auto words = ParseHotwordList(response);  // 已处理换行/逗号切分、Trim、去重
    std::vector<std::string> result;
    for (auto& w : words) {
        if (w.size() > kMaxWordLen) continue;
        result.push_back(std::move(w));
        if (result.size() >= kMaxWordCount) break;
    }
    return result;
}

std::vector<std::string> HotwordExtractor::DiffNewHotwords(
    const std::vector<std::string>& extracted,
    const std::vector<std::string>& existing) {
    std::vector<std::string> result;
    for (const auto& w : extracted) {
        if (std::find(existing.begin(), existing.end(), w) == existing.end() &&
            std::find(result.begin(), result.end(), w) == result.end()) {
            result.push_back(w);
        }
    }
    return result;
}

} // namespace voicestick
```

`CMakeLists.txt` 在 `src/llm_refinement_client.cc`（`:66`）之后添加：

```cmake
    src/hotword_extractor.cc
```

- [x] **Step 4: 构建并运行测试确认通过**

跑增量构建 + 单测命令。预期：PASS。

- [x] **Step 5: Commit**

```bash
git add -f desktop/windows/src/hotword_extractor.h desktop/windows/src/hotword_extractor.cc desktop/windows/CMakeLists.txt desktop/windows/tests/core_tests.cc
git commit -m "feat(core): 新增 HotwordExtractor 热词提炼组件"
```

---

### Task 3: `SelectionHotwordManager` 可配置长度上限

**Files:**
- Modify: `desktop/windows/src/selection_hotword_manager.h:90`
- Modify: `desktop/windows/src/selection_hotword_manager.cc:286`
- Modify: `desktop/windows/src/win32_app.cc:1484-1488`（`SyncSelectionHotword`）

- [x] **Step 1: 修改头文件**

`selection_hotword_manager.h`：

- 在 `SetLanguage` 声明（`:35`）后添加：

```cpp
    // 设置划词长度上限（字符）。热词处理启用时上层放宽到 kMaxProcessLen。
    void SetMaxLength(int max_length) { max_length_ = max_length; }
```

- 常量区（`:90` 附近）把 `kMaxHotwordLen` 改为 public 并新增 `kMaxProcessLen`：将这两个常量移到 `public:` 区（放在 `on_add_hotword` 声明之后），并新增成员 `max_length_`：

```cpp
    static constexpr int kMaxHotwordLen = 64;      // 未启用热词处理：超长选区忽略
    static constexpr int kMaxProcessLen = 2000;    // 启用热词处理：长文送 LLM 提炼
```

private 常量区删除原 `kMaxHotwordLen` 行；private 成员区（`pending_text_` 之后）添加：

```cpp
    int max_length_ = kMaxHotwordLen;  // 当前生效的划词长度上限
```

- [x] **Step 2: 修改实现**

`selection_hotword_manager.cc:286`：

```cpp
    if (text.size() > static_cast<std::size_t>(max_length_)) return;  // 选区过长：静默忽略
```

- [x] **Step 3: 接线 SyncSelectionHotword**

`win32_app.cc` `SyncSelectionHotword()`（`:1484-1488`）改为：

```cpp
void Win32App::SyncSelectionHotword() {
    if (!selection_hotword_manager_) return;
    selection_hotword_manager_->SetLanguage(EffectiveUiLanguage(config_.ui_language));
    selection_hotword_manager_->SetMaxLength(config_.hotword_process_enabled
                                                 ? SelectionHotwordManager::kMaxProcessLen
                                                 : SelectionHotwordManager::kMaxHotwordLen);
    selection_hotword_manager_->SetEnabled(config_.selection_hotword_enabled);
}
```

- [x] **Step 4: 构建并跑单测**

增量构建 + 单测命令。预期：PASS（无新测试，回归保护）。

- [x] **Step 5: Commit**

```bash
git add -f desktop/windows/src/selection_hotword_manager.h desktop/windows/src/selection_hotword_manager.cc desktop/windows/src/win32_app.cc
git commit -m "feat(selection): 划词长度上限可配置，热词处理启用时放宽到 2000"
```

---

### Task 4: `OverlayWindow::ShowTimedMessage` + `Mode::kInfo`

**Files:**
- Modify: `desktop/windows/src/overlay_window.h:36`（ShowError 声明后）、`:48`（Mode 枚举）
- Modify: `desktop/windows/src/overlay_window.cc:346-350`（ShowError 后）、`:1137-1151`（PaintIndicator）

- [x] **Step 1: 头文件声明**

`overlay_window.h` 在 `ShowError` 声明（`:36`）后添加：

```cpp
    // 显示一条临时消息，duration_ms 后自动隐藏（复用 kAutoHideTimerId）。
    // 用于热词处理结果/错误反馈等一次性提示。
    void ShowTimedMessage(const std::string& text, int duration_ms,
                          std::function<void()> on_complete = {});
```

Mode 枚举（`:48`）改为：

```cpp
    enum class Mode { kListening, kCountdown, kPaused, kError, kHidden, kRefining, kInfo };
```

- [x] **Step 2: 实现 ShowTimedMessage**

`overlay_window.cc` 在 `ShowError` 实现（`:346-350`）之后添加：

```cpp
void OverlayWindow::ShowTimedMessage(const std::string& text, int duration_ms,
                                     std::function<void()> on_complete) {
    Show(Mode::kInfo, text);
    pending_callback_ = std::move(on_complete);
    SetTimer(hwnd_, kAutoHideTimerId, duration_ms, nullptr);
}
```

- [x] **Step 3: PaintIndicator 增加 kInfo 分支**

`overlay_window.cc` 在 kError 分支（`:1141-1151`）的 `}` 之后、函数收尾 `}` 之前添加：

```cpp
    else if (mode_ == Mode::kInfo) {
        // 中性信息指示：实心圆点。
        Gdiplus::SolidBrush dot_brush(Gdiplus::Color(kIndicatorAlpha, ink_rgb, ink_rgb, ink_rgb));
        const int inset = size / 4;
        graphics.FillEllipse(&dot_brush, x + inset, y + inset, size - inset * 2, size - inset * 2);
    }
```

注意：不要把 `kInfo` 加进 `:405`/`:410`/`:450` 的动画模式条件——它不需要连续动画。

- [x] **Step 4: 构建并跑单测**

增量构建 + 单测命令。预期：PASS。

- [x] **Step 5: Commit**

```bash
git add -f desktop/windows/src/overlay_window.h desktop/windows/src/overlay_window.cc
git commit -m "feat(overlay): 新增 ShowTimedMessage 定时消息与 kInfo 指示器"
```

---

### Task 5: 本地化字符串

**Files:**
- Modify: `desktop/windows/src/localization.h`（设置区 `:36` 后 + 划词区 `:247` 后）
- Modify: `desktop/windows/src/localization.cc`（英文表与中文表对应位置）

- [x] **Step 1: 新增 StringId 枚举**

`localization.h` 在 `kSettingsSelectionHotwordHint`（`:36`）之后添加：

```cpp
    kSettingsSectionHotwordProcess,
    kSettingsHotwordProcessEnable,
    kSettingsHotwordProcessPrompt,
```

注意：`kSettingsSectionHotwordProcess` 语义上属于 section 标题组，但枚举顺序只影响表索引，放在一起即可（表按 Index(StringId) 填充，不要求与分组一致）。

`localization.h` 在 `kSelectionHotwordTooLongBody`（`:247`）之后添加：

```cpp
    // 热词处理（LLM 提炼）
    kHotwordProcessExtracting,
    kHotwordProcessAdded,
    kHotwordProcessAllDuplicate,
    kHotwordProcessEmptyResult,
    kHotwordProcessFailed,
    kHotwordProcessNoKey,
```

- [x] **Step 2: 英文表条目**

`localization.cc` 英文表（参照 `kSettingsSelectionHotword` 条目位置）添加：

```cpp
    table[Index(StringId::kSettingsSectionHotwordProcess)] = "Hotword Processing";
    table[Index(StringId::kSettingsHotwordProcessEnable)] = "Extract hotwords from selected text with LLM";
    table[Index(StringId::kSettingsHotwordProcessPrompt)] = "Extraction prompt";
```

英文表划词区（`kSelectionHotwordTooLongBody` 条目之后）添加：

```cpp
    table[Index(StringId::kHotwordProcessExtracting)] = "Extracting hotwords...";
    table[Index(StringId::kHotwordProcessAdded)] = "Hotwords added: ";
    table[Index(StringId::kHotwordProcessAllDuplicate)] = "No new hotwords (all already exist)";
    table[Index(StringId::kHotwordProcessEmptyResult)] = "No hotwords extracted";
    table[Index(StringId::kHotwordProcessFailed)] = "Hotword extraction failed";
    table[Index(StringId::kHotwordProcessNoKey)] = "LLM API key not configured";
```

- [x] **Step 3: 中文表条目**

`localization.cc` 中文表对应位置添加：

```cpp
    table[Index(StringId::kSettingsSectionHotwordProcess)] = "热词处理";
    table[Index(StringId::kSettingsHotwordProcessEnable)] = "划词后用 LLM 提炼热词";
    table[Index(StringId::kSettingsHotwordProcessPrompt)] = "提炼提示词";
```

```cpp
    table[Index(StringId::kHotwordProcessExtracting)] = "热词提炼中…";
    table[Index(StringId::kHotwordProcessAdded)] = "已添加热词：";
    table[Index(StringId::kHotwordProcessAllDuplicate)] = "没有新热词（提炼结果均已存在）";
    table[Index(StringId::kHotwordProcessEmptyResult)] = "未提炼出热词";
    table[Index(StringId::kHotwordProcessFailed)] = "热词提炼失败";
    table[Index(StringId::kHotwordProcessNoKey)] = "未配置 LLM API Key，无法提炼热词";
```

- [x] **Step 4: 构建并跑单测**

增量构建 + 单测命令。预期：PASS（`LocalizationTablesAreComplete` 相关测试覆盖两表完整性）。

- [x] **Step 5: Commit**

```bash
git add -f desktop/windows/src/localization.h desktop/windows/src/localization.cc
git commit -m "feat(l10n): 热词处理相关本地化字符串"
```

---

### Task 6: `Win32App` 接线（LLM 提炼流程）

**Files:**
- Modify: `desktop/windows/src/win32_app.h:103`（SyncSelectionHotword 声明后）
- Modify: `desktop/windows/src/win32_app.cc:1027-1054`（on_add_hotword 回调）

- [x] **Step 1: 头文件方法声明**

`win32_app.h` 在 `SyncSelectionHotword();` 声明（`:103`）之后添加：

```cpp
    // 热词处理：LLM 异步提炼选中文本中的热词，回调经 DispatchToUi 回 UI 线程处理。
    void ProcessHotwordWithLlm(const std::string& text);
    // 提炼完成（UI 线程）：解析、去重、写表、浮窗反馈 3 秒。
    void OnHotwordExtracted(bool ok, const std::string& result);
```

- [x] **Step 2: 修改 on_add_hotword 分支**

`win32_app.cc` 顶部 include 区添加：

```cpp
#include "hotword_extractor.h"
```

`win32_app.cc:1028-1054` 的回调，在 `text.empty()` 判断之后、直接入表逻辑之前插入热词处理分支：

```cpp
        selection_hotword_manager_->on_add_hotword =
            [this](const std::string& text) {
                if (text.empty()) {
                    const auto lang = EffectiveUiLanguage(config_.ui_language);
                    ShowNotification(Tr(StringId::kSelectionHotwordEmptyTitle, lang),
                                     Tr(StringId::kSelectionHotwordEmptyBody, lang));
                    return;
                }
                // 热词处理：长文送 LLM 提炼，只把提炼结果写入热词表。
                if (config_.hotword_process_enabled) {
                    ProcessHotwordWithLlm(text);
                    return;
                }
                const auto lang = EffectiveUiLanguage(config_.ui_language);
                // ……以下为原有直接入表逻辑，保持不变……
```

（原有 `auto& hotwords = ...` 到 `ShowNotification(... AddedBody ...)` 的代码不变。）

- [x] **Step 3: 实现 ProcessHotwordWithLlm / OnHotwordExtracted**

`win32_app.cc` 在 `SyncSelectionHotword()` 实现（`:1488`）之后添加：

```cpp
void Win32App::ProcessHotwordWithLlm(const std::string& text) {
    const auto lang = EffectiveUiLanguage(config_.ui_language);
    if (config_.llm_api_key.empty()) {
        if (overlay_) overlay_->ShowTimedMessage(Tr(StringId::kHotwordProcessNoKey, lang), 3000);
        return;
    }
    if (overlay_) overlay_->ShowRefining(Tr(StringId::kHotwordProcessExtracting, lang));
    // ChatAsync 内部拷贝配置并 detached 线程执行，栈上临时对象安全。
    HotwordExtractor(config_).Extract(text, config_.hotword_process_prompt,
        [this](bool ok, std::string result) {
            DispatchToUi([this, ok, result = std::move(result)]() mutable {
                OnHotwordExtracted(ok, result);
            });
        });
}

void Win32App::OnHotwordExtracted(bool ok, const std::string& result) {
    const auto lang = EffectiveUiLanguage(config_.ui_language);
    if (!ok) {
        LogLine("Hotword extraction failed: " + result);
        if (overlay_) overlay_->ShowTimedMessage(Tr(StringId::kHotwordProcessFailed, lang), 3000);
        return;
    }
    const auto extracted = HotwordExtractor::ParseExtractResult(result);
    if (extracted.empty()) {
        if (overlay_) overlay_->ShowTimedMessage(Tr(StringId::kHotwordProcessEmptyResult, lang), 3000);
        return;
    }
    const auto new_words = HotwordExtractor::DiffNewHotwords(extracted, config_.asr_hotwords);
    if (new_words.empty()) {
        if (overlay_) overlay_->ShowTimedMessage(Tr(StringId::kHotwordProcessAllDuplicate, lang), 3000);
        return;
    }
    auto& hotwords = config_.asr_hotwords;
    hotwords.insert(hotwords.end(), new_words.begin(), new_words.end());
    try {
        config_.Save();
    } catch (const std::exception& e) {
        LogLine(std::string("Save config on hotword extract failed: ") + e.what());
    }
    if (coordinator_) coordinator_->UpdateConfig(config_);
    // 顿号拼接新词列表用于浮窗展示（重复词不展示）。
    std::string joined;
    for (std::size_t i = 0; i < new_words.size(); ++i) {
        if (i != 0) joined += "、";
        joined += new_words[i];
    }
    if (overlay_) {
        overlay_->ShowTimedMessage(Tr(StringId::kHotwordProcessAdded, lang) + joined, 3000);
    }
    LogLine("Hotwords extracted and added: " + joined);
}
```

- [x] **Step 4: 构建并跑单测**

增量构建 + 单测命令。预期：PASS。

- [x] **Step 5: 手动运行时验证**

运行 `desktop\windows\build-x64\VoiceStick.exe`，在 `%APPDATA%\VoiceStick\config.toml` 手动加 `hotword_process_enabled = true` 并确保 LLM key 已配；划选一段长文点击「添加到热词」，观察：浮窗先显示"热词提炼中…"，随后显示"已添加热词：…"3 秒消失；检查 config.toml 中 `asr_hotwords` 追加了新词。再次对同样文本操作，应显示"没有新热词"。把 `llm_api_key` 清空重试，应显示 key 未配置提示。

- [x] **Step 6: Commit**

```bash
git add -f desktop/windows/src/win32_app.h desktop/windows/src/win32_app.cc
git commit -m "feat(app): 划词加词接入 LLM 热词提炼流程"
```

---

### Task 7: 设置对话框「热词处理」区块

**Files:**
- Modify: `desktop/windows/src/settings_dialog.h:82-83`（成员）、`:149`（控件 ID）、`UpdateRefinePromptVisibility` 声明附近
- Modify: `desktop/windows/src/settings_dialog.cc:233-235`（命令处理）、`:656-657`（UI 布局）、`:1022-1029`（加载）、`:1093-1111`（保存）

- [x] **Step 1: 头文件成员与 ID**

`settings_dialog.h` 在 `refine_prompt_edit_`（`:83`）之后添加成员：

```cpp
    HWND hotword_process_check_ = nullptr;
    HWND hotword_process_prompt_label_ = nullptr;
    HWND hotword_process_prompt_edit_ = nullptr;
```

在 `kIdSelectionHotword = 2033`（`:149`）之后添加控件 ID：

```cpp
    static constexpr UINT kIdHotwordProcessEnable = 2034;
    static constexpr UINT kIdHotwordProcessPromptEdit = 2035;
```

在 `UpdateRefinePromptVisibility()` 声明旁添加：

```cpp
    void UpdateHotwordProcessPromptVisibility();
```

- [x] **Step 2: UI 布局**

`settings_dialog.cc` 在精修提示词块（`:643-656`）之后、`separator();`（`:657`）之前插入：

```cpp
    // ===== 热词处理 =====
    section_title(StringId::kSettingsSectionHotwordProcess);
    {
        HWND hp_lbl = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, Dp(20), instance_));
        hotword_process_check_ = remember(CreateButton(hwnd_,
            TrW(StringId::kSettingsHotwordProcessEnable, language).c_str(),
            0, 0, ctrl_w, Dp(22), kIdHotwordProcessEnable, instance_,
            BS_AUTOCHECKBOX));
        add(row_h + Dp(10), {
            {hp_lbl, Dp(10), Dp(3), label_w, Dp(20)},
            {hotword_process_check_, ctrl_x, 0, ctrl_w, Dp(22)},
        });
    }
    {
        // 提炼提示词块：仅 hotword_process_check 勾选时显示，隐藏时不占位。
        hotword_process_prompt_label_ = remember_label(CreateLabel(hwnd_,
            label_text(StringId::kSettingsHotwordProcessPrompt).c_str(),
            0, 0, label_w, Dp(20), instance_));
        hotword_process_prompt_edit_ = remember(CreateMultilineEdit(hwnd_, 0, 0, ctrl_w, Dp(64),
                                                                    kIdHotwordProcessPromptEdit, instance_));
        add(Dp(70), {
            {hotword_process_prompt_label_, Dp(10), Dp(3), label_w, Dp(20)},
            {hotword_process_prompt_edit_, ctrl_x, 0, ctrl_w, Dp(64)},
        }, [this]() {
            return SendMessageW(hotword_process_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        });
    }
```

`settings_dialog.cc` 顶部 include 区添加（`BuildExtractPrompt` 用）：

```cpp
#include "hotword_extractor.h"
```

- [x] **Step 3: 命令处理与显隐**

命令处理 switch（`:233-235` `kIdRefineText` case 之后）添加：

```cpp
        case kIdHotwordProcessEnable:
            if (HIWORD(w_param) == BN_CLICKED) UpdateHotwordProcessPromptVisibility();
            return TRUE;
```

`UpdateRefinePromptVisibility()` 实现（`:1290-1293`）之后添加：

```cpp
void SettingsDialog::UpdateHotwordProcessPromptVisibility() {
    // 提炼提示词块的显隐与定位交由 Relayout 统一处理。
    Relayout();
}
```

- [x] **Step 4: 加载与保存**

`LoadSettings()` 中 `UpdateRefinePromptVisibility();`（`:1029`）之后添加：

```cpp
    SendMessageW(hotword_process_check_, BM_SETCHECK,
                 config_.hotword_process_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    {
        std::string src = config_.hotword_process_prompt.empty()
            ? HotwordExtractor::BuildExtractPrompt("")
            : config_.hotword_process_prompt;
        SetWindowTextW(hotword_process_prompt_edit_, Utf16(src).c_str());
    }
    UpdateHotwordProcessPromptVisibility();
```

`SaveSettings()` 中 refine_prompt 保存块（`:1094-1111`）之后添加（CRLF→LF 归一化逻辑与精修同款）：

```cpp
    config_.hotword_process_enabled =
        SendMessageW(hotword_process_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    {
        auto prompt = Utf8(GetWindowText(hotword_process_prompt_edit_));
        // 归一化 \r\n → \n（编辑控件返回 CRLF，LLM 用 LF）。
        std::string normalized;
        normalized.reserve(prompt.size());
        for (std::size_t i = 0; i < prompt.size(); ++i) {
            if (prompt[i] == '\r' && i + 1 < prompt.size() && prompt[i + 1] == '\n') {
                normalized.push_back('\n');
                ++i;
            } else if (prompt[i] == '\r') {
                normalized.push_back('\n');
            } else {
                normalized.push_back(prompt[i]);
            }
        }
        auto default_prompt = HotwordExtractor::BuildExtractPrompt("");
        config_.hotword_process_prompt = (normalized == default_prompt) ? std::string() : normalized;
    }
```

- [x] **Step 5: 构建并跑单测**

增量构建 + 单测命令。预期：PASS。

- [x] **Step 6: 手动运行时验证**

运行 `VoiceStick.exe` → 托盘 → 设置：确认「热词处理」区块出现在「文本精修」之后；勾选 checkbox 出现提示词编辑框（预填默认提示词），取消勾选则收起；勾选并保存后检查 `%APPDATA%\VoiceStick\config.toml` 出现 `hotword_process_enabled = true`；默认提示词未改动时 `hotword_process_prompt` 保存为空串。

- [x] **Step 7: Commit**

```bash
git add -f desktop/windows/src/settings_dialog.h desktop/windows/src/settings_dialog.cc
git commit -m "feat(settings): 设置对话框新增热词处理区块"
```

---

### Task 8: 文档同步与全量验证

**Files:**
- Modify: `README.md`、`README.zh-CN.md`（配置字段说明中 `refine_enabled` 条目附近）
- Modify: `CLAUDE.md`、`AGENTS.md`（「关键配置项」列表）
- Modify: `CHANGELOG.md`（顶部 Unreleased/新版本条目）

- [ ] **Step 1: README 配置说明**

`README.md` 与 `README.zh-CN.md` 中找到 `refine_enabled` / `refine_prompt` 的配置说明位置，在其后追加两个键的说明（英文版写英文，中文版写中文）：

- `hotword_process_enabled`：划词加词时用 LLM 提炼热词（默认 `false`，复用 `llm_*` 配置）。
- `hotword_process_prompt`：提炼提示词覆盖，留空用内置默认。

- [ ] **Step 2: CLAUDE.md / AGENTS.md**

两份文件的「关键配置项」列表中，在 `llm_*` / `refine_enabled` 条目后追加一行：

```markdown
- `hotword_process_enabled` / `hotword_process_prompt`：热词处理（Windows），划词加词时用 LLM 提炼热词，复用 `llm_*` 连接配置；默认关闭。
```

- [ ] **Step 3: CHANGELOG**

`CHANGELOG.md` 顶部追加条目（遵循现有格式）：

```markdown
- 新增热词处理（Windows）：划词加词可选 LLM 提炼，长选文自动提取热词去重入表，浮窗展示结果 3 秒；设置对话框新增「热词处理」配置栏（启用开关 + 提示词，复用文本精修 LLM 配置）。
```

- [ ] **Step 4: 全量构建与测试**

从仓库根目录执行 `build_win.bat` 全量重建，然后运行全部 CTest：

```bash
cmd //c 'ctest --test-dir desktop\windows\build-x64 --output-on-failure'
```

预期：构建成功（核对 `desktop\windows\build-x64\VoiceStick.exe` 时间戳为本次构建），`voicestick_windows_tests` PASS（integration 测试无 key 时 SKIP 属正常）。

- [ ] **Step 5: Commit**

```bash
git add README.md README.zh-CN.md CLAUDE.md AGENTS.md CHANGELOG.md
git commit -m "docs: 热词处理功能文档同步（README/CLAUDE/AGENTS/CHANGELOG）"
```

---

## Self-Review 记录

- Spec 覆盖：配置模型→Task 1；HotwordExtractor→Task 2；长度上限→Task 3；浮窗 3 秒展示/提炼中→Task 4+6；本地化→Task 5；运行时流程（含失败兜底/全重复）→Task 6；设置区块→Task 7；example.toml→Task 1；测试→Task 1/2/4/5 的 ctest 步骤。腾讯 ASR `cached_vocab_id_` 路径按 spec 不改动。
- 类型一致性：`BuildExtractPrompt` / `ParseExtractResult` / `DiffNewHotwords` / `ShowTimedMessage` / `ProcessHotwordWithLlm` / `OnHotwordExtracted` / `SetMaxLength` / `kMaxProcessLen` 在各 Task 间签名一致。
- `ChatAsync` 已确认拷贝配置后 detached 线程执行（`llm_chat_client.cc:132-147`），Task 6 栈上临时 `HotwordExtractor` 安全。
