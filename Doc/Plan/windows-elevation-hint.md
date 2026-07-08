# 检测高权限前台并提醒提权（微信输入法模式 UIPI 提醒）

## 背景

v1.9.0 VoiceStick 清单回退 `asInvoker`（Medium 完整性）后，微信输入法模式在微信 4.0（High 完整性）窗口前台失效：`StartWechatInputMethodSession` 第一步 `wechat_hotkey_->SendDown()` 用 `SendInput` 注入 `Ctrl+Win`，被 UIPI 静默丢弃，微信输入法不弹框。

2026-07-08 提权验证确认根因：退出 Medium 实例、以管理员身份重启 VoiceStick 升为 High 后，同一微信前台、同一设备键即正常激活。详见 [[windows-uipi-elevated-injection]]（已纠正"微信输入法模式不受 UIPI 影响"的认知盲点）。

当前方案为"手动提权运行"。痛点：用户在 Medium 实例下于微信窗口按设备键时无任何反馈，困惑"为什么没反应"。本方案让软件在按下时主动检测前台是否高权限，气泡提醒原因，并提供"以管理员身份重启"入口。

## 设计

### 架构（core 可测 + 平台注入）

- **core（`voicestick_core`，单测覆盖）**：新增抽象接口 `IForegroundProcessProbe`，协调器持有 probe，在微信输入法模式 `button_down` 入口检测，高权限则 `ui_->ShowNotification` 提醒，按进程名去重防打扰。
- **平台（`VoiceStickApp`，真机验证）**：`Win32ForegroundProcessProbe` 实现（`OpenProcess(QUERY)` 失败判 High）、托盘菜单项"以管理员身份重启"、`RelaunchElevatedAndQuit`、setter 注入 probe。

### core 改动

`voice_stick_coordinator.h`：

```cpp
class IForegroundProcessProbe {
 public:
  virtual ~IForegroundProcessProbe() = default;
  // 前台窗口所属进程是否高于本进程完整性（SendInput 注入必被 UIPI 拦截）。
  // true 时 process_name 填入 exe 名（如 "Weixin.exe"）供提醒文案。
  virtual bool IsForegroundHigherIntegrity(std::wstring& process_name) = 0;
};
```

协调器新增 `void SetForegroundProbe(std::unique_ptr<IForegroundProcessProbe>)`（setter 注入，避免改构造签名）；私有成员 `std::unique_ptr<IForegroundProcessProbe> foreground_probe_` 与 `std::optional<std::wstring> elevation_warned_process_`（按进程名去重）。

新增 `bool MaybeWarnForegroundElevated(const std::string& device_id)`：probe 为 null 直接返回 false；探测，若 High 且进程名未提醒过 -> `ui_->ShowNotification(title, body)` + 记录进程名，返回 true；否则 false。

**触发点**：`HandleWechatInputMethodPrimaryButtonDown` 入口（残留清理之后、`StartWechatInputMethodSession` 之前）调 `MaybeWarnForegroundElevated`。检测到 High -> `ble_->SendUiState("ready", "", device_id)` + `return`（不发 `SendDown`，避免必失败 + 空转 renderer 残留；`button_up` 到达时 `IsWechatInputMethodActive()=false` 早退，无残留）。

`focused_app` 粘贴路径**MVP 暂不加**，留 TODO。检测编排已通用化，未来易扩展。

### 平台改动

`win32_app.cc`：

- `Win32ForegroundProcessProbe`：`GetForegroundWindow` -> `GetWindowThreadProcessId` -> `OpenProcess(PROCESS_QUERY_INFORMATION=0x0400)` 失败且 `GetLastError()==ERROR_ACCESS_DENIED` -> true；`QueryFullProcessImageNameW` 取 basename 填 `process_name`。
- 托盘菜单加项"以管理员身份重启"（`kMenuRelaunchElevated`），在"退出"前。`WM_COMMAND` 处理 -> `RelaunchElevatedAndQuit()`。
- `RelaunchElevatedAndQuit()`：`GetModuleFileNameW` 取自身路径 -> `ShellExecuteW(nullptr, L"runas", exe, nullptr, nullptr, SW_SHOWNORMAL)` -> 成功后 `ShutdownAndQuit()`（复用现有清理）；失败 `ShowNotification` 报错。
- 协调器构造后 `Start` 前调 `coordinator_->SetForegroundProbe(std::make_unique<Win32ForegroundProcessProbe>())`。
- localization：新增菜单文案 `StringId`，同步 zh-CN/en-US json。

### 提醒文案

- 气泡标题：`需提权运行 VoiceStick`
- 气泡正文：`检测到 Weixin.exe 以高权限运行，语音输入被系统拦截。右键托盘 → 以管理员身份重启后重试。`

### 气泡点击直达

MVP 不做 `NIN_BALLOONUSERCLICK` 直达（气泡易超时消失，不如菜单项可靠）。气泡正文明确引导用户去托盘菜单。气泡点击直达作为可选增强（加 `pending_elevation_relaunch_` 标志 + `NIN_BALLOONUSERCLICK` 分支）。

## TDD

新增 `FakeForegroundProcessProbe`（可控返回值+进程名），`FakeUi` 已记录 `notifications`、`FakeBleCentral` 已记录 `sent_ui_states`。测试用例加入 `core_tests.cc` 的 `main()`：

1. `TestWechatWarnsWhenForegroundElevated`：probe=High+`Weixin.exe` -> `ui.notifications` 含提权提醒 + `fake_hotkey.send_down_count==0`（未 Start）+ `sent_ui_states` 含 ready。
2. `TestWechatNoDuplicateElevationWarnForSameProcess`：同进程名再次 button_down -> `notifications` 计数不增。
3. `TestWechatWarnsAgainForDifferentElevatedProcess`：换进程名再次 button_down -> 再次 `notifications`。
4. `TestWechatNoWarnWhenForegroundNormal`：probe=false（前台 Medium）-> 不调 `ShowNotification` + `send_down_count==1`（正常 Start）。
5. `TestWechatNoProbeNoWarn`：probe=null（未注入）-> 不检测 + `send_down_count==1`（正常 Start）。

## 真机验证

1. Medium 实例运行，微信前台按设备键 -> 气泡弹出 + 设备 ready + 不 Start。
2. 托盘右键 -> 以管理员身份重启 -> UAC -> High 实例 -> 微信前台按设备键 -> 正常激活（OpenProcess 成功不触发提醒）。
3. 浏览器（Medium）前台按设备键 -> 不弹气泡 -> 正常。
4. `ctest` 全绿。

## 涉及文件

- `desktop/windows/src/voice_stick_coordinator.h` / `.cc`（接口+检测编排+触发点+setter）
- `desktop/windows/src/win32_app.h` / `.cc`（probe 实现+菜单项+RelaunchElevatedAndQuit+setter 调用）
- `desktop/windows/src/localization.h` / `.cc`（菜单文案 StringId）
- `desktop/windows/tests/core_tests.cc`（FakeForegroundProcessProbe + 5 测试用例）

## 风险与权衡

- **误报**：`OpenProcess(QUERY)` 失败可能非 High IL（严格 DACL）。低概率，已验证微信确为 ACCESS_DENIED。提权后即正常。
- **事后提醒**：本次输入白费，下次提权重试。asInvoker+手动提权方案的固有代价。
- **气泡超时**：托盘菜单项作为持久兜底入口。
- **focused_app 未覆盖**：MVP 留 TODO。
- **开机自启仍 Medium**：HKCU\Run 拉起的是 Medium 实例，开机后首次在微信前台按设备键会触发提醒，引导用户提权。
