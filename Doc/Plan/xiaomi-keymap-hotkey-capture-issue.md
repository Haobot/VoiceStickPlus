# 小米遥控器按键映射：「录入」状态识别不到键盘按键（缺陷调查）

- 状态：**修复建议 1/2 已落码（未提交），机制层已证伪，根因待真机证据**
- 报障日期：2026-09-07 晚（用户日志时段 23:09–23:25）
- 关联模块：`desktop/windows/src/xiaomi_keymap_dialog.cc`、`shortcut_capture.cc`、`xiaomi_keymap_hook.cc`
- 关联 GitHub Issue：见仓库 Issue 列表「录入状态识别不到键盘按键」

## 0. 修复落地记录（2026-09-08 凌晨，自动会话）

按 §4 建议落地以下改动（**TDD：红灯→绿灯**，UI 字符串断言先行，`core_tests.cc:1374-1378`）：

1. **`ShortcutCapture` 排障日志**（`shortcut_capture.{h,cc}`，未跟踪文件直接改）：
   `Start` 成功、**首个键盘事件到达**（一次性打点）、`captured`/`cancelled`/`rejected` 回调触发点。
   下次复现可直接分叉「钩子收不到事件（环境隔离）」vs「收到但链路没走通（代码缺陷）」。
2. **录入超时提示**（两个捕获对话框同模式）：
   `xiaomi_keymap_dialog.{h,cc}` + `hotkey_settings_dialog.{h,cc}`——录入启动后 `SetTimer` 3 秒，
   无任何键盘事件则弹一次 UIPI 引导（`kHotkeyCaptureTimeoutTitle/Body`，中英双语），不中断捕获；
   捕获结束的公共汇合点（`RestoreCaptureButtonText`/`UpdateHotkeyDisplay`）与 `WM_DESTROY` 停表。
3. **本地化**：`localization.{h,cc}` 新增两条 StringId 与中英文案（diff 干净，仅本改动）。

**验证状态与中断原因**：
- 全量构建通过（BUILD_EXIT=0，含双方改动）。
- **CTest 未全绿即中断**：`TestClipboardVaultMultiFormatRoundTrip`（`core_tests.cc:10533`）失败。
  归属分析：该失败位于并行会话（local-mic 迭代三「剪贴板恢复」）**进行中的新模块 `clipboard_vault`**
  （23:56 仍在改 `clipboard_vault.cc`、CMakeLists 注册、`watch_tests.ps1` 监视），且两会话的测试进程
  曾并发互踩剪贴板（全局资源）。本改动的相关断言（UI strings，1374 行起）位于失败点之前、**全部通过**。
- **源码未提交**：`core_tests.cc` 的工作区 diff 混有并行会话 ~190 行（剪贴板/local-mic 测试），
  提交会夹带其半成品。待并行会话收尾（或用户裁决）后，以本节清单为界补提交并补跑全量 CTest。
- **未重启 VoiceStick.exe 真机冒烟**：避免占用 exe 锁干扰并行会话的后续构建（LNK1104 教训）。
  冒烟清单：托盘→按键映射→录入→不按键等 3 秒看提示→按键看捕获→查日志四条打点。

## 1. 现象

## 1. 现象

小米按键自定义设置对话框（`XiaomiKeymapDialog`）：选中任一可映射按键，点「录入」进入热键识别状态后，按**物理键盘**按键，软件端无任何反应——映射值不更新、「录入」按钮文案停留不变。

## 2. 排查过程与证据

### 2.1 静态链路审查（全部通过，无嫌疑）

- `ShortcutCapture`（`WH_KEYBOARD_LL`，对话框点录入时后装）：修饰键 keydown 累积并吞掉；非修饰键 keydown 触发 `on_captured` → `MakeKeySpecFromVk` → `RefreshSidePanel`。`require_modifier=false`（keymap 场景）时单键也接受，`VkeyDisplayName` 对未知 VK 退化为 `VK0xXX` 文本，不存在「捕获成功但显示为空」路径。
- 同进程共 4 个 LL 键盘钩子（`XiaomiKeymapHook` / `MicModeHotkey` / `VoiceF5Suppressor` / `ShortcutCapture`），前三个均只处理自己的目标键集，非目标键一律 `CallNextHookEx` 放行；且 `ShortcutCapture` 后装、位于钩子链头、**最先**收到事件，不存在被前面钩子吞掉的可能。
- 对话框模态循环（`DialogBoxIndirectParamW`）与钩子安装同在主线程，LL 回调可正常派发（`win32_app` 无自建 UI 线程）。
- `ShortcutCapture::active_instance_` 进程级单例：同一时间仅一个对话框实例持有捕获，无覆盖路径。

### 2.2 动态复现（spike ×2，均捕获成功 → 机制层证伪）

用工作区当前源码直编的独立复现程序（模拟键盘事件为 `SendInput` 注入，`ShortcutCapture` 不过滤 `LLKHF_INJECTED`，与物理键等价）：

| 环境 | 形态 | 结果 |
|---|---|---|
| spike1-裸 | 仅 `ShortcutCapture::Start` + 泵消息 + 注入 `A` | `on_captured vk=0x41` ✅ |
| spike1-共存 | `XiaomiKeymapHook` 常驻 + `ShortcutCapture` 后装（真实钩子链形态） | `on_captured vk=0x41` ✅ |
| spike2-对话框 | 完整复刻运行形态：`DialogBoxIndirectParamW` 模态循环 + `WM_COMMAND(kIdCapture)` 走 `StartCapture` 等价路径 + `XiaomiKeymapHook` 常驻 + 后台线程 1s 后注入 `A` | `on_captured vk=0x41` ✅ |

结论：**当前代码的捕获机制（钩子安装、链序、模态循环内回调派发、UI 回写）功能正常**，「识别不到」无法在机制层复现。

### 2.3 运行日志（%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log）

- 用户测试时段（23:09–23:25）仅见 `XiaomiKeymapHook` 的 `swallow-down` / `break-remote-inject`（home/up），无任何 `ShortcutCapture` 记录。
- **盲区**：`ShortcutCapture` 成功/取消路径完全无日志（仅 `SetWindowsHookExW` 失败时记一条），无法从日志判定当时钩子是否收到事件。

## 3. 剩余根因假设（按可能性排序）

1. **UIPI 隔离（最可能）**：`VoiceStick.exe` 清单为 `asInvoker`（普通完整性）。若按键时**前台窗口属于高完整性（管理员）进程**——提权终端、任务管理器、提权编辑器——普通权限进程的 LL 键盘钩子收不到该事件（Windows 既有行为，键盘/鼠标钩子均受 UIPI 边界限制）。开发者本机常开提权终端，复现概率高。
   验证方式：进入录入状态后，先点一下普通权限窗口（如记事本）或对话框本身使前台为普通权限，再按键——若能捕获即坐实。
2. 旧构建差异：用户测试时段运行的 exe 早于 23:27 构建，但 `shortcut_capture.cc`（09-02）/`xiaomi_keymap_dialog.cc`（09-03）此后未改动，机制一致，可能性低。
3. 第三方安全软件/输入法全局钩子干预：低概率（若发生会波及系统全局键盘行为，与现象不符）。

## 4. 修复建议

1. **补排障日志**（`ShortcutCapture`）：`Start` 成功、收到首个键盘事件、`on_captured` / `on_cancelled` 触发点各一条——下次复现可从日志直接分叉「钩子未收到事件（环境隔离）」vs「收到但 UI 未更新（代码缺陷）」。
2. **录入态超时提示**：进入识别状态 2 秒仍无任何键盘事件到达时，提示用户「若前台是管理员权限（提权）窗口，键盘事件被系统隔离，请先点击本对话框或任意普通窗口再试」。
3. 文档化 UIPI 与 LL 键盘钩子的边界行为（`Doc/Ref/` 或 FAQ）。

## 5. 附：spike2 复现程序骨架（复测用）

```cpp
// 形态：XiaomiKeymapHook 常驻 + DialogBoxIndirectParamW 模态对话框 +
// WM_COMMAND(kIdCapture) 内 capture_.Start(require_modifier=false) +
// 后台线程 1s 后 SendInput 'A' → 模态循环内 on_captured vk=0x41 触发。
// 全部源文件直编：shortcut_capture.cc / xiaomi_keymap_hook.cc /
// xiaomi_keymap_interceptor.cc / key_spec.cc / log.cc（log.cc 需 stub
// AppConfig::DefaultDebugAudioDirectory 指向临时目录）。
```
