# 热词处理（Hotword Processing）设计文档

日期：2026-07-28
范围：仅 Windows 桌面端（macOS 端暂无划词加词/文本精修基础能力，后续单独立项）

## 背景

Windows 端已有「划词加词」功能（`selection_hotword_manager.cc/h`）：全局鼠标钩子监听 `WM_LBUTTONUP`，通过 UI Automation 读取焦点控件选中文本，弹出浮层「添加到热词」按钮；点击后去重写入 `asr_hotwords` 并持久化，托盘气泡反馈。目前超过 64 字符的选中会被直接忽略。

当用户选中一大段文字（如会议纪要、文章）想加入热词时，整段入表没有价值。本功能在「添加到热词」动作中插入一步 LLM 提炼：自动从长文中提取可能的热词（专有名词、人名、术语等），只把提炼结果写入热词表，并在浮窗展示 3 秒。

## 已确认的需求决策

- **落地范围**：仅 Windows 端。
- **写入策略**：只把 LLM 提炼出的词写入热词表，原文不入表。
- **失败兜底**：LLM 调用失败（无 key / 超时 / 空结果 / 解析为空）时浮窗报错 3 秒，热词表不变，不回退加原文。
- **长度上限**：热词处理启用时划词上限从 64 放宽到约 2000 字符；未启用时保持 64 字符现状。
- **去重展示**：提炼结果中与现有热词表重复的词不再显示；全部重复时显示"没有新热词"提示。

## 配置模型

`AppConfig`（`desktop/windows/src/app_config.h/.cc`）新增两个字段，复用文本精修的 TOML 读写模式：

- `hotword_process_enabled`：bool，默认 `false`。
- `hotword_process_prompt`：string，空串表示使用内置默认提示词（与 `refine_prompt` 同语义）。

LLM 连接参数复用文本精修的 `llm_base_url` / `llm_api_key` / `llm_model`，不新增配置项。

`desktop/macos/Config/config.example.toml` 同步补充注释示例（标注 Windows-only）。

## 核心组件 `HotwordExtractor`（voicestick_core）

新文件 `desktop/windows/src/hotword_extractor.h/.cc`，纳入 `voicestick_core` 目标。

接口：

- `Extract(text, prompt_override, callback)`：构造提炼请求（system prompt + 原文作为 user 消息），调用 `LLMChatClient::ChatAsync`，完成后回调返回词列表或错误。
- `BuildExtractPrompt(prompt_override)`：静态方法。覆盖值 Trim 后非空则用覆盖值，否则返回内置默认提示词。默认提示词大意："从用户给出的文本中提取应该作为语音识别热词的专有名词、人名、术语等，每行输出一个词，只输出词本身，不要编号、不要解释"。
- `ParseExtractResult(response)`：按行/逗号切分、Trim、去空、单个词限长 64 字符、总量限 20 个，切分逻辑复用 `ParseHotwordList`。
- `DiffNewHotwords(extracted, existing)`：返回不在现有热词表中的新词（保序去重）。

## 运行时流程（`win32_app.cc`）

`on_add_hotword` 回调按 `config_.hotword_process_enabled` 分支：

1. **未启用**：走现有逻辑（64 字符上限、直接入表、托盘气泡），行为完全不变。
2. **启用**：
   - 划词长度上限放宽到 2000 字符（`SelectionHotwordManager` 的上限改为构造时按配置取值）。
   - 点击浮层按钮后立即在 overlay 显示"热词提炼中…"（复用 `OverlayWindow` 的 refining 类展示模式）。
   - 异步提炼完成 → `DiffNewHotwords` 与 `config_.asr_hotwords` 去重 → 新词追加 + `SaveConfig` → overlay 显示"已添加热词：词1、词2…"3 秒自动隐藏（复用 `kAutoHideTimerId` 定时器模式，参考 `ShowError` 的 2 秒实现，时长参数化为 3000ms）。
   - 全部重复：overlay 显示"没有新热词（提炼结果均已存在）"3 秒。
   - 失败：overlay 显示错误信息 3 秒，热词表不变。
3. 腾讯 ASR 侧热词表变化后的 `cached_vocab_id_` 处理与现有人工加词路径一致（下次会话重新同步），不额外改动。

线程模型：LLM 回调在后台线程完成，回到 UI 线程更新 overlay 与配置（沿用精修的回切模式）。

**会话冲突处理（实现期补充）**：协调器会话活跃（录音/识别/确认倒计时等，`VoiceStickCoordinator::HasActiveSession()`）期间，浮窗被状态机占用，热词处理的全部反馈（含无 key、失败、空结果、全重复、成功）改走托盘气泡（标题「热词处理」），且提炼期间不显示"热词提炼中…"浮窗指示；会话不活跃时维持浮窗 3 秒反馈。

## 设置对话框

`desktop/windows/src/settings_dialog.cc` 在「文本精修」区块之后新增「热词处理」区块，形态复刻精修区：

- checkbox（`hotword_process_check_`，`BS_AUTOCHECKBOX`）。
- 仅勾选时显示的多行提示词编辑框 + label（`UpdateHotwordProcessPromptVisibility()` + `Relayout` 动态布局，隐藏时不占位）。
- 加载时 `hotword_process_prompt` 为空则预填内置默认提示词；保存时内容与默认一致则存空串，否则存归一化后的覆盖值。

## 错误处理

| 场景 | 行为 |
|---|---|
| 未配置 `llm_api_key` | overlay 报错 3 秒，不加词 |
| LLM 超时/网络失败 | overlay 报错 3 秒，不加词 |
| 返回为空 / 解析后无有效词 | overlay 提示"未提炼出热词"3 秒，不加词 |
| 提炼结果全部重复 | overlay 提示"没有新热词"3 秒，热词表不变 |
| 选中超过 2000 字符（启用时） | 不弹「添加到热词」按钮（与现状 64 字符忽略一致，仅阈值不同） |

## 测试

`desktop/windows/tests/core_tests.cc` 新增 `TestHotwordExtractor*()` 函数并注册进 `main()`：

- 默认提示词构建（覆盖值为空/空白时返回内置默认）。
- 覆盖提示词构建（非空覆盖值原样返回）。
- 结果解析：换行分隔、逗号分隔、空行、前后空白、超长词过滤（>64 字符）、总量截断（>20 个）。
- `DiffNewHotwords`：全新增、全重复、部分重复、保序。

LLM 网络层用现有 Fake 模式 mock，不联网。UI 流程（浮层按钮 → overlay 展示）走运行时手动验证。

## 不做的事（YAGNI）

- macOS 端实现（需先补划词/精修基础，另立项）。
- 提炼结果的用户二次确认/编辑界面（直接写入，失败重选即可）。
- 原文+提炼结果双写、短词直加分支（已被需求决策排除）。
- 热词处理专用的独立 LLM 连接配置（复用精修配置）。
