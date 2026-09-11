# 微信输入法（WeType）语音输入无法被程序注入触发——实证链与结论

- 日期：2026-09-11
- 背景：`wechat_input_method` 模式按遥控器语音键后，WeType 语音面板「没反应」，无识别无上屏。
- 结论：**WeType 全链路校验键盘事件的物理来源，第三方 SendInput 注入（含 CUA/PowerShell/C++ 任何宿主、单次或重复注入、任何组合键）都无法触发其语音识别；改键监听同样拒绝注入。这是输入法侧的技术性封死，非 VoiceStick 缺陷。**

## 实证链（全部可复现）

| # | 实验 | 结果 |
|---|------|------|
| 1 | C++ 诊断工具装 LL 键盘钩子 + 库 `SendDown`（首拍+40ms 重复线程） | 钩子计数到 40+ 对 keydown，`LLKHF_INJECTED=1`，节奏正确——**注入事件流完整进入系统** |
| 2 | 注入按住期间 `EnumWindows` | WeType `title=[语音输入]` 浮层窗口（450x280，pid=wetype_update 宿主）**弹出**——浮层 UI 不校验来源 |
| 3 | WeType 官方诊断日志 `%TEMP%\WeTypeVoiceDiagnostic_*.log` | 所有**注入**时刻零 `VoiceCompositionTrace start_received`；所有**物理按键**时刻全部有（gen15/18/21/22/23/24）——**识别会话校验来源** |
| 4 | WeType 设置 → 语音输入 → 改「按住说话」快捷键，注入 Ctrl+Alt / 右 Ctrl（分步按住序列） | 均不被录入——**改键监听也拒绝注入** |
| 5 | 物理长按 Ctrl+Win（用户） | 会话启动 + 识别上屏（「12345678」）100% 正常 |

「昨天第一次能用」的真相：2026-09-10 13:54:47 的 `generation=1` 会话发生在第一轮 ASR 死锁期间——当时 VoiceStick 的 SendDown 因 UI 线程卡死根本发不出，那次识别是用户**物理按键**触发。WeType 从未被注入成功触发过。

## 关键诊断手法（复用价值）

1. **WeType 自带语音诊断日志**：`%TEMP%\WeTypeVoiceDiagnostic_<pid>.log`，`start_received`/`abort reason=finished_without_text` 可精确判定「会话是否启动」。多进程（server/renderer/update 宿主）各写一份，必须全量 grep。
2. **窗口枚举判定浮层**：`EnumWindows` 过滤 wetype 进程，比截图+视觉分析可靠（本轮视觉分析连续误判两次：一次漏看浮层、一次把浮层当成注入成功的证据）。
3. **旁观 LL 钩子验证注入到达**：诊断工具同进程装 `WH_KEYBOARD_LL` 记录 vk/scan/flags——先证明「事件到了系统」，再归因上层。
4. **AI 视觉分析截图只能作线索不能作证据**：本案例中截图分析两次与客观日志矛盾。

## 处置

- `WechatInputMethodHotkey::SendDown` 的 40ms 重复注入（7a444c7d）保留：对任何依赖「长按事件流」的第三方输入法更忠实于物理按键，无害；但对 WeType 无效（其会话启动校验物理来源，与事件流形态无关）。
- `wechat_input_method` 模式 + WeType 组合不可用；用户路径：切 `paste`（腾讯 ASR）/本地识别模式，或等 WeType 提供可触发的开放接口（鼠标点击其状态栏麦克风理论上可行，但状态栏可见性依赖 IME 焦点、位置动态、语义为「按住说话」难以模拟点击，工程上不可靠，未采用）。

## 关联

- 第一轮死锁修复（f4480928）真实有效：解决了 SendDown 发不出的 UI 线程卡死。
- 后续若要恢复「输入法语音」路线，候选方向：驱动级注入（Interception 等，分发成本高）、或与 WeType 官方沟通 deeplink/COM 触发接口。
