# 修复微信 4.0 等高完整性窗口文本注入失败（UIPI / UIAccess 方案）

> **状态更新（2026-07-06）：本方案已撤销。** VoiceStick.exe 清单回退为 `asInvoker`，不再以
> 管理员启动，也不弹 UAC；开机自启从任务计划程序改回标准 `HKCU\...\Run`。
>
> 原因：微信输入法语音模式（走虚拟麦克风渲染，不依赖窗口注入）成为主要输入路径，
> UIPI 注入不再必需，管理员启动带来的 UAC 与任务计划程序复杂度不再划算。
>
> 权衡：`focused_app` 粘贴注入模式无法再向微信 4.0 等高完整性窗口发送 `SendInput`（向浏览器等
> 同级 Medium IL 窗口仍正常）；微信输入法模式不受影响。未来若需恢复高完整性窗口注入，应走
> UIAccess（uiAccess=true）+ 权威 CA 签名证书路径（本机无法验证，需签名机处理）。
>
> 迁移细节：`SyncLaunchAtLogin` 内以 `std::call_once` 调用 `schtasks.exe /Delete /TN
> VoiceStickAutoStart /F`，清理历史 requireAdministrator 方案遗留的任务计划程序任务，避免它仍
> 以 RunLevel=Highest 权限拉起本 asInvoker exe（否则去管理员后仍会弹 UAC）。
>
> 下方原始记录保留作为决策背景与权衡依据。

## 背景

用户反馈：语音识别完成的文字可以注入浏览器在线文档，但**无法注入微信 4.0 输入框**。

经决定性实验定位根因（非猜测，有实测证据）：

1. 剪贴板写入成功 —— 识别完成后在微信输入框手动按 `Ctrl+V` 能粘贴出那段文字。
2. 焦点确实在微信输入框。
3. 合成 `Ctrl+V`（`SendInput`）完全无效，真实 `Ctrl+V` 有效。
4. 进程完整性实测（`diag_integrity.ps1`）：VoiceStick 为 **Medium**（RID=8192）；微信 4.0
   连 `OpenProcess(QUERY_INFORMATION)` 都被拒，强烈提示其以 **High** 完整性运行。
5. **以管理员身份运行 VoiceStick 后，微信输入框立即能注入文字** —— 根因确认。

## 根因

**UIPI（User Interface Privilege Isolation）**：低完整性进程通过 `SendInput` 合成的键盘事件，
会被系统静默丢弃，无法到达高完整性窗口。真实键盘事件来自内核，不受 UIPI 限制 —— 这解释了
"手动 Ctrl+V 有效、合成 Ctrl+V 无效、浏览器（Medium 同级）能用"的全部现象。

关键结论：
- `SendInput`、`PostMessage(WM_PASTE/WM_CHAR/WM_KEYDOWN)` 均**同等**受 UIPI 限制。换"模拟
  打字"（逐字 `WM_CHAR`）**不能绕过** —— UIPI 挡的是"来自低 IL 进程的所有合成输入"，与消息
  类型无关。管理员实验已间接证明（提权后粘贴即通 → 起作用的是 UIPI 这道完整性隔离）。
- 因此"不提权、纯换注入 API"在原理上不可行，必须**提权**或**授予 UIAccess**。

## 方案选型（已与用户确认）

| 方案 | 结论 |
|------|------|
| **requireAdministrator 清单** | ✅ 采纳。已真机验证提权后能注入微信，本地可测。代价：每次启动弹 UAC；自启改任务计划程序。 |
| UIAccess（uiAccess=true） | ❌ 不采。本机自签名证书跑不通：普通启动报 Win32 740（需提权），`RunAs` 提权仍报 referral 失败；无权威 CA 证书无法本地验证。等签名机就绪可作未来优化。 |
| 模拟键盘打字（不提权） | ❌ 不可行。原理上同样被 UIPI 挡。 |

### 为何放弃 UIAccess

实测：自签名证书（已装 LocalMachine\Root + CurrentUser\Root）签名的 uiAccess exe，从 Program Files
启动时，`CreateProcess` 返回 error 740（需提权），`ShellExecute runas` 返回 referral 失败。
UIAccess 对签名链的校验比预期严格，开发机无权威 CA 证书无法跑通，且改完代码无法本地验证是否
真正解决微信问题，风险高。requireAdministrator 已真机验证有效，作为确定方案。

## 设计

### 改动1：嵌入 requireAdministrator 清单

`desktop/windows/resources/VoiceStick.manifest` 声明 `level="requireAdministrator"`，外加 DPI 感知
与 Win10/11 兼容性声明。CMakeLists 用 post-build `mt.exe` 把清单写入 exe 资源 ID 1
（`CREATEPROCESS_MANIFEST_RESOURCE_ID`），覆盖 CMake 默认清单。VoiceStick.rc 不声明 RT_MANIFEST，
避免与 mt.exe 重复嵌入。

### 改动2：开机自启改用任务计划程序

`HKCU\...\CurrentVersion\Run` 启动提权程序会被 UAC 阻挡，自启失效。`Win32App::SyncLaunchAtLogin`
改用任务计划程序 COM（taskschd.h）：
- 开启：注册任务 `VoiceStickAutoStart`，`TASK_TRIGGER_LOGON` + `TASK_ACTION_EXEC`（当前 exe 路径）
  + `TASK_RUNLEVEL_HIGHEST`（最高权限）+ `StartWhenAvailable=true`。
- 关闭：`DeleteTask`，不存在时忽略。
- 便携模式仍跳过。
- 链接 `taskschd.lib`。

### 不改动的部分

- 注入路径（`input_injector_win.cc`）：提权后即工作。
- WinSparkle、全局热键、BLE、ASR、协调器：不涉及。
- VoiceStickCtl：不注入文字，不提权。

## TDD 计划

本改动核心是清单资源 + COM 自启注册，无可单测的业务逻辑（注入代码不变）。`voicestick_core`
单测保持现状确保不回归；自启 COM 靠真机验证。

## 真机验证清单

1. `build_win.bat` 构建 + `ctest` 全绿。
2. 启动 VoiceStick → 弹 UAC → 确认提权（任务管理器标"管理员"）。
3. 焦点放微信 4.0 输入框，语音输入 → 文字注入成功。
4. 浏览器在线文档注入仍正常（不回归）。
5. 开启"开机自启" → 注销重登 → VoiceStick 自动以管理员启动。
6. 关闭"开机自启" → 注销重登 → 不再自启；任务计划程序里 `VoiceStickAutoStart` 已删除。

## 涉及文件

- `desktop/windows/resources/VoiceStick.manifest`（requireAdministrator）
- `desktop/windows/CMakeLists.txt`（mt.exe post-build 嵌清单 + 链接 taskschd）
- `desktop/windows/src/win32_app.cc`（`SyncLaunchAtLogin` 重写为任务计划程序）
- `desktop/windows/resources/VoiceStick.rc`（不声明 RT_MANIFEST，由 mt.exe 提供）

## 风险与权衡

1. **每次启动弹 UAC**：用户已接受。
2. **任务计划程序自启兼容性**：少数企业策略禁用任务计划程序时自启失败，捕获 COM 错误记日志。
3. **便携版**：便携模式跳过自启（现状），注入微信需手动以管理员身份运行便携 exe。
