# RFC：ASR 文本 LLM 精修（Windows 端）

## 背景与目标

VoiceStick Windows 端已调通 ASR → LLM 翻译 → 输出链路。但 ASR 裸文本直通输出，存在三类问题：

1. 说话停顿会在识别文本里留下多余空格（尤其 CJK 字符之间的空格）。
2. 标点缺失或位置不当。
3. 含「嗯」「啊」「那个」等无意义口头语和口吃重复。

目标：在 ASR 出文本后、输出（粘贴/字幕）前，复用现有 LLM 能力过一道「精修」，产出更准确自然的书面文本，且不破坏已工作的翻译链路。

## 设计决策

- **作用范围**：原文直出路径走独立精修调用；翻译路径把精修要求融入翻译 prompt（不额外加调用，避免双倍延迟）。
- **默认开关**：`refine_enabled` 默认 `true`（开箱即用，设置里可关）。
- **Prompt**：内置硬编码精修 prompt，同时提供 `refine_prompt` 配置项允许覆盖。
- **失败回退**：精修是 best-effort，LLM 失败时回退原文输出，不报错（区别于翻译失败报错）。

## 现状链路

- ASR final 文本出口：`asr_client_win.cc` `on_final(final_text)`，无任何清理。
- 协调器入口：`VoiceStickCoordinator::FinishWithFinalText`（`voice_stick_coordinator.cc:771`）。
  - 分支 C 翻译：`TransformText` → `EnterPendingConfirmation(result)`。
  - 分支 D 原文：直接 `EnterPendingConfirmation(text)`。
- `TransformText`（`voice_stick_coordinator.cc:985`）是聚焦与字幕两条路径的共同出口；原文分支当前 `completion(true, text)` 直通。
- 现有 LLM 封装：`LLMTranslationClient`，OpenAI 兼容 `/chat/completions` + WinHTTP + detached thread 异步 + `completion(bool,string)` 回调。`SystemPrompt` 硬编码。
- 配置：`AppConfig` 扁平顶层键 `llm_base_url`/`llm_api_key`/`llm_model`；`OutputProfile` 含 `transform`/`translation_target`。
- `translator_` 是值成员，构造函数与 `UpdateConfig` 从 `config_` 重建。

## 实现

### 配置项（`app_config.h` / `app_config.cc`）

- `AppConfig` 新增 `bool refine_enabled = true;` 与 `std::string refine_prompt;`。
- `Load()`/`Save()`/`ApplyConfigValue` 加 `refine_enabled`、`refine_prompt` 键。

### 精修客户端（新增 `llm_refinement_client.h` / `.cc`）

- 结构平行于 `LLMTranslationClient`：`Refine(text, prompt_override, completion)` 异步 + `RefineSync` 同步。
- 复用网络层模式（URL 解析 / WinHTTP / cJSON 解析）。
- 可单测纯函数：`BuildRefinePrompt(prompt_override)`（override 非空返回 override，否则内置默认）、`BuildChatPayload(model, system_prompt, user_text)`。
- 内置默认精修 prompt：
  ```
  You are a speech-recognition post-processor.
  The input is raw text from automatic speech recognition. Rewrite it into clean written text:
  - Remove stray spaces inserted at speech pauses, especially spaces between CJK characters; keep legitimate spaces between words and around inline Latin text or numbers.
  - Fix punctuation: add missing punctuation, correct misplaced ones, remove redundant punctuation, following the conventions of the text's language.
  - Remove filler words, false starts, stutters, and meaningless colloquial fragments (e.g. "嗯", "啊", "那个", "就是", "uh", "um", "you know") when they carry no meaning.
  - Preserve the original meaning, language, and tone. Do not translate or expand the content.
  - Keep proper nouns, numbers, code, and technical terms intact.
  Return only the cleaned text, with no explanations, quotes, prefixes, alternatives, or markdown.
  ```

### 翻译 prompt 融合精修（`llm_translation_client.cc` `SystemPrompt`）

- 在现有翻译 prompt 末尾追加：
  ```
  Before translating, clean the source text: remove stray pause spaces (especially between CJK characters), fix punctuation, and drop filler words/false starts that add no meaning. Preserve proper nouns and numbers.
  ```
- 不改 `Translate` 签名。

### 协调器接入（`voice_stick_coordinator.h` / `.cc`）

- 新增成员 `LLMRefinementClient refiner_;`（构造函数与 `UpdateConfig` 同机制重建）。
- 改造 `TransformText`：
  ```cpp
  if (profile.transform == TextTransform::kTranslate) {
      translator_.Translate(...); return;
  }
  if (config_.refine_enabled && !text.empty()) {
      refiner_.Refine(text, config_.refine_prompt,
          [text, completion](bool ok, std::string result) mutable {
              completion(true, ok ? result : text);  // best-effort 回退原文
          });
      return;
  }
  completion(true, text);
  ```
- `FinishWithFinalText` 分支 D 原文路径进入前，若 `refine_enabled && transform==kOriginal` 调 `ui_->SetStatus("Refining")`。

### 设置 UI（`settings_dialog.cc` + `localization`）

- 新增 `StringId::kSettingsRefineText`（中英两语）。
- 新增 checkbox 绑定 `refine_enabled`。
- `refine_prompt` 暂不暴露 UI（进阶用户改 `config.toml`）。

### 重构

- 抽公共基类 `LLMChatClient`，把 `ChatSync`/`ChatAsync`/URL 解析/WinHTTP/cJSON/`JsonEscape`/`Trim`/Utf16 下沉。
- `LLMTranslationClient`/`LLMRefinementClient` 继承，仅保留各自 `BuildSystemPrompt`。
- 纯函数测试保持全绿。

## 验证

1. 构建：`build_win.bat`。
2. 测试：`ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests`。
3. 端到端：录音带停顿/口头语中文，确认粘贴已清理；翻译路径同样清理；字幕路径精修；关闭 `refine_enabled` 回退原文；断网回退原文不报错。
4. 测试通过后打包 MSI 并提交 Git。

## 可选增强（不在本次范围）

- 引入 `LLMClient` 抽象接口，协调器持有 `unique_ptr<LLMClient>`，支持 mock 注入，使精修/翻译流转可单测。
