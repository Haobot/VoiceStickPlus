# 微信 4.x MMUI 自绘窗口的 accessibility 树按需激活——targeted「间歇失败」真根因（冷态树回收）

> 排查日：2026-11-04。前五轮见 [targeted-replace-chromium-ax-async-write-2026-10-29.md](targeted-replace-chromium-ax-async-write-2026-10-29.md)（本轮根因与 Chromium AX 异步写入是**两个独立家族**：那篇是「写不落地」，本篇是「读不到」）。

## 症状

微信输入框语音编辑（targeted）间歇失败：`voice targeted rejected reason=verify_unavailable` + `voice_edit fail_stage=focused_element bundle=Weixin.exe code=verify_unavailable`。同一天内 11:52 成功（`whole-field paste applied → result=ok`）、13:20 失败——间歇形态。听写（纯 SendInput 粘贴）不受影响。

## 日志判据（关键：失败有多快）

失败时从 `tseq asr_final` 到 `rejected` 仅 **5ms**，且**没有**任何 `voice edit: ... failed hr=` 错误日志、没有 `voice targeted action=` 行。对照 `focus_text_reader.cc`：失败只能落在**静默分支**（`GetCurrentPatternAs` 拿不到 TextPattern / `selection_count==0`）——5ms 也远不够走完整条跨进程 UIA 链，说明失败在链路第 2~3 步（焦点元素拿到了，但它不提供 TextPattern）。

## 证据链（定位过程）

1. **窗口重建追踪**：`voice edit record` 日志带 hwnd——07:48~11:52 微信窗口 hwnd=265424，12:11 起变成 396398（窗口重建），13:20 失败在新窗口上；但 10:55 旧窗口也失败过 → 间歇与窗口重建无关，树状态本身会变。
2. **热态实锤**：11:52（全文读+整框替换+终验全通）、12:55（GetSelection 通、仅 caret 换算失败）——同一窗口树时活时死。
3. **提权 UIA 探测**（PowerShell UIAutomationClient，必须管理员）：
   - 微信主窗口 `Qt51514QWindowIcon`（Qt 5.15.14 壳），内容区唯一子窗口 `MMUIRenderSubWindowHW`——**微信自研 MMUI 渲染层，没有 CEF/Chromium 子窗口**（与 ZCode/Electron 完全不同架构）。
   - 冷态实测：`WM_GETOBJECT` 返回 0、`AccessibleObjectFromWindow` 返回 `E_FAIL`、UIA `FindAll(IsTextPatternAvailable)` 后代元素数 **0**、连 `IsLegacyIAccessiblePatternAvailable` 都 false——完全哑窗口。
   - 连续 20+ 轮 UIA/MSAA 定点查询无法激活（探测时微信在后台）——激活钩子大概率在「窗口持有焦点」路径上。

## 根因

微信 4.x MMUI 自绘渲染窗口的 accessibility 树**按需激活、闲置超时回收**（本例观察：闲置 ~25 分钟后回收，且新窗口重建后从冷态开始）。VoiceStick 的首次 UIA 查询本身就是激活信号，但树构建是**异步**的——单次查询失败即放弃的旧实现等不到树建好。使用中树保持热（VoiceStick 高频查询即保温），所以「连续用就好用、放一会儿再编辑就失败」。

## 修复（`3329e02`）

- `TakeOverTargetedCommand` 加 `reread_attempt`：全文读不可用时按 `{100,200,400,800}ms` 异步重入（detached 线程 + `RunOnUiThread` + `kFinalizing` 态守卫，与 `MoveCaretForVoiceEditDeletion` 同范式），耗尽才 `verify_unavailable` 拒绝；`full_truncated`/密码框不重试。**验证语义零放宽**（终验/读回轮询/防篡改不动）。
- `focus_text_reader.cc` 静默分支全部补日志：焦点元素 `class/name(截16)/pid` + 各步骤 hr——下次间歇失败一眼判「树未激活 vs 焦点漂移」。
- TDD：fake reader 加 `fulltext_fail_streak`/`fulltext_reads`/`fulltext_truncated`；收敛/耗尽/超长三用例。

## 验证（真机结果：重试生效但**未解决**——新证据修正根因认知）

真机复现（16:10:57，微信闲置 3 小时后）：重试链完整执行（`reread attempt=1..4`，间隔 100/200/400/800ms），5 次读取**全部**失败后拒绝。新诊断日志给出关键事实：

```
voice edit: fulltext TextPattern unavailable class='Qt51514QWindowIcon' name='微信' pid=4168
```

**焦点元素恒为 Qt 壳主窗口（`Qt51514QWindowIcon`），不是渲染子窗口**——`GetFocusedElement` 从 win32 焦点 hwnd 层面就拿的是主窗口，它的 UIA 树无任何 TextPattern。即：失败不在「树未激活后被重试救活」的时序模型，而在**焦点元素指向本身**。

结合 11:52/12:55 曾成功：当时 `GetFocusedElement` 一定返回过带 TextPattern 的元素（无诊断日志，形态未知）——可能是微信某窗口形态/状态下 Qt 壳会把 UIA 焦点转发到 MMUI 渲染层。**「冷/热」的微观机制仍未确证**；本文「树按需激活+超时回收」是宏观描述，微观落点待查。

遗留关键实验（下一轮必做）：**微信前台+输入框聚焦时**，提权采样 `GetFocusedElement` 返回什么（class 是否仍为 Qt 壳）、以及 `MMUIRenderSubWindowHW` 子树的 TextPattern 状态——此前所有探测都在微信**后台**时做的，前台状态的子树从未被探测过。可行方向：GetFocusedElement 无 TextPattern 时回退遍历前台窗口子树找文本元素（ElementFromHandle → FindAll）。

## 第三轮（16:17–16:32，前台探测 + 屏幕阅读器标志实验）

### 新增实锤

1. **微信前台 + 输入框聚焦时，焦点元素仍是 Qt 壳**：16:17:35–16:17:52 微信处于前台（`<<WX-FG>>`），`GetFocusedElement` 恒为 `Qt51514QWindowIcon`（ControlType.Window），`IsTextPatternAvailable=false`；前台窗口子树 TextPattern 计数 **0**。因此「前台/聚焦才激活」不成立。
2. **MSAA 顶层通道是活的，但没有子元素**：`AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible)` 对 Qt 壳与 `MMUIRenderSubWindowHW` 均返回 `role=10 (ROLE_SYSTEM_CLIENT)`、`name='Weixin' / 'MMUIRenderSubW'`、**`accChildCount=0`**。此前「MSAA E_FAIL」的结论是探测脚本 bug（PowerShell 把 `0xFFFFFFFC` 转成 `-4` 再转 `UInt32` 抛异常），实际接口可拿、只是哑树。
3. **`WM_GETOBJECT` 在标志 TRUE 时仍返回 0**：对主窗口/两个子窗口分别发 `OBJID_CLIENT` 与 `OBJID_WINDOW`，全部 0；向窗口发 `WM_SETTINGCHANGE(SPI_SETSCREENREADER)` 也全部返回 0，树不变。
4. **`SPI_SETSCREENREADER` 的正确调用约定是 `uiParam=TRUE, pvParam=NULL`**（不是 MSDN 字面的 pvParam=TRUE）：
   - 约定 A `SystemParametersInfo(0x47, 0, (IntPtr)1, 0x03)` → 返回 True 但 `SPI_GETSCREENREADER` 仍 False；
   - 约定 B `SystemParametersInfo(0x47, 1, IntPtr.Zero, 0x03)` → `SPI_GETSCREENREADER` **变 True**；
   - 注册表 `HKCU\Control Panel\Accessibility\Blind Access\On=1` 可写，但 API 读回仍 False（说明 GET 不读该值）。
5. **运行中的微信不响应标志变化**：标志置 TRUE 后立即探测（0.6s/3.5s）以及发 `WM_SETTINGCHANGE`/广播后，树仍全哑。与 Qt 5.15 在进程初始化时只检查一次 `SPI_GETSCREENREADER` 的行为一致。
6. **`Weixin.dll` 含无障碍代码路径**：`QAccessible` ×26、`IAccessible`、`UIAutomation`、`WM_GETOBJECT`；安装目录无 `libcef.dll`（只有 `mmcronet`/`mmmojo` 网络层），确认聊天界面是 MMUI 自绘。

### 修正后的根因模型

微信 4.x 的 a11y 树开关 = **进程启动时的 `SPI_GETSCREENREADER`**。11:52 成功说明当时标志为 TRUE 且微信在标志为 TRUE 的状态下启动过；12:11 窗口/进程重建后标志已恢复 FALSE，树从此不再构建。**「冷态重试」永远等不到树**，因为它不是异步构建，而是启动时一次性判定。

### 待验证（阻塞修复方案）

**标志 TRUE + 微信重启 → 树是否激活**。验证脚本：`C:\Users\cross\AppData\Local\Temp\wechat-uia-probe\probe_restart_verify.ps1`（置标志 → 等微信完全退出并重启 → 探测；`-RestoreAfter` 另测标志恢复后树是否保持）。需用户同意重启微信（会丢未发送消息）。

### 两条候选修复路线

- **A. UIA 点火（依赖上述验证）**：VoiceStick 启动时以读屏器客户端身份设置 `SPI_SETSCREENREADER`（约定 B），使微信**下次启动**激活树；对已运行的微信需提示用户重启一次。风险：全局 a11y 常驻（其他 Qt/Chromium 应用启用无障碍，可能有性能开销）；退出时是否恢复需实测树是否保持。
- **B. 剪贴板回读 fallback（不依赖 UIA）**：UIA 读不到时用 `Ctrl+A / Ctrl+C` 读真实全文、改动后 `Ctrl+V`、再次 `Ctrl+A / Ctrl+C` 终验。风险：改变用户选区、占用剪贴板（现有 `ClipboardVault` 可完整快照/恢复）、微信自身快捷键冲突；安全语义可保持（读到的全文与预期比对，不伪造成功）。需用户决定是否接受该通道。

### 真机验证（2026-11-04 20:18，成功）

- 19:49 首轮成功：`clipboard field reader: seq_before=2497 seq_after=2507 updated=1 read_ok=1 text_len=9` → `clipboard fallback full_len=9` → `result=ok`。
- **关键修正：Ctrl+A 后必须留 ≥120ms 再发 Ctrl+C**。30ms 时微信不产生复制（`updated=0`），表现为「输入框全选但没复制」；Ctrl+C 另加一次重试（1000ms 等待窗口）。
- 20:18 优化后复验：正好 **2 次**剪贴板读（接管初读 + 终验读回），`result=ok`。构造替换文本改为复用接管阶段的全文（`exec.pre_action_text`），去掉第三次读。
- 闪动副作用：每次剪贴板读 → 输入框全选闪一下。**2 次是下限**（改写前读全文 + 改写后终验）；再减只能放弃终验，违反「不伪造成功」约束。

### 本机构建陷阱（本轮两次「改完无效」的真凶）

`VoiceStick.exe` 被运行中的 VoiceStick 进程锁定时，ninja 链接报
`LNK1104: 无法打开文件"VoiceStick.exe"`，但 **`build_win.bat` 仍打印
`Build SUCCEEDED` 且 `exit=0`**：

- 脚本第 2 行 `setlocal enabledelayedexpansion` 把 `echo Build SUCCEEDED! Output:`
  里的 `!` 当延迟变量吞掉（实际输出 `Build SUCCEEDED <path>`，与脚本源码格式不一致，易误读为成功）；
- `if errorlevel 1` 未拦住 `cmake --build` 的失败（`cmake --build` 直接调用返回 127，脚本内为何返回 0 待修）。

**构建后必查两件事**（缺一即可能是旧二进制）：
1. `grep -c LNK1104 build_compile.log` 必须为 **0**；
2. `build-x64/VoiceStick.exe` 时间戳必须晚于本次构建开始。

**正确顺序**：提权结束 VoiceStick（普通权限 taskkill 杀不掉管理员进程，
`build_win.bat` 的清进程步骤对此无效）→ 构建 → 验时间戳 → 提权重启。

## 长期经验

1. **探测进程完整性必须与目标一致**：Medium 完整性的 PowerShell 查 High 完整性微信/ZCode 的 UIA provider 数据会被 UIPI **静默降级**（`IsTextPatternAvailable` 全 false 的假象，连 ControlType 都可能拿默认值）——先 `IsInRole(Administrator)` 确认再探测，否则结论全错。
2. **静默 return false 分支必须留日志**：本轮 5ms 快速失败 + 零错误日志，浪费一整轮排查才定位到分支；修复后每步失败都有 hr/元素信息。
3. **`voice edit record` 的 hwnd 是现成的窗口生命周期追踪器**：跨时段对比 hwnd 即可判断目标窗口是否重建（微信 12:11 重建过一次）。
4. **失败速度本身就是证据**：跨进程 UIA 全链至少几十 ms；「5ms 内拒绝」= 失败在链路早期（GetFocusedElement 后第一步），可直接排除 GetSelection/GetText 等后段。
5. **微信与 ZCode 是两个失败家族**：ZCode（Electron/Chromium）是「树在、写不落地」（AX 异步），微信（MMUI 自绘）是「树本身时有时无」——同样的症状（targeted 失败）不要套同一个根因。
6. PowerShell 5.1 对**无 BOM UTF-8 中文脚本按 ANSI 解析**会炸语法错误（Try 缺 Catch 等假错误）——探测脚本一律纯 ASCII。

## 互链

- 前因与同日五轮：[targeted-replace-chromium-ax-async-write-2026-10-29.md](targeted-replace-chromium-ax-async-write-2026-10-29.md)
- UIPI/完整性（听写「无法输入」轮）：同上第五轮追加
- distilled 同步：见 [claude-memory-distilled.md](claude-memory-distilled.md)
