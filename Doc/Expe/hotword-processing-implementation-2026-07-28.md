# 热词处理功能实施：成果与教训（subagent 驱动开发全流程）

日期：2026-07-28。范围：Windows 端热词处理（划词加词 LLM 提炼）。
Spec：`docs/superpowers/specs/2026-07-28-hotword-processing-design.md`；
Plan：`docs/superpowers/plans/2026-07-28-hotword-processing.md`。
提交：`main` 上 `c1aa82d..4d67e19` 共 10 个提交（feat/config/core/selection/overlay/l10n/app/fix/settings/docs）。

## 成果摘要

- 设置新增「热词处理」栏（`hotword_process_enabled` / `hotword_process_prompt`，复用 `llm_*`），启用后划词加词改为 LLM 提炼：只把提炼结果去重写入 `asr_hotwords`，浮窗展示 3 秒；划词上限 64→2000 字节（启用时）。
- 新组件 `HotwordExtractor`（voicestick_core，静态纯函数可单测）；`OverlayWindow::ShowTimedMessage` + `Mode::kInfo`；`VoiceStickCoordinator::HasActiveSession()`。
- 全流程：brainstorm → spec → plan → subagent 逐任务实现 + 规格/质量双审 → 整体审查 → 合并。全量 CTest 2/2 PASS（含真实 ASR 集成测试）。

## 教训与速查

### 构建环境

- **提升权限运行的 VoiceStick.exe 会锁定链接产物，`build_win.bat` 杀不掉且仍报「Build SUCCEEDED」**。本次第一次全量构建就是假成功（exe 时间戳还是旧版）。判据永远是 `desktop\windows\build-x64\VoiceStick.exe` 的时间戳与体积，不是脚本输出。需用户从托盘手动退出后重建。
- **Git Bash 里 `cmd //c` 内联命令 stdout 偶发丢失、`&&` 链重定向不稳定**：验证类命令写成临时 `.bat` 落盘跑（用后删除）更可靠。
- **`ctest` 不在裸 cmd PATH**：用 VS BuildTools 全路径 `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe`。

### 架构（overlay 共享资源）

- **`OverlayWindow` 的 `kAutoHideTimerId` + `pending_callback_` 是全局共享资源**：任何旁路（非协调器状态机）调 `Show*()` 都会 KillTimer 并覆盖回调——确认倒计时的自动粘贴回调就是这样被踩掉的（Task 6 质量审查抓到的 Important，spec 原本没考虑）。规则：**旁路反馈（热词、通知类）在 `coordinator_->HasActiveSession()` 时必须改走托盘气泡 `ShowNotification`，不得碰 overlay**。
- `ChatAsync` 拷贝 config 后 detached 线程执行、不持有 this → 栈上临时 `HotwordExtractor(config_)` 安全；回调经 `DispatchToUi`（PostMessage）回 UI，hwnd 销毁后 PostMessage 失败自动清理 heap lambda，无悬挂回调。此模式与精修/翻译链路一致，可复用。

### 本地化机制

- 新增 `StringId` 必须同步 `localization.cc` 的 `kStringCount`（末位枚举 + 1）；表是 `std::array` 按枚举索引填充，漏填条目会被 `LocalizationTablesAreComplete()` 单测抓住（该测试只查非空，查不出张冠李戴，diff 仍需人工比对文案）。

### 流程（subagent 驱动开发）

- **两阶段审查确实抓得到 spec 级缺陷**：overlay 双写冲突（质量审查）、「测试跑在陈旧二进制上」（规格审查）都是审查环节而非实现环节发现的。实现者的「测试通过」汇报不可尽信，审查方要核对测试二进制时间戳。
- **用户并行会话会切分支/提交**：subagent 每次 git 操作前先 `git branch --show-current` 确认；本次分支被并行会话切走两次。
- 提示词中写死构建/测试命令、已知环境坑（LNK1104、stdout 丢失、`git add -f`）能显著减少 subagent 的无效挣扎。

## 可复用资产

- `VoiceStickCoordinator::HasActiveSession()`：会话活跃判断，任何要碰 overlay 的旁路都应先查它。
- `OverlayWindow::ShowTimedMessage(text, duration_ms, on_complete={})`：一次性定时消息（3 秒反馈范式）；新消息会覆盖前一条及其回调。
- `HotwordExtractor::ParseExtractResult` / `DiffNewHotwords`：LLM 词列表输出的解析与增量去重，模式可用于其他「LLM 输出结构化列表」场景。
- `SelectionHotwordManager::kMaxHotwordLen` / `kMaxProcessLen` + `SetMaxLength`：划词长度上限的运行时切换。

## 遗留（低危）

- 长度限制均为 UTF-8 字节语义（中文热词实际限约 21 字）；`macOS config.example.toml` 的 `hotword_process_enabled` 行缺 Windows-only 注释（纯注释）。
