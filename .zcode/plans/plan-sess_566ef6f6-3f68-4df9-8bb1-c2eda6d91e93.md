# 微信 targeted 间歇失败修复：MMUI accessibility 冷态重试 + 诊断日志

## Context（根因，已实锤）

微信 4.x 聊天内容区是单个自绘渲染窗口 `MMUIRenderSubWindowHW`（Qt 壳 `Qt51514QWindowIcon` 内，**无 CEF 子窗口**），其 accessibility 树**按需激活、超时回收**：

- **冷态**（闲置约 25 分钟后，如 13:20 与探测时刻）：`WM_GETOBJECT` 返回 0、MSAA `E_FAIL`、UIA 查不到任何 TextPattern 元素 → `ReadFocusedFieldFullText` 在 `GetCurrentPatternAs` 处 5ms 内静默失败 → targeted 被 `verify_unavailable` 拒绝（coordinator_tail.cc:1304）
- **热态**（使用中，如 11:52/12:55）：读全文/GetSelection/终验全通，targeted 正常工作
- 冷→热转换是**异步**的：首次 UIA 查询充当激活信号，但树构建需要时间；VoiceStick 当前单次查询失败即放弃，等不到树建好
- client 侧定点查询（后台窗口）无法激活——激活钩子在焦点路径上；但使用中树保持热说明「重试等树建好」可行

另：微信聊天窗口在中途被重建过（hwnd 265424→396398），新窗口重建后同样从冷态开始。

## 改动清单

### 1. coordinator 冷态重试（核心修复）
`desktop/windows/src/voice_stick_coordinator_tail.cc` + `voice_stick_coordinator.h`

- `TakeOverTargetedCommand` 签名加 `int reread_attempt = 0` 参数
- 批次 2 路径（1295-1311）`!field_read || !field.readable` 且**非** `full_truncated`/`is_password` 时：按项目既有异步重试范式（`kCaretMovePollDelays` 同构，见 tail.cc:1049-1106）——detached 线程 sleep → `RunOnUiThread` → 守卫（`alive->load()`、`is_shutdown_`、`session_state_ == kFinalizing`）→ 以 `reread_attempt+1` 重入 `TakeOverTargetedCommand`（入口副作用 `pasted_final_text_=true`/`pending_clarification_.reset()` 幂等）
- 延迟表：`static constexpr {100, 200, 400, 800}ms`（与终验表 `kVoiceTargetedVerifyPollDelays` 同节奏，总预算 1.5s，远小于 finalizing 超时）
- 用尽后才走现有 `reject(verify_unavailable)`；首次失败与每次重试各记一行日志（`voice targeted field reread attempt=N`），成功记收敛行
- **第一期不动**（注释 TODO 注明）：gate 调用点①（tail.cc:1133，失败退化为普通文本上屏，不违反安全模型）与 clarify 调用点③（tail.cc:1935，刚交互过树为热态，概率低）

### 2. reader 静默分支诊断日志（防复发）
`desktop/windows/src/focus_text_reader.cc`

`ReadFocusedField` / `ReadFocusedFieldFullText` 的静默失败分支补日志（失败才打，不刷屏）：
- `GetCurrentPatternAs` 拿不到 TextPattern：记焦点元素 `ClassName/Name(截断16字符)/ProcessId/ControlType` —— 微信冷态实锤落点，一眼区分「焦点漂移」vs「树未激活」
- `selection_count == 0`：同上
- 全文路径 `Clone`/`MoveEndpointByUnit`/`GetText` 失败：记步骤名 + hr

### 3. TDD：fake 冷态可编程 + 用例
`desktop/windows/tests/core_tests_voice_edit.cc`

- `VoiceTargetedFakeFocusReader` 加 `int fulltext_fail_streak = 0;`（ReadFocusedFieldFullText 前 N 次返回 false 后正常，模拟冷态→热态收敛）
- 用例一（先红后绿）：streak=3 → `RunCommand("删除刚才的北京")` 经 `PumpUntil`（现有 3s 泵机制）收敛到 result=ok，field 终态正确、ops 含整框替换序列
- 用例二：恒失败（streak=999）→ 用尽重试后 rejected `verify_unavailable` + toast + **零写入 ops（不盲改）**
- 用例三：`full_truncated=true` 不重试，直接 `field_too_long`（防止重试语义扩散）

按项目惯例（04b8366 先例）测试与实现同一 commit，开发过程遵守红-绿顺序。

## 验证

1. `build_win.bat` 构建（Git Bash 需 `export PATH="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:$PATH"`）
2. `ctest --test-dir desktop/windows/build-x64 --output-on-failure`（跑前提权 taskkill VoiceStick.exe pid 33596）
3. 重启 VoiceStick.exe（项目惯例自动重启）+ 查 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 确认新二进制
4. **真机微信复现**：保持微信闲置使树冷却 → 听写「我要去北京吃烤鸭」→「把北京改成上海」→ 期望日志：`field reread attempt=N` 收敛 → `whole-field paste applied` → `result=ok`；若用尽仍失败，新诊断日志给出焦点元素 ClassName/Name，为进一步处置提供实锤
5. 收尾：`Doc/Expe/wechat-mmui-a11y-ondemand-2026-11-04.md` 沉淀（按 work-summary-retro 惯例），探测脚本归档说明

## 边界与安全模型

- 重试仅针对「读不可用」冷态，**不放宽任何验证**：终验/读回轮询/防篡改逻辑零改动，验证不过仍拒绝
- 重试期间 `session_state_` 保持 kFinalizing（设备 thinking），新会话/关机由守卫安全放弃
- 不做「无读通道盲改」类降级（违反「验证不过绝不盲改」红线，已明确否决）
