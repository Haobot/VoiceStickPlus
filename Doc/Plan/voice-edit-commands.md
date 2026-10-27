# 语音编辑指令（Voice Edit Commands）——需求分析与可行方案

> 状态：已实现（2026-10-20）。macOS 本机 swift build 通过、swift run VoiceStickTests PASSED 1336/1336；Windows 纯逻辑本机 clang 对拍通过，Windows MSVC 构建/CTest 与双端真机验收待补（见 §16 实施记录）。确认项：D1 单独"删除"=1 字、"撤销/撤回"=整段；D2 默认开启；D4 Windows 不可校验时 auto（8s 窗口）；D3 字/句/整段；D6 含常见自然句式；D7 仅 focused_app；D8 两端 auto（spike 后可调 macOS strict）；D9 暂不调 0.5s 门槛；D10 双端同期。
> 关联：[交互模型](../Agent/interaction-model.md)、[桌面端配置](../Ref/desktop-config.md)、[协议参考](../Ref/protocol.md)、[用户数据飞轮](typeless-lessons-and-user-data-flywheel.md)、[语音输入延迟根因链](../Expe/voice-input-latency-root-cause-chain-2026-10-12.md)
> 结论先行：需求可行。建议采用「ASR final 后本地规则解析 + 最近一次注入记录 + 焦点字段/光标校验 + 受控退格注入」四层方案；不改固件、不改 BLE 协议、不依赖云端 LLM。唯一硬假设（焦点字段 + 光标位置可读）需先做 0.5~1 天双端真机 spike 验证。

---

## 0. TL;DR

- **用户要什么**：把"按物理键删字"换成"直接说'删掉最后两个字'/'删除'"，删掉刚刚语音输入的文字。核心是零学习成本的事后修正能力。
- **可行路径**：语音指令仍走一次正常录音→ASR final；在 final 文本进入精修/翻译/粘贴链路**之前**做本地确定性解析；命中后用现有按键注入通道发 N 次退格。**不需要**新的 OS 写回 API，也不需要固件/协议改动。
- **成败关键不是识别，而是"删谁"**：① 只允许编辑"最近一次语音注入、且仍留在当前焦点输入框里的那段文本"；② 必须校验光标仍在注入文本末尾；③ 校验不了就拒绝（比误删安全）。
- **平台现状不对称**：macOS 已有 AX 读焦点字段全文（AppDelegate.swift:127-157），缺光标读取（需新增 kAXSelectedTextRange）；Windows 只有 UIA 读选区（selection_hotword_manager.cc:413-461），需新增焦点文本尾部 + 光标位置读取。两端都能补，但覆盖率必须真机验证。
- **不建议**把执行依据放在流式 partial 上（火山/Cloud 首 partial 约等于音频全长，partial 非稳态），也不建议一期就上 LLM 语义解析（默认精修本身已要 4s 级延迟，独立意图调用只会更慢）。
- **建议一期范围**：中文；删除 N 个字 / 删除最后 N 句 / 删除本次输入全部 / 单独"删除"（默认 1 个字）；仅 focused_app 输出目标；macOS + Windows 双端；默认开关开启（决策点 D2）。
- **前置 spike（0.5~1 人日）**：真机验证 AX/UIA 在文本类应用中的字段+光标读取覆盖率、auto_enter 换行下的退格行为、CJK/emoji 的退格粒度。Spike 通过再进入 P0 实现。
- **工作量**：spike 1 人日 + 双端 P0 约 9~11 人日 + 真机验收 1.5 人日；P1（撤销恢复、词/行、英文、自然句式）另 3~5 人日。

---

## 1. 需求分析

### 1.1 用户问题

当前"删除输入内容"完全依赖**按键映射**：把设备上的某个键（小米遥控器返回键、编码器按键/旋转、StickS3 侧键等）映射成键盘 backspace/delete，由桌面端向前台注入退格（参见 §2.3）。问题：

1. **要记映射**：用户得知道"哪个键被配成了删除"，不同设备/不同平台默认不同，学习成本高。
2. **操作不自然**：语音输入是"说话"，删除却要切回"摸设备按键"，交互通道来回切换。
3. **表达力不足**：按键只能删"光标前 N 个字符"，无法一句话表达"删最后两个字/删最后一句/把刚才那段删掉"。
4. **各平台体验不一致**：macOS 全键长按连发（默认 300ms 后 80ms 节拍，Doc/Ref/desktop-config.md:127-133）；Windows 直连遥控器只有 tap 三键有连发、其余 10 键"按住=多次单击"（xiaomi_keymap_interceptor.h:117），RC003 批次 back 键还要开 UIA 注入器并过 UAC（Doc/Ref/desktop-config.md:93）。
5. **删多字要长按**：长按连发逐字删除，删 10 个字需要按住 1 秒以上，且容易删多/删少。

### 1.2 目标用户与场景

- **语音优先用户**：长时间口述文档/邮件/消息，ASR 错字（实测 CER 约 0.9%~1.3%，见 Doc/Expe/asr-bench-lessons-2026-08-01.md:12-13）和口误需要频繁修正。
- **双手被占用/移动场景**：不愿伸手按设备键。
- **典型场景**
  1. 口述完一句，发现末尾多了两个字："删掉最后两个字"。
  2. 口述到一半改主意："删除最后一句"。
  3. 整段说废了："删除刚才的"（= 撤销本次语音输入）。
  4. 说错一个字："删除"（= 删最后 1 个字，可连说多次）。

### 1.3 用户故事与验收口径

| # | 用户故事 | 验收 |
|---|---|---|
| U1 | 我刚语音输入"你好世界"（auto_enter 开启，已粘贴并回车），说"删掉最后两个字"，期望得到"你好" | 松键后 ≤2s 完成删除；overlay 提示"已删除 2 个字"；不产生多余空白行残留 |
| U2 | 我说"删除"（单独），期望删掉最后 1 个字 | 每次说一次删 1 字；连续说可逐步删 |
| U3 | 我说"删除最后一句"，期望把最后一句（含句末标点）删掉 | 句子边界按 。！？； 与换行切分 |
| U4 | 我说"删除刚才的/撤销"，期望把最近一次语音输入的剩余部分整段删掉 | 整段消失；无残留换行 |
| U5 | 我刚说完但文本已不在输入框（聊天已发送/切换应用/光标移走），说"删除"，期望**不误删任何内容** | 不注入任何按键；overlay 提示"没有可删除的语音输入"或"光标位置已变化" |
| U6 | 我在输入法组合态/密码框/提权窗口里说"删除" | 不注入；给出明确提示 |
| U7 | 我正常口述含"删除"两字的句子，如"请把删除按钮改成红色" | 不被当作指令，正常上屏 |
| U8 | 我没有配置任何删除按键，也能用语音删除 | 开箱即用（默认配置可用） |

### 1.4 目标与非目标

**目标**
- 说一句自然中文，完成对**最近一次语音输入文本**的删除（字/句/整段）。
- 零配置可用、双端一致、失败可见、绝不误删非语音输入内容。
- 完全复用现有录音/ASR/注入/反馈链路，不改固件、不改 BLE 协议。

**非目标（一期明确不做）**
- 编辑任意文本（用户手打内容、历史内容、跨应用内容）。
- 替换/改写/插入（"把某某改成某某"）——P2。
- 字幕模式（无注入对象）、微信输入法模式（音频进虚拟麦，桌面端根本不过 ASR；voice_stick_coordinator.cc:989-1094）。
- 设备屏新增状态/中文文案（固件 thinking 的 text 被丢弃、屏幕字体无 CJK，firmware/main/main.c:1941-1949、firmware/components/ui_status/ui_status.c:158,264）。
- 语音指令与正文混说（"你好世界删除最后两个字"不识别；指令必须单独一轮）。
- 流式 partial 边说边删（P2 只考虑用于提示）。

### 1.5 适用边界（必须在产品文案/文档中写清）

1. **只对"最近一次语音注入、且仍在当前焦点输入框"的文本生效**；用户切应用、移动光标、继续手打后，可能拒绝执行。
2. **auto_enter 已把文本提交出去的应用**（聊天/搜索/表单）无法事后删除——文本已不在输入框，会安全拒绝。语音编辑最适合文档/笔记/邮件正文这类"回车=换行"的场景。
3. **不能跨应用**；不能在密码框、提权窗口（Windows UIPI）里工作。
4. **删多字是"退格 N 次"**，跨应用行为可能有细微差异（emoji/组合字符、输入法组合态）；见 §7。
5. 字幕模式/微信输入法模式不支持。

---

## 2. 现状事实与约束（代码证据）

> 本节为方案的事实底座，均来自本次只读代码调查；未运行验证的部分在 §12.1 spike 中列出。

### 2.1 文本收口链路（唯一一条主干）

**macOS**：ASR onFinal（VoiceStickCoordinator.swift:506-510）→ finishWithFinalText（:1960）→【全句纠错对精确命中 / LLM 流式精修（默认开，预算 2.5s 首 token、5s 起总预算，:207-214）】→ finishWithRefinedText（:2072）→ 按输出 profile 分支（字幕/空文本/翻译/直通）→ enterPendingConfirmation（:2130）→ overlay 0.4s 倒计时（OverlayController.swift:515-526）→ commitPendingPaste（:2714）→ completePendingPaste（:2738-2755）→ inputInjector.paste（:2753）→ beginEditTracking（:2754）。

**Windows**：ASR on_final（voice_stick_coordinator.cc:628-634）→ FinishWithFinalText（:2386-2440）→ 翻译 / 纠错对 / 本地精修 / 云端精修 / 直通（TransformText :2616-2805）→ EnterPendingConfirmation（voice_stick_coordinator_tail.cc:797-805）→ **直接** CompletePendingPaste（tail.cc:406-414，无倒计时，commit b6753e0 移除了 400ms 确认窗）→ input_injector_->Paste(text, ConfigSnapshot()->auto_enter)。
另外，**finalizing watchdog** 兜底路径（tail.cc:310-342）在 15s 无进展时直接用回退原文 EnterPendingConfirmation，绕过 FinishWithFinalText/TransformText——指令拦截必须覆盖这条路径（防御性在 watchdog 分支再调用一次同一拦截器）。

### 2.2 注入原语

- 两端文本注入都只有**剪贴板 + 粘贴**：macOS NSPasteboard + Cmd+V（InputInjector.swift:11-40，可选 0.12s 后补 Return）；Windows 写 CF_UNICODETEXT + Ctrl+V（input_injector_win.cc:11-76，含 ClipboardVault 全格式恢复）。
- 按键注入：macOS sendKeyCombo(_ spec: KeySpec)（InputInjector.swift:87-111）；Windows SendKeyCombo（input_injector_win.cc:116-135，主键带 scan code，兼容第三方输入法）。
- KeySpec 两端都支持 backspace/delete（macOS ConfigDomain.swift:309-321；Windows key_spec.cc:38-39）。
- **没有**对第三方应用文本的写回 API（无 AX SetAttributeValue、无 UIA ValuePattern 写）；也没有"文本插入到光标"接口。
- 接口扩展成本：给 Windows InputInjector 加纯虚方法要同步改 InputInjectorWin 与两处测试 Fake（tests/test_support.h:419、tests/integration_tests.cc:258）。**本方案不扩展接口**，直接复用 SendKeyCombo。

### 2.3 现有"按钮删除"链路（用户今天怎么删）

| 路径 | 行为 | 证据 |
|---|---|---|
| 网关模式（遥控器配对在 StickS3） | 可映射键一律 software 路由；back 的 native 语义 = backspace；按住按节拍连发 | GatewaySupport.swift:58-72；VoiceStickCoordinator.swift:814-896 |
| macOS 直连遥控器 | HID 拦截 + 映射后注入；intercept 默认 false，需显式开；除语音键外按住连发（300/80ms 默认） | XiaomiButtonInterceptManager.swift:182-207；RemoteButtonRepeater.swift；Doc/Ref/desktop-config.md:127-133 |
| Windows 小米遥控器 | [xiaomi.keys] back = "backspace" 一类映射；直连 LL 钩子注入，网关路径连发可配（默认 400/120ms，但该配置只 load 不 save） | xiaomi_keymap_interceptor.cc:92-286；xiaomi_keymap_hook.cc:156-436；app_config.cc:786 vs WriteTo |
| 编码器 | 旋转/单击/双击可映射 backspace，逐格注入 | voice_stick_coordinator.cc:1482-1706（Windows）；VoiceStickCoordinator.swift:1231-1438（macOS） |

结论：今天的删除**没有文本语义**——不判断光标位置、不判断输入框内容、不判断输入法组合态、不能撤销；语音编辑方案要补的正是这些"语义"，而不是重新发明按键。

### 2.4 编辑追踪 / 焦点读取（决定"能不能安全删"）

- **macOS 有**：EditTracking.editedRegion（SessionHistory.swift:321-347，公共前后缀覆盖 ≥ 注入长度一半才归因）、readFocusedFieldText（AppDelegate.swift:127-157，系统级 AX 焦点元素 + kAXValueAttribute 整段文本）、frontmostAppBundleID（AppDelegate.swift:120-123）。读窗为注入后 8s 首读 + 每 10s 重试、最多 5 次（VoiceStickCoordinator.swift:185-187）。
- **macOS 缺**：光标/选区读取（全仓无 kAXSelectedTextRange）。
- **Windows 缺**：主分支没有编辑追踪；没有焦点字段读取。只有 selection_hotword_manager.cc:413-461 的 UIA 选区读取范式（GetFocusedElement → TextPattern → GetSelection → GetText）。未合并分支 feat/windows-session-history（领先 main 3 / 落后 6）有会话历史存储与 ForegroundAppBucket()，但**仍无**焦点字段读取与 diff 生产者。
- **UIPI**：Windows 前台进程权限更高时 SendInput 静默失效；现有检测只在会话启动时提醒（coordinator.h:262-267、voice_stick_coordinator.cc:1239-1268）。

### 2.5 ASR 与延迟

- macOS 无本地 ASR；云端 final 尾延迟实测腾讯约 125ms、火山约 969ms；端到端松键→粘贴实测 0.44~0.48s（无精修，Doc/Expe/voice-input-latency-root-cause-chain-2026-10-12.md）。
- 默认 refine_enabled=true（两端），LLM 精修实测短句 +2.7s、长句可达 13.9s/29.1s（Doc/Plan/refine-latency-optimization.md）。**指令路径必须绕开精修**，否则删除命令要等 4s 级延迟。
- 流式 partial：腾讯首 partial p50 672~968ms 可用，火山/VoiceStick Cloud 首 partial 约等于音频全长（p50 4172ms、p95 18656ms，Doc/Expe/asr-bench-baseline-2026-08-01.md:23,29,40、Doc/Expe/volc-config-ablation-2026-08-01.md:22,26）；且 partial 非稳态、会被后续修正。→ **只能做提示，不能做执行依据**。
- 最短录音门槛 0.5s（macOS VoiceStickCoordinator.swift:203,1944-1958；Windows coordinator.h:905、voice_stick_coordinator.cc:2375-2384），固件录音起始有 60ms 静音 + 60ms 淡入（firmware/components/audio_pipeline/audio_pipeline.c:250-251）。按住时长通常包含按/放余量，单说"删除"多数能过门槛，但"秒按秒说"可能被丢——列入真机验收（§11.3）。

### 2.6 反馈面与设备屏

- macOS overlay 可显示任意中文（showTimedMessage，StatusSink.swift:62、OverlayController.swift:536-539），不可交互。
- Windows overlay ShowTimedMessage 可用（overlay_window.cc:307-360），但**会话活跃时会回落托盘气泡**（win32_app_prefs.cc:843-853）。
- 设备屏：ui_state 仅 ready/recording/thinking/pending_confirmation/error/air_mouse（Doc/Ref/protocol.md:97）；固件 thinking 丢弃 text、pending 固定英文、字体无 CJK（firmware/main/main.c:1941-1949、ui_status.c:158,264）；协议帧预算 244B（protocol.md:357）。→ **一期不改设备屏文案**，只维持现有 ready/thinking 状态。

### 2.7 配置 / UI / 本地化落点

- macOS：AppConfig.swift（字段/默认值/parse/serializedText/legacy）→ ConfigFileModels.swift → Config/config.example.toml → Doc/Ref/desktop-config.md → SettingsWindowController（build/load/save/visibility）→ L10nKey 双表（英文 Localization.swift:189 / 中文 :464；缺任一表 tablesAreComplete 断言会红，AppDelegate.swift:35）。
- Windows：app_config.h（字段+默认值）→ ApplyConfigValue / 段表解析 → WriteTo 白名单（**不加写回=不持久化**；既有反例 xiaomi_gateway_repeat_interval_ms 只 load 不 save）→ resources/config.template.toml → Doc/Ref/desktop-config.md → settings_dialog.cc → localization.h/.cc 双表（StringId 只能在末尾追加；core_tests_license_ima.cc:413 断言）。
- 平台差异：macOS auto_enter 默认 **true**（AppConfig.swift:184），Windows 默认 **false**（app_config.h:317）——删除时的换行处理按"注入时实际配置"记录，不假设。

---

## 3. 设计空间与选型

### 3.1 意图识别三条路

| 路径 | 做法 | 延迟/成本 | 误判风险 | 结论 |
|---|---|---|---|---|
| **A. final 后本地规则解析** | 白名单语法 + 中文数字/单位解析，整句严格匹配 | 约 0ms / 0 成本 | 低（可枚举、可测试、可关） | **一期采用** |
| B. LLM 意图解析 | 复用现有 LLM 通道；模型只出"编辑计划"，代码守卫执行 | 独立调用 +1~4s（首 token 1~2s，精修实测 p50 约 4s）；并入精修 prompt 边际成本约 0 但指令必须发生在精修前 | 中高（幻觉/过度执行），需二次确认 | **二期**（且只做"确认后执行"） |
| C. 流式 partial 提前拦截 | partial 命中命令词即准备/执行 | 只有腾讯/本地 ASR 有 0.6~1s 提前量；partial 非稳态 | 高（提前动作可能做错） | **不做执行**；P1 可做"识别到指令"提示 |

规则解析还有现成同构先例：全句纠错对精确匹配（applyCorrectionPairExact，VoiceStickCoordinator.swift:1969,2482-2492）就是"final 文本 → 确定性动作"的工程范式；Windows 侧 ApplyPinyinCorrections（pinyin_guard.h:44-70）是"模型出受限指令、代码守卫执行"的范式（二期 LLM 路线沿用）。

### 3.2 作用对象三种模型

| 模型 | 含义 | 安全性 | 可用性 | 结论 |
|---|---|---|---|---|
| S1 仅本次/待粘贴文本 | 在粘贴前直接改文本 | 最高（不碰 OS） | 低：macOS 确认窗仅 0.4s、Windows 无窗 | P1 补充（待粘贴文本修正） |
| **S2 最近一次语音注入文本（已上屏）** | 记录注入内容，之后对齐焦点输入框删除其尾部 | 中高（需字段+光标校验） | 高（符合用户直觉） | **一期采用** |
| S3 光标前任意文本 | 相当于语音版退格，不限定来源 | 低（可能删用户手打内容） | 最高 | 不采用 |

### 3.3 执行手段

| 手段 | 评价 |
|---|---|
| **退格 N 次**（复用 SendKeyCombo(backspace)） | 与现有按钮删除同机制、跨应用通用；缺点是无回执、行为受应用/输入法影响 → 用字段+光标校验兜底。**采用** |
| 选中 N 字再 Delete（Shift+Left 乘 N） | 多一倍按键、选区行为差异更大，无收益。否决 |
| Cmd+Z / Ctrl+Z 撤销 | 撤销粒度不可控（粘贴、回车是不同单元，用户可能已做别的操作）。否决 |
| 全选 + 重贴修正文本 | 破坏撤销栈/富文本/应用内编辑器语义，且需全选（危险）。否决 |
| AX/UIA 写回文本 | 两端均无实现，且需要新权限/兼容性风险；Windows 连全文读都没有。否决 |

### 3.4 选型结论

**S2 + A + 退格注入**，配合"两道安全闸"（§6）与"可验证才执行"原则。整体不触碰固件/协议；未来移动端可通过同一协议事件扩展，不在本方案范围。

---

## 4. 方案总览

~~~text
按住语音键 → 录音 → 松键 → ASR final 文本
                                   │
                    ┌──────────────┴──────────────┐
                    │ VoiceEditParser（纯逻辑）    │
                    │ 整句白名单语法，约 0ms       │
                    └──────────────┬──────────────┘
                       未命中 │            │ 命中
                              ▼            ▼
                 原链路：纠错对→精修→   VoiceEditController
                 翻译→倒计时→粘贴        ├─ L0 作用域闸（记录/时间/前台应用）
                 （注入成功后记录          ├─ L1 目标校验（字段尾部 + 光标）
                  LastInjection）          ├─ 计算删除计划（grapheme/换行/上限）
                                           ├─ 执行：SendKeyCombo(backspace) 乘 N
                                           └─ 反馈：overlay toast / 托盘；回 ready
~~~

**关键原则**

1. **拦截点唯一**：macOS 在 finishWithFinalText 入口；Windows 在 FinishWithFinalText 入口 + watchdog 兜底分支。命中即 return，绝不进入精修/翻译/粘贴。
2. **记录点唯一**：两端都在**唯一粘贴 chokepoint**（macOS completePendingPaste、Windows CompletePendingPaste）写入 LastInjection（实际注入文本、时间、前台应用/窗口、auto_enter 实际值）；避免依赖 last_recoverable_text_（Windows watchdog 路径不设置它）。
3. **安全优先**：校验失败 = 不删除 + 明确提示；宁可不删，不可误删。
4. **不污染数据飞轮**：命令轮不写会话历史、不参与纠错对挖掘；执行前结束上一轮编辑追踪。

---

## 5. 指令语法与解析器（VoiceEditParser）

### 5.1 一期语法表（中文）

> 匹配前统一归一化：去首尾空白、去尾部标点（。．.!！?？,，;；、）、全角数字转半角。**整句匹配**，不允许夹带其他词。

| 意图 | 例句（含变体） | 动作 |
|---|---|---|
| 删最后 N 个字 | "删掉最后两个字"、"删除两个字"、"删掉最后 2 个字"、"去掉最后两个字符"、"最后两个字删掉"、"把最后两个字删掉" | deleteLastChars(N)，N 缺省=配置默认（见 5.2） |
| 删最后 N 句 | "删掉最后一句"、"删除最后这句话"、"删掉最后两句话"、"把最后一句删掉" | deleteLastSentences(N)，默认 1 |
| 删本次输入全部 | "删除刚才的"、"删掉全部"、"删除本次输入"、"清空"、"撤销"、"撤回"、"都删掉" | deleteAll |
| 单独"删除" | "删除"、"删掉"、"去掉"、"删掉一下" | deleteLastChars(voice_edit.default_chars)（默认 1） |

**语法要点**

- 动词白名单：删除 / 删掉 / 删去 / 删了 / 去掉 / 移除 / 清除 / 清空 / 撤销 / 撤回 / 取消 / 删（单独"删"只在带数量时接受，如"删两个字"）。
- 数量：中文数字（一~二十、两、俩）+ 阿拉伯数字；缺省时按单位默认 1。
- 位置词：最后 / 末尾 / 结尾 / 后面 可选；**不接受** 前面/开头/第一（一期不支持，避免误判；P1 扩展）。
- 单位：字/字符/汉字/文字（按 grapheme 计）、句/句话/句子（按句末标点/换行切分）；段/行/词 一期不识别（按普通文本上屏，P1 扩展）。
- 可选礼貌前缀/语气尾：请 / 帮我 / 麻烦 开头、吧 / 啊 / 呀 / 哦 / 嘛 结尾（仍要求其余部分整句匹配）。
- **不识别**：疑问/否定（"要不要删除"、"别删除"）、包含式（"把这个文件删除"、"帮我删掉那个"）、混合正文（"你好世界删除最后两个字"）→ 一律按普通文本上屏。

### 5.2 数量与默认值

- N 必须为整数 1..max_chars（默认 20）；超过上限或无法解析 → 不视为指令（按普通文本），避免"想删 100 字"造成大范围破坏。
- **单独"删除"的默认量** = [voice_edit] default_chars，默认 **1**（等价一次退格，最保守、可连说）。"撤销/撤回"明确映射为**整段删除**（撤销本次输入）。
  - 备选语义：默认 2 字（与用户示例"删掉最后两个字"对齐）或默认整段。列为决策点 D1。
- 解析结果携带 commandKind 与 requestedCount，便于日志/测试/未来埋点。

### 5.3 误触发防护（多层）

1. **整句严格匹配**：不是正则"包含"，而是整句消费完；任何多余词即不命中。
2. **长度上限**：指令文本不超过 24 个字符（中文），避免长句里的偶然匹配。
3. **不含引号/数字序列/英文混排**等异常形态（降低"删除 2024 年数据"这类误判）。
4. **作用域不存在时不吞文本、也不粘贴**：明确 toast（"没有可删除的语音输入"）。这一条让"用户真想输入'删除'二字"的最坏后果只是"提示 + 没上屏"，不会误删。
5. **可关闭**：[voice_edit] enabled=false 完全回到现状。
6. **可选严格前缀模式**（P1）：require_prefix="执行"，只认"执行删除最后两个字"，进一步降低口述误触。
7. **回归测试向量**：至少 30 条正例 + 20 条负例（含"请把删除按钮改成红色"、"删不删"、"删除 100 个字"）。

### 5.4 多语言

- 一期中文（产品主场景；ASR 默认 16k_zh）。英文词表（"delete last two characters / remove last sentence / delete it"）作为 P1，数字解析与单位（characters/words/sentences）复用同一 VoiceEditParser 框架。
- 方言/中英混说未评测（腾讯 16k_zh_en 仅配置项存在，见 Doc/Ref/desktop-config.md:12）；一期不承诺。
- 同音容错：仅收录极少数确定性别名（"子"→"字"，且仅在数量词后），**不做通用拼音模糊匹配**（会把"删除最后两个子"这类噪声也当指令）。P1 可用 PinyinGuard 做"疑似指令"提示而非自动执行。

### 5.5 数据结构（两端同构）

~~~swift
public enum VoiceEditAction: Equatable {
    case deleteLastChars(Int)
    case deleteLastSentences(Int)
    case deleteAll
}
public struct VoiceEditCommand: Equatable {
    public let action: VoiceEditAction
    public let source: VoiceEditCommandSource  // .explicitCount / .defaultCount / .all
}
public enum VoiceEditParser {
    public static func parse(_ text: String, defaultChars: Int, maxChars: Int) -> VoiceEditCommand?
}
~~~

Windows 侧以 voice_edit_command.h 提供等价 VoiceEditAction/ParseVoiceEditCommand，测试向量与 Swift 保持同一张表（延续 correction_pairs 双端同向量约定）。

### 5.6 接入点

| 平台 | 文件/函数 | 插入位置 |
|---|---|---|
| macOS | VoiceStickCoordinator.finishWithFinalText（:1960） | guard !pastedFinalText 之后、beginSessionHistory 之前；命中即 return（并自行 finishEditTracking） |
| Windows | VoiceStickCoordinator::FinishWithFinalText（:2386） | pasted_final_text_ 重入保护之后、finalizing_fallback_text_ = text 之前；命中即 return |
| Windows | watchdog 兜底（tail.cc:310-342） | 调 EnterPendingConfirmation(fallback_text) 之前再调用同一拦截器（防指令被当正文粘贴） |

> 顺序理由：① 必须在精修之前（省掉 4s 级 LLM 延迟）；② 必须在 beginSessionHistory 之前（命令轮不产生历史行/编辑追踪草稿）；③ 命中后置 pastedFinalText/pasted_final_text_ = true，保证之后任何迟到回调不二次粘贴。

---

## 6. 作用域与目标校验（安全核心）

### 6.1 LastInjection 记录（两端同一 schema）

| 字段 | 说明 |
|---|---|
| text | 实际注入的最终文本（精修后/翻译后，即 completePendingPaste 的入参） |
| remainingText | 理论上仍留在输入框中的剩余文本；每执行一次删除后更新 |
| injectedAt | 注入完成时刻（monotonic） |
| pressedEnter | 注入时 auto_enter 的实际值（决定末尾是否有换行；删除后若换行已消费则置 false） |
| appID | macOS bundle id（frontmostAppBundleID）；Windows 进程 exe 名 + 前台窗口句柄 |
| historyID | macOS 会话历史行 ID（可选，便于排障；不写命令轮） |

- **写入**：macOS completePendingPaste（:2738-2755）在 inputInjector.paste 之后；若 isProcessTrusted 为 false（注入被拦）则不记。Windows CompletePendingPaste（tail.cc:406-414）在 Paste 之后记录（覆盖 watchdog 兜底粘贴路径）。
- **清理**：新注入覆盖；output.target 变化；配置更新/退出；pastedFinalText 重置不影响记录。
- 不做多设备队列：始终只保留**最近一次**注入（符合"最后两个字"的直觉）。

### 6.2 L0 作用域闸（必过）

1. voice_edit.enabled == true；
2. 输出目标为 focused_app（字幕/微信模式直接按原链路处理，不拦截）；
3. 存在 LastInjection 且 remainingText 非空；
4. now - injectedAt <= scope_seconds（默认 120s，可配）；
5. 前台应用一致（macOS bundle id；Windows 前台窗口句柄 + 进程 exe 名）；
6. Windows：前台进程完整性不高于本进程（复用 IForegroundProcessProbe，coordinator.h:262-267）；否则提示"目标窗口权限较高，无法删除"；
7. macOS：辅助功能权限仍在（AXIsProcessTrusted）。

不满足 → **不注入任何按键**，toast 提示原因。

### 6.3 L1 目标校验（可验证时强制通过）

**目标**：确认"注入文本仍完整/部分留在焦点字段末尾，且光标就在其后（允许在末尾换行前）"。

**macOS（新增读取能力）**：把 AppDelegate 的 readFocusedFieldText 扩展为 readFocusedFieldState -> (text: String, caretUTF16: Int?, selectionLength: Int?)：
- 焦点元素 kAXValueAttribute（现有）；
- 新增 kAXSelectedTextRangeAttribute → AXValueGetValue(kAXValueCFRangeType) → (location, length)；
- 安全字段/无可读值/无选区属性 → 返回不可验证；
- 大文本保护：字段 UTF-16 长度 > 2M 时跳过校验（按不可验证处理），避免主线程阻塞。

**Windows（新增读取能力）**：新增 FocusTextReader（COM STA，UI 线程）：
- IUIAutomation8::GetFocusedElement → UIA_TextPatternId；
- GetDocumentRange() → 读**尾部**文本（只取最后约 512 字符的 range，避免全文档读取性能问题：Clone 文档 range → MoveEndpointByUnit(Start, TextUnit_Character, -512) → GetText）；
- GetSelection() → 要求恰 1 个选区且**折叠**（CompareEndpoints(Start, End)==0），且选区末端 == 文档 range 末端（CompareEndpoints）→ 光标在文末；
- 不支持 TextPattern / 读失败 → 不可验证。

**校验算法（伪码）**

~~~text
input: remainingText, pressedEnter, requested N（或 sentence/all）
field := 读到的焦点字段文本（可验证时）
caretAtEnd := 光标折叠且位于字段末尾

if 可验证:
    require field != nil && caretAtEnd
    # 允许用户手动删过尾部：取 remainingText 的最长前缀，使它是 field 的后缀
    eff := 0
    for k in min(len(remainingText), len(field)) .. 1:
        if field.hasSuffix(remainingText.prefix(k)): eff := k; break
    require eff > 0
    effectiveText := remainingText.prefix(eff)
    newline := (pressedEnter && field.hasSuffix(effectiveText + LF)) ? 1 : 0
    targetChars := 按动作计算（字=N / 句=最后N句 / 全部=effectiveText）
    n := min(targetChars, graphemeCount(effectiveText))
    count := n + newline
    require count > 0
else:
    # 不可验证分支，见 6.5
    n := min(targetChars, graphemeCount(remainingText))
    count := n + (pressedEnter ? 1 : 0)
~~~

**执行后更新**：remainingText = dropLastN(effectiveText / remainingText)；若本次消费了换行 → pressedEnter = false。

**示例（U1）**：注入"你好世界"、auto_enter=true → 字段 "你好世界" + 换行、光标在末尾。说"删掉最后两个字"：eff=4、newline=1、n=2 → count=3（1 换行 + "世" + "界"）→ 字段变 "你好"。
**示例（用户手动删过）**：字段 "你好世" + 换行（用户已删"界"），remaining="你好世界"：eff=3、effective="你好世"、newline=1、n=min(2,3)=2 → count=3 → 删换行+"世"+"好" → "你"（用户可见的最后 2 个字）。
**示例（用户继续手打）**：字段 "你好世界" + 换行 + "吗"：field 不以 remaining 的任何前缀结尾 → eff=0 → 拒绝并提示。
**示例（光标移中间）**：字段文本仍完整但 caretAtEnd=false → 拒绝并提示"请先将光标移回末尾"。

### 6.4 额外安全边界

- **选区非空**：拒绝（提示"请先取消选中"）——避免把"删除 N 字"变成"删除整段选区"（P1 再支持"删除选中"）。
- **密码框/安全输入**：macOS AX 读不到值 → 走不可验证分支并被拒绝；Windows UIA IsPassword 属性可选检查（P1）。
- **不越过注入边界**：N 永远 clamp 到 effectiveText 的 grapheme 数，绝不删到用户手打内容。
- **删除量上限**：max_chars 默认 20（整段删除例外，但整段本身就是本次注入内容）。
- **隐私红线**：字段全文只在内存参与校验，**不落库、不上传、不写日志内容**（只记长度/结果码）；沿用编辑追踪的既有约束（Doc/Plan/typeless-lessons-and-user-data-flywheel.md 第六节）。

### 6.5 不可验证时的降级策略（verify_mode）

| 模式 | 行为 | 建议场景 |
|---|---|---|
| strict | 读不到字段/光标 → 一律拒绝 | 安全优先（AX 覆盖率验证良好时建议默认，如 macOS） |
| auto（默认） | 可验证→必须通过；不可验证→仅当 now-injectedAt <= unverified_seconds（默认 8s）且前台应用/窗口一致时执行，日志标记 unverified | 兼容 UIA 覆盖不足的应用 |
| off | 不校验，仅 L0（高级/排障） | 不推荐日常 |

> auto 的不可验证分支存在"用户选中了文本 / 光标移走但 8s 内"的理论风险；缓解：短时间窗 + 前台窗口一致 + 日志留痕 + 默认单次删除量小。若评审更偏安全，可把默认改为 strict，Windows 不支持 UIA 的应用直接提示"该应用不支持语音删除"（决策点 D4）。

---

## 7. 执行机制

### 7.1 退格注入

- 复用现有通道：macOS inputInjector.sendKeyCombo(KeySpec.parse("backspace")!)；Windows input_injector_->SendKeyCombo(*ParseKeySpec("backspace"))。
- 次数 = §6.3 计算出的 count；上限 20（或整段时一次最多 = 注入文本 grapheme 数，仍受 max_chars 之外的整段例外）。
- **异步执行**：不要在 UI 线程循环注入（会卡 300ms+）。macOS 用后台 DispatchQueue + usleep 节拍，完成后回主线程更新记录/反馈；Windows 用 std::thread（SendInput 线程安全）+ RunOnUiThread 回收。
- **节拍**：每次 10~15ms，避免目标应用/输入法丢事件；count 上限保证总耗时不超过约 350ms。
- 注入期间不阻塞新会话（状态已回 ready）；如用户立刻再次按键，按现有逻辑开新会话即可。

### 7.2 "字"的计数（grapheme）

- 用户说的"字"= 用户可见字符（grapheme cluster）：CJK 1 个；emoji 1 个；组合字符 1 个。
- macOS 直接用 Swift Character 计数（String.count）。
- Windows 需新增 GraphemeCount/DropLastGraphemes（UTF-16 码元 + surrogate pair + combining 扩展；可先覆盖 CJK/emoji/常见组合，边界测试列入 §11.1）。CJK 场景 1 汉字 = 1 UTF-16 码元，MVP 风险低。
- **与目标应用退格行为的关系**：大多数编辑器按 grapheme 删除；个别控件按 code unit。此差异列入真机矩阵（§11.3），异常应用降级为"删除量可能差 1"并在文档标注。

### 7.3 auto_enter 换行

- 注入时记录实际 auto_enter；可验证分支通过字段末尾是否真有换行决定是否多发 1 次退格（§6.3），不假设"一定有换行"（单行输入框可能忽略 Return，或应用把回车当提交）。
- 不可验证分支：pressedEnter==true 时多发 1 次；执行后置 false（避免重复多发）。

### 7.4 失败与回执

- CGEvent/SendInput 无回执；**不做"注入后读回验证"**作为 P0（会引入额外延迟与复杂度）。
- 注入前已尽量校验；注入后 toast 只是结果告知（"已删除 2 个字"），不是保证。
- Windows UIPI/前台权限问题 → 注入前拒绝（§6.2）；macOS 无辅助功能权限 → 现有提示链路（VoiceStickCoordinator.swift:2724-2736）。

### 7.5 撤销/恢复（P1）

- 每次删除记 lastDeletion = {deletedText, deletedAt, injectionRef}（内存）。
- 指令"恢复刚才删除的 / 撤销删除"→ 在光标处重新粘贴 deletedText（同样要过 L0/L1 校验；若光标已移动则拒绝）。
- 不依赖系统 Cmd+Z（撤销粒度不可控）。P1 另可在托盘菜单/overlay 提供入口。

---

## 8. 交互、反馈与状态机

### 8.1 状态流转

命令轮**不新增状态**：沿用 recording → finalizing，在 final 命中后于 finalizing 内完成"解析→校验→注入→收尾"，随后回 ready。

**macOS**（finishWithFinalText 内）：
1. guard !pastedFinalText；guard let command = parse(...)；
2. pastedFinalText = true（阻止迟到回调）；
3. finishEditTracking(status: SKIPPED, editedSpan: nil)（结束上一轮编辑追踪，防把退格误判为用户编辑）；
4. cancelStreamingRefinement()（防御）；
5. L0 → L1 → 得到 plan 或失败原因；
6. 失败：finishRecognitionCycle() + Ready + sendUIStateForActiveDevice("ready") + mainInputState = .ready + toast；
7. 成功：先 finishRecognitionCycle() + Ready + ui_state ready + mainInputState = .ready，再异步注入，完成后 toast 结果。
   > 顺序注意：finishRecognitionCycle 会清 pastedFinalText（:2908-2914），因此记录/计划要在调用前算好；toast 放在收尾之后，避免被 hideOverlay 吞掉。

**Windows**（FinishWithFinalText 内）：
1. if (HandleVoiceEditIfMatched(text)) return;（内部：pasted_final_text_=true → 解析 → 校验 → FinishRecognitionCycle() → EnterReady("voice_edit") → 异步注入 → ShowTimedMessage）；
2. FinishRecognitionCycle 会清 finalizing_fallback_text_（tail.cc:539-547），状态离开 kFinalizing 后 watchdog 自动退出；
3. watchdog 兜底分支同样先调 HandleVoiceEditIfMatched（防"删除"两字被当正文粘贴）；watchdog 在 detached 线程，必须先 RunOnUiThread 再调用，避免并发写状态机。

### 8.2 反馈文案（新增 L10nKey / StringId，中英双表）

| 场景 | 中文 | 英文（建议） |
|---|---|---|
| 删除成功 | "已删除 {n} 个字" / "已删除最后一句" / "已删除本次输入" | "Deleted {n} characters" / "Deleted last sentence" / "Deleted this dictation" |
| 无目标 | "没有可删除的语音输入" | "No voice input to delete" |
| 光标移开 | "光标位置已变化，未删除" | "Cursor moved; nothing deleted" |
| 选区存在 | "请先取消选中" | "Clear the selection first" |
| 不支持的模式 | （不拦截，按普通文本处理，无需文案） | — |
| 权限/提权 | "目标窗口权限较高，无法删除" | "Cannot delete in an elevated window" |
| 未校验执行 | "已删除（未校验光标）"（可选） | "Deleted (cursor unverified)" |

- macOS：showTimedMessage（信息态圆点，1.5~2s，StatusSink.swift:62）。
- Windows：ShowTimedMessage（会话已结束后走 overlay；会话活跃时回落托盘气泡是既有行为，可接受）。
- 设备屏：只发 ready/thinking，不带文案（固件不支持 CJK，且 thinking text 被丢弃）。

### 8.3 与会话历史 / 编辑追踪 / 纠错对的关系

- **不写会话历史**：命令轮不是"语音输入内容"，写 history 会污染数据飞轮与 latestRefinedTexts。
- **结束上一轮编辑追踪**：命令执行前调 finishEditTracking(SKIPPED)，否则退格造成的字段变化会被编辑追踪误判为用户编辑并写入纠错对库（VoiceStickCoordinator.swift:2621-2657）。
- **不进纠错对**：命令文本（"删掉最后两个字"）绝不参与精修 diff / 纠错对挖掘。
- **可观测**：只写日志（命令类型、n、校验结果、耗时、verification=verified/unverified）与可选 usage event（P1）。

### 8.4 短录音与延迟

- 命令轮同样受 0.5s 最短录音门槛；按住时长含按/放余量，正常"删除"通常能过。真机验收覆盖"快速单说删除"；若实测经常被丢，再评估"短会话指令白名单"（放宽到 0.3s，且非命令短文本仍丢弃，不改变现状）。
- 命令路径跳过精修/翻译，**松键→完成删除**预计 0.5~1.5s（取决于 ASR final 尾延迟），比普通输入更快。

### 8.5 与现有交互的冲突检查

| 场景 | 现有行为 | 语音编辑影响 |
|---|---|---|
| 识别中按侧键取消 | 取消会话 | 不变；不会执行删除 |
| 待确认倒计时（macOS 0.4s）内按主键 | 转暂停确认 | 命令轮开始前先暂停；若此时文本尚未粘贴，L0 无记录 → 提示无目标（P1 可改为"修正待粘贴文本"） |
| 主键双击 | 取消会话 + 注入 Enter | 不变；双击期间不会发指令 |
| 编码器/按键映射删除 | 注入退格 | 不冲突，保留为旧通道 |
| refine 正在跑（上一轮） | 上一轮已粘贴结束 | 命令轮拦截在 refine 之前，不会并发 |

---

## 9. 配置与设置 UI

### 9.1 配置字段（两端同名同默认，建议 [voice_edit] 表）

| 键 | 类型 | 默认 | 范围/说明 |
|---|---|---|---|
| enabled | bool | **true**（决策点 D2） | 总开关；false = 完全回到现状 |
| default_chars | int | 1 | 单独说"删除"时删几个字，1..10 |
| max_chars | int | 20 | 单条指令允许删除的最大字数；超限按普通文本处理 |
| scope_seconds | int | 120 | L0 作用域时间窗（秒），10..600 |
| verify_mode | string | auto | strict / auto / off（见 §6.5；macOS 可切 strict，D8） |
| unverified_seconds | int | 8 | auto 模式下不可验证的允许窗口（秒），0..30 |

- macOS 落点：AppConfig 字段/默认值/parse/serializedText/legacy → ConfigFileModels → Config/config.example.toml → Doc/Ref/desktop-config.md。
- Windows 落点：app_config.h（新增 VoiceEditConfig 子结构）→ ApplyConfigValue + [voice_edit] TOML 表解析 → **WriteTo 白名单**（必加，参考 xiaomi_gateway_repeat_interval_ms 只读不写的既有坑）→ resources/config.template.toml → Doc/Ref/desktop-config.md。
- 热更：两端都走现有 updateConfig/UpdateConfig 路径；手改文件仍需重启（与现状一致）。

### 9.2 设置 UI

- macOS SettingsWindowController 新增"语音编辑"区块（checkbox + 默认删除字数 stepper + 说明文案），保存走现有 saveSettings。
- Windows settings_dialog.cc 新增同区块（checkbox + 数字输入/下拉 + 说明），保存走 SaveInputOptions。
- 文案必须双表（macOS L10nKey 英文+中文；Windows StringId 末尾追加+双表，缺一 LocalizationTablesAreComplete 红）。
- 说明文案要写清边界：只对最近一次语音输入、粘贴模式、光标仍在该文本末尾时生效；字幕/微信模式不支持。

### 9.3 与"按钮删除"的关系

- 保留所有按键映射能力（老用户/其他场景仍可用），语音编辑作为新增通道。
- 语音编辑落地并验收后，可评估**简化默认映射**（例如不再默认把返回键映射为退格），但**不删除用户已有配置**；该动作单独评审。

---

## 10. 平台落点清单（文件级）

### 10.1 macOS

| 类型 | 文件 | 内容 |
|---|---|---|
| 新增 | Sources/VoiceStickCore/VoiceEditCommand.swift | VoiceEditAction/VoiceEditCommand/VoiceEditParser/VoiceEditPlanner（纯逻辑：解析、grapheme 计数、删除计划） |
| 修改 | Sources/VoiceStickApp/AppDelegate.swift | 新增 readFocusedFieldState 闭包（AX value + kAXSelectedTextRange），注入协调器 |
| 修改 | Sources/VoiceStickCore/VoiceStickCoordinator.swift | finishWithFinalText 拦截；completePendingPaste 记录 LastInjection；L0/L1 校验；异步退格执行；toast 收尾 |
| 修改 | Sources/VoiceStickCore/AppConfig.swift、ConfigFileModels.swift | [voice_edit] 配置读写/默认值/序列化 |
| 修改 | Sources/VoiceStickApp/SettingsWindowController.swift、Localization.swift | 设置区块 + 中英文案 |
| 新增测试 | Tests/VoiceStickTests/VoiceEditCommandTests.swift | 解析正反例、计划算法向量（字段/光标/换行/边界） |

### 10.2 Windows

| 类型 | 文件 | 内容 |
|---|---|---|
| 新增 | src/voice_edit_command.h/.cc | 解析 + 计划算法（与 macOS 同向量） |
| 新增 | src/focus_text_reader.h/.cc | UIA TextPattern：尾部文本 + 折叠选区 + 光标是否在文末 |
| 修改 | src/voice_stick_coordinator.h/.cc、voice_stick_coordinator_tail.cc | FinishWithFinalText + watchdog 拦截；CompletePendingPaste 记录 LastInjection；L0/L1；异步退格；反馈/状态收尾 |
| 修改 | src/app_config.h/.cc、resources/config.template.toml | [voice_edit] 配置（含 WriteTo） |
| 修改 | src/settings_dialog.cc、localization.h/.cc | 设置区块 + 双表文案 |
| 新增测试 | tests/core_tests_voice_edit.cc + tests/test_suites.h + CMakeLists.txt | 解析/计划/执行器（Fake injector + Fake focus reader）；注册到 main() |
| 可选 | tests/integration_tests.cc | 协调器级集成：命令 final 不粘贴、调用注入、状态回 ready |

> Windows 若后续需要会话历史/编辑追踪，可先 rebase feat/windows-session-history（6/3），但**不阻塞本方案 P0**。

---

## 11. 测试与验收

### 11.1 单元测试（纯逻辑，CI 必过）

**解析器**（至少 30 正例 / 20 负例）
- 正例：数量缺省/中文数字/阿拉伯数字/两/俩、单位字/字符/句、最后/末尾、把字句、礼貌前缀/语气尾、单独删除/撤销/清空、尾部标点、全角空格。
- 负例："请把删除按钮改成红色"、"删不删"、"删除 100 个字"（超限）、"别删除"、"要不要删除"、"删除功能"、"把文件删除"、"你好世界删除最后两个字"、空串、纯标点。
- 归一化：首尾空白/标点/全角数字。

**删除计划**（至少 15 向量）
- 字段完整 + 光标在末尾（有/无换行）→ count 正确；
- 用户手动删过尾部 → eff 截断正确；
- 用户追加手打内容 → eff=0 拒绝；
- 光标在中间/选区非空 → 拒绝；
- N 超过剩余 → clamp；
- 整段删除 → 删完含换行；
- sentence 切分（多标点/无标点/换行）；
- grapheme：CJK、emoji、组合字符；
- 记录更新（remainingText/pressedEnter）。

**Windows 专属**：UIA 读取失败/不支持/多选区/空文档的 Fake 行为；WriteTo 持久化回归。

### 11.2 集成测试（无真机）

- Windows：Fake injector 记录 SendKeyCombo 调用序列（次数/键）；Fake focus reader 注入字段/光标；断言：命令 final 不调用 Paste、注入次数正确、状态回 ready、非命令 final 行为不变。
- macOS：纯逻辑在 swift run VoiceStickTests；协调器级可沿用现有 FSM 假物（如成本高可只做 planner 单测 + 手动验收）。

### 11.3 真机验收矩阵（必须）

| 维度 | 取值 |
|---|---|
| 平台 | macOS（CLT 构建 arm64）、Windows（MSI 安装版） |
| 应用 | TextEdit/备忘录、Chrome/Safari 输入框、微信输入框、Word/Pages、VS Code 编辑器、搜索框（单行）、终端（不可读场景验证拒绝） |
| 输入方式 | StickS3、小米遥控器（直连/网关）、Windows 本地麦克风 PTT |
| 配置 | auto_enter 开/关；refine 开/关；verify_mode=auto/strict |
| 指令 | U1~U8 全部；连续多次删除；删除到空；删除后手动继续输入再删；快速说"删除" |

**验收门槛**
- 正例删除成功率 ≥ 95%（可验证应用）；
- 误删（删掉非本次语音输入内容）**0 例**（严格验收，不允许"偶发"）；
- 失败场景 100% 有提示且无注入；
- 命令轮不产生粘贴内容、不写会话历史、不污染纠错对；
- 普通语音输入回归无变化。

### 11.4 指标（本地日志，不上传）

- 指令命中率（命中/说指令次数）、误触发率（被吞的普通文本/总普通文本）、删除成功/拒绝分布、松键→删除完成耗时、verified/unverified 比例。
- 无遥测：只写本机日志，遵守隐私红线。

---

## 12. 分期、工作量与风险

### 12.1 前置 Spike（0.5~1 人日，必须先做）

| # | 验证项 | 方法 | 通过标准 |
|---|---|---|---|
| S1 | macOS AX 焦点字段 + kAXSelectedTextRange 覆盖 | 临时脚本/调试日志，跑 §11.3 应用清单 | 文本类应用 ≥80% 可读；光标偏移与预期一致 |
| S2 | Windows UIA TextPattern 尾部文本 + 折叠选举 + 文末判定 | 临时 C++ 探针（复用 UIA 初始化代码） | 同上 |
| S3 | auto_enter 下退格次数与结果（含单行输入框、聊天框） | 手工/脚本注入验证 | 次数公式成立；异常应用记录 |
| S4 | CJK/emoji/组合字符退格粒度 | 手工注入验证 | 记录差异；必要时降级策略 |
| S5 | 快速说"删除"是否过 0.5s 门槛 | 真机 10 次 | 统计被丢比例，决定是否调门槛 |

Spike 结论（覆盖率表）直接决定 verify_mode 默认值与 Windows 是否首版上线。

### 12.2 分期

**P0（一期，本方案主体）**
- 解析器 + 删除计划（双端，同向量）+ 单测；
- macOS AX 字段/光标读取 + 协调器接线 + 记录/校验/执行/反馈；
- Windows UIA 读取 + 协调器接线（含 watchdog 分支）+ 记录/校验/执行/反馈；
- 配置/设置 UI/双语文案/示例配置/文档；
- 真机验收 + 回归。
- 上线前默认开关按决策点 D2 决定。

**P1（体验补全，3~5 人日）**
- "恢复刚才删除的/撤销删除"（本地撤销）；
- 词（中文分词）/行/段 单位；英文指令；
- partial 提示（"识别到删除指令…"）；
- 待粘贴文本修正（macOS 0.4s 窗内/暂停态）；
- 选中内容删除；
- 误判反馈优化（"疑似指令，未执行"提示/学习引导）。

**P2（语义编辑，需单独评审）**
- LLM 意图解析（"把刚才那句删了"/"更正式一点"/"把 A 改成 B"），模型只出编辑计划、代码守卫执行、破坏性动作二次确认；
- 跨轮改写/替换/插入。

### 12.3 工作量估算

| 工作 | 人日 |
|---|---|
| S0 Spike | 1 |
| macOS P0 | 4（解析 1 / AX 0.5 / 协调器 1 / 配置 UI 0.5 / 测试 1） |
| Windows P0 | 5（解析 1 / UIA 1.5 / 协调器 1 / 配置 UI 0.5 / 测试 1） |
| 真机验收矩阵 + 回归 | 1.5 |
| 文档/CHANGELOG/模板 | 0.5 |
| **合计** | **约 12 人日（两人并行约 1.5 周）** |

### 12.4 风险与缓解

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R1 | 误删用户内容 | 数据损失（最严重） | 两道闸 + N clamp + 选区/光标校验 + 失败拒绝；P1 本地撤销；真机矩阵"误删 0 例"门槛 |
| R2 | AX/UIA 覆盖率不足 | 功能在部分应用不可用 | Spike 先测；verify_mode 可配；拒绝时明确提示；Windows 可先 open beta |
| R3 | auto_enter 换行处理错误 | 多删/少删 1 字 | 可验证分支按实际字段判断；不可验证分支多删风险由短窗+少删量控制 |
| R4 | emoji/组合字符退格粒度差异 | 少删/多删 | grapheme 计数 + 真机矩阵；异常应用记录 |
| R5 | 聊天/搜索类已提交文本 | 用户以为能删但删不了 | 拒绝 + 提示；文档写明适用场景 |
| R6 | 输入法组合态 | 退格删拼音缓冲 | 可验证分支通常拒绝（字段/光标不匹配）；文档说明先上屏再删 |
| R7 | Windows 提权窗口 SendInput 静默失败 | 无效果 | 注入前 detect（复用 IForegroundProcessProbe）+ 提示 |
| R8 | 短口令被 0.5s 门槛丢弃 | "删除"无效 | 真机测；必要时 P1 放宽短会话并仅对指令白名单生效 |
| R9 | 配置不持久化（Windows WriteTo 白名单） | 重启回默认 | 单测覆盖 Save→Load 往返；参考既有 bug 教训 |
| R10 | 本地化双表/枚举下标 | 断言红/文案缺失 | 双表同步 + 现有断言；Windows 只在 StringId 末尾追加 |
| R11 | 与编辑追踪/纠错对污染 | 数据飞轮脏数据 | 命令轮先 finishEditTracking、不写 history、不进 diff |
| R12 | 双端行为漂移 | 体验不一致 | 解析/计划共用测试向量；配置同名同默认；文档同步 |
| R13 | 用户不知道有这功能 | 采纳率低 | 设置页说明 + 首次使用提示（P1）+ release notes |
| R14 | 隐私 | 读焦点字段引发顾虑 | 只读内存、不落库/不上传、日志只记长度；沿用既有隐私红线 |
| R15 | 与旧按钮删除并存 | 用户困惑 | 保留旧能力；文档说明二者关系；后续单独评审默认映射简化 |

---

## 13. 待拍板决策点

> 2026-10-20 用户已确认：全部采纳下表的推荐值（D1/D2/D4 等），本方案进入实现阶段；实现或验收中若发现推荐值不可行，按"安全优先"回落并同步记录。

| # | 决策 | 推荐 | 备选/影响 |
|---|---|---|---|
| D1 | 单独说"删除"默认删多少 | **1 个字**（等价一次退格，可连说）；"撤销/撤回"=整段 | 2 个字（对齐用户示例）；整段（破坏性大） |
| D2 | 默认开关 | **true**（用户明确提出需求，开箱即用） | false（项目惯例：实验特性默认关；需用户去设置里找） |
| D3 | P0 动作集 | **字 / 句 / 整段** | 是否首版就加"词"（需分词，P1） |
| D4 | Windows 不可验证时策略 | **auto：8s 内允许盲删** | strict：一律拒绝（更安全但 UIA 覆盖差的应用不可用） |
| D5 | 是否 P0 就做"恢复刚才删除的" | 否（P1） | 首版含撤销可提升安全感，+2~3 人日 |
| D6 | 自然句式范围 | P0 含"把最后两个字删掉"等 2~3 个常见句式 | 只认最严格的动宾式，误判更低但学习成本略高 |
| D7 | 生效输出目标 | 仅 focused_app | 是否允许 subtitle（无对象，不建议） |
| D8 | 默认 verify_mode | 两端 auto；若 spike 证明 macOS AX 覆盖率高，macOS 默认改 strict | 统一 strict（最安全，但 UIA 覆盖差的应用不可用） |
| D9 | 是否调整 0.5s 最短录音 | 暂不调，先真机测 | 放宽到 0.3s 并仅对指令生效（有成本） |
| D10 | 是否首版同时上线双端 | 是（同一评估周期） | 先 macOS 后 Windows（UIA 覆盖不确定） |

---

## 14. 备选与否决方案

1. **显式"指令模式"手势**（双击侧键/长按编码器后再说命令）：误判最低，但引入新手势=新的学习成本，与"降低学习成本"的初衷相悖。列为 P1 可选严格模式，不做默认。
2. **只用 LLM 判断指令**：延迟/成本/误判都更差，且会把"是否删除"的决策交给模型；一期否决，二期只做"模型出计划、代码执行"。
3. **partial 边说边删**：partial 非稳态、多数 provider 无提前量（§2.5）；否决。
4. **全选+重贴**：破坏撤销栈/富文本/应用语义；否决。
5. **系统撤销键（Cmd+Z/Ctrl+Z）**：撤销粒度不可控、可能撤销用户其他操作；否决（仅 P1 本地"恢复删除"）。
6. **设备屏显示命令反馈**：固件无 CJK、thinking 丢弃文本、协议改动成本高；否决（用 overlay/托盘）。
7. **微信输入法模式支持**：音频进虚拟麦、桌面端看不到文本；原理不可行，明确不支持。

---

## 15. 竞品参考（外部资料，未在本项目验证）

- Wispr Flow 的 Command Mode 支持 "delete last sentence" 这类免手编辑语音命令（[The Startup Starter Kit](https://www.thestartupstarterkit.com/tool/wispr-flow)）；说明"事后语音编辑"是行业已验证的方向。
- Typeless 的 Voice Superpowers 采用"先选中文本 → 按热键 → 说话改写"的交互（[Typeless 发布说明](https://www.typeless.com/help/release-notes/macos/voice-superpowers)），需要先选中，对双手占用场景不友好；本方案反过来——**免选区、直接说删多少**，更贴合 VoiceStick 的按键输入形态。

> 外部资料仅作方向参考，不构成产品承诺；未做竞品实测。

---

## 附录 A：端到端示例（U1，macOS，auto_enter=true）

~~~text
1. 用户按住主键说"你好世界"，松开。
2. ASR final "你好世界。" →（refine 开则精修）→ 粘贴 "你好世界" + Return。
   completePendingPaste 记录 LastInjection = {text:"你好世界", remaining:"你好世界",
   injectedAt:T, pressedEnter:true, app:"com.xxx"}
3. 用户再说"删掉最后两个字"（同一应用，<120s）。
4. finishWithFinalText("删掉最后两个字。") → 归一化 → parse → deleteLastChars(2)。
5. L0：记录存在、时间窗内、前台 app 一致 → 通过。
6. L1：AX 读到字段 "你好世界" + 换行、光标 UTF-16 offset=5（末尾）→ 通过；
   eff=4、newline=1、n=2、count=3。
7. finishRecognitionCycle + Ready + ui_state ready；异步发 3 次 Backspace（15ms 节拍）。
8. 更新 remaining="" / pressedEnter=false；toast "已删除 2 个字"。
9. 结果字段 "你好"，光标在"好"后。
~~~

## 附录 B：失败提示与错误码（日志用）

| code | 触发 | 用户提示 |
|---|---|---|
| no_injection | 无 LastInjection / remaining 空 | 没有可删除的语音输入 |
| scope_expired | 超过 scope_seconds | 最近一次语音输入已过期，未删除 |
| app_changed | 前台应用/窗口变化 | 已切换应用，未删除 |
| field_mismatch | 字段尾部不含注入文本 | 光标位置已变化，未删除 |
| caret_not_end | 光标不在末尾 | 请先将光标移回末尾 |
| selection_present | 有选区 | 请先取消选中 |
| verify_unavailable | 读不到字段/光标且不在 unverified 窗口 | 当前应用无法确认光标位置，未删除 |
| foreground_elevated | Windows 前台提权 | 目标窗口权限较高，无法删除 |
| accessibility_denied | macOS 无辅助功能权限 | 辅助功能权限未授予 |
| inject_failed | 注入 API 失败（无回执，仅日志） | 删除可能未生效，请检查输入框 |

## 附录 C：术语

- **LastInjection**：最近一次语音文本注入记录（文本/时间/应用/auto_enter/剩余文本）。
- **L0 作用域闸**：基于注入记录、时间窗、前台应用的粗校验。
- **L1 目标校验**：基于焦点字段文本 + 光标位置的精确校验（可验证平台）。
- **verified / unverified**：本次删除是否通过了 L1 校验。
- **grapheme**：用户可见字符（一个汉字/一个 emoji/一个组合字符均算 1）。

## 16. 实施记录（2026-10-20）

### 16.1 交付状态

| 项 | 状态 |
|---|---|
| 方案与决策 | 已定案（D1/D2/D4 等默认值已确认） |
| macOS 实现 | 完成；本机 `cd desktop/macos && swift build` 通过、`swift run VoiceStickTests` PASSED 1336/1336（基线 1053；含纯逻辑、配置、协调器级命令轮集成测试） |
| Windows 实现 | 代码完成（纯逻辑 + UIA reader + 协调器/配置/设置 UI）；纯逻辑在本机用 clang++ 与临时 main 对拍通过（与 macOS 同向量、39 条整段白名单 parity 0 missing）；**Windows MSVC 构建/CTest/真机待补** |
| 真机验收 | 待办（§11.3 矩阵；macOS 需 .app 形态 + 设备语音；Windows 需 Windows 机器） |

### 16.2 实施中的偏差与取舍（待评审确认）

1. **方案 §6.3 伪码与附录 A 矛盾修正**：字段以换行结尾时"remaining 前缀是字段后缀"必然不成立。实现改为先在"去掉一个尾随换行"的字段内容上做最长前缀匹配，再按实际字段是否真以换行结尾决定 newlineCount；U1 向量 count=3 与附录 A 一致。
2. **verified 分支执行后 pressedEnter 恒置 false**（比伪码保守）：避免后续降级到 unverified 时重复多发退格。
3. **同音别名"子→字"未纳入一期**：双端一致地不识别"删掉最后两个子"；该条从 §5.4 一期列表移到 P1（避免新增误判面）。
4. **Windows 纯逻辑 max_chars 不在 parser 层二次钳制**：由配置层钳 1..50 + 显式数量超限即非指令；macOS planner 另有 maxChars 纵深防御（行为等价，保留）。
5. **macOS AX 读取新增前台应用级 fallback**（Lead spike 实测 macOS 27 system-wide 返回 -25204）：`NSWorkspace.frontmostApplication → AXUIElementCreateApplication(pid) → kAXFocusedUIElementAttribute`，既有编辑追踪的 `readFocusedFieldText` 同步受益。
6. **macOS 设置页只暴露 enabled/default_chars**；max_chars/scope_seconds/verify_mode/unverified_seconds 走 config.toml 手编（双端一致）。
7. 本机 0.5s 最短录音门槛未调整（D9）；快速单说"删除"的真机通过率待 §11.3 验证，必要时再按 P1 方案放宽短会话。
8. **配置数值钳位口径双端对齐**（2026-10-20 收尾修）：[voice_edit] 四个数值字段改为区间钳位（同 Windows std::clamp），不再 macOS 特有的"越界回落默认/跨字段改写"；max_chars 范围统一 1..50，default_chars > max_chars 时由两端解析器同口径收紧 effective 删除量。
9. **Windows L1 只读字段尾部约 512 UTF-16**（方案 §6.3 的尾部窗口）：若 remainingText 长于该窗口，verified 分支会因尾部前缀不匹配而保守拒绝（安全方向，不误删）；macOS 为全量 AX + 2M 守卫，无等价窗口。
10. **整段删除的退格次数 = 剩余 grapheme 数**（不套 max_chars 的 20 上限，方案 §7.1 整段例外）；12ms 节拍下长文本（如 200 字约 2.4s）的真机可接受性待验收；字/句动作仍受 max_chars 约束。

### 16.3 验证证据（可复核）

- macOS：`cd desktop/macos && swift build` → Build complete；`swift run VoiceStickTests` → PASSED 1336/1336（基线 1053）。
- Windows 纯逻辑：`clang++ -std=c++20 -I desktop/windows/src desktop/windows/src/voice_edit_command.cc <临时 main>` 编译并运行全部解析/计划/UTF-8 grapheme 向量通过；Lead 独立 harness 与 39 条整段白名单 parity 复核通过。
- Lead 独立 spike（自建 NSTextView 宿主 + CGEvent 注入）：AX app 级读字段/光标可用；1 次退格删 1 个汉字；emoji 按 1 grapheme 删除；auto_enter 换行 + N 字需 N+1 次退格；caret 在中间可被读取。
- Windows 独立套件复核：`clang++ -std=c++20 -Wall -Wextra -Werror` 编译 `core_tests_voice_edit.cc` + 临时 main 运行 `RunVoiceEditTests()` exit=0（无 -DNDEBUG；assert 生效）；Lead fresh harness 32/32、双端整段白名单 parity 39/39。
- 待补：Windows `build_win.bat`/CTest、双端 §11.3 真机验收（含 AX/UIA 在 TextEdit/Chrome/Word/VS Code/终端等的覆盖率）。

### 16.4 后续（P1）

词/行/段单位、英文指令、删除恢复（撤销删除）、待粘贴文本修正、选中内容删除、partial 提示；"子→字"同音别名。

### 16.5 Windows 环境补验清单（阻塞发布）

1. `build_win.bat`（MSVC）编译：UIA 头与调用签名、`uiautomationcore`/`oleaut32` 链接、`focus_text_reader.cc`。
2. `ctest --test-dir desktop/windows/build-x64 --output-on-failure`：含 `RunVoiceEditTests`（纯逻辑 + 协调器集成）。
3. 真机 UIA 覆盖率与退格粒度（§11.3 矩阵：记事本/Chrome/Word/VS Code/微信输入框/终端）。
4. 设置对话框新增控件布局/中英文案、保存热更、重启持久化。
5. UIPI 提权前台拒绝路径；快速单说"删除"是否过 0.5s 最短录音门槛。
6. 整段删除长文本的实际耗时（12ms×字数）与可接受性。

---

## 17. targeted 词/短语删除与替换（2026-10-27 实现）

> 状态：已实现（工作区，未发布；2026-10-27 返修轮已合入）。macOS 本机 `swift build` 通过、`swift run VoiceStickTests` **PASSED 1525/1525**（基线 1351；含 targeted 解析/匹配/计划向量、协调器级调用顺序/安全闸/换行拆解/配置清理代际测试）。Windows 纯逻辑与 macOS 同向量、本机 clang `-Wall -Wextra -Werror` 对拍通过（另经 ASan+UBSan 构建运行通过，含既有套件回归，临时 harness 在 /tmp，不在仓库）；Windows MSVC 构建/CTest 与双端真机验收**待补**（§17.5）。

### 17.1 需求与语法（P0）

在既有尾部删除之上，支持编辑最近一次语音输入中的**指定词/短语**：

| 句式 | 例句 |
|---|---|
| 动宾式删除 | 「删除刚才的北京」「删掉刚才的北京」「删去/去掉刚才的北京」（作用域也认「刚才/刚刚/刚刚的」） |
| 把字句删除 | 「把刚才的北京删掉/删除/去掉/删去」 |
| 把字句替换 | 「把刚才的北京改成/改为/换成/替换为/替换成上海」 |
| 位置限定 | 上述结构支持「第一个」「最后一个」（含「刚才的最后一个」语序）；无位置限定时**仅 1 个精确匹配才执行**，多处匹配安全拒绝并提示 |

- **作用域强制**：不带「刚才的」的「把A改成B / 删除A」一律非指令（走普通文本），避免把口述「请把删除按钮改成红色」误判为命令。
- **词 = 连续文本片段精确匹配**（不引入分词依赖），非重叠匹配；目标 ≤32、替换 ≤64 graphemes（双端一致硬常量，独立于 `max_chars`，**无新配置字段**）。
- **归一化**：去首尾空白/尾部标点、全角数字转半角、剥绝对开头礼貌前缀一层（请/帮我/麻烦…）、剥绝对结尾语气词一层（吧/啊/呀/哦/嘛）——**操作数内部有意义空白保留**（不照搬旧 parser 的全量去空白）；疑问尾「吗/呢」→ 非指令；否定（target 含「不要」或以「别」开头）→ 非法指令（吞掉提示）。已知限制：语气词剥离使「改成好吧」类替换得到「好」。
- **解析三态**：指令 / `.invalid`（结构命中但空目标/空替换/多替换分隔符/超长——吞掉并提示，不上屏不修改）/ 非指令（原链路）。旧解析器优先、targeted 其次（「删除刚才的」仍是旧整段删除）。

### 17.2 安全模型（与旧尾部删除的关键差异）

1. **targeted 动作任何 `verify_mode`（strict/auto/off）下都必须 verified**：读不到字段/光标一律拒绝，**不存在 8s unverified 盲删/盲改**。
2. **L1 完整后缀校验**：字段（允许末尾一个**已记录** `auto_enter` 换行）必须以**完整** `remainingText` 为后缀——**不用旧「最长前缀」容错**（用户手工删改会使目标偏移不可靠，宁可拒绝）。
3. **平台精确选区执行**（不轮询方向键、不全选重贴、不用系统撤销）：
   - macOS：`AXUIElementSetAttributeValue(kAXSelectedTextRange)` 写选区 + `kAXSelectedTextAttribute` 读回（`FocusedFieldAXReader.writeSelectedRange/readSelectedText`，AppDelegate 注入协调器）。
   - Windows：UIA TextRange `Select()` + `GetSelection()->GetText()` 读回（`ITargetedFieldEditor`，focus_text_reader.cc 生产实现；doc 长度用 Start 端点前进 INT_MAX 实测，tail 窗口同 reader 的 512 定义）。
4. **读回逐字确认**：实际选中文本 == 目标词才执行；不符 → 恢复末尾光标、零删除/零粘贴、**记录保留**（字段文本未被改动）。
5. **单次动作**：删除 = 一次 Backspace；替换 = 现有粘贴通道 `pressEnter=false`（不额外触发 Return，保留原 `auto_enter` 换行与 `pressedEnter`）。
6. **恢复末尾光标 + 终验**：修改后把光标恢复到**字段末尾**（不是被改词之后），读回字段文本/光标终验；通过才更新 `remainingText`（`pressedEnter` 不变——targeted 不消费换行）并允许连续编辑。
7. **结果未确认**：修改已发出但终验不符 → 不自动重试、不宣称成功、不盲目撤销；**注入记录失效**（防后续基于陈旧偏移继续编辑）+ 提示用户检查。修改前失败记录保留。
8. **串行 + generation**：执行/终验回调携带 `voiceEditGeneration`（新注入 +1）；stale 回调直接丢弃，新注入/新记录不被旧回调覆盖。macOS 全程主线程（单次动作无循环、无 UI 阻塞）；Windows UIA COM STA 留 UI 线程，延时终验走 worker sleep + `RunOnUiThread` 回收（与既有退格执行同范式）。
9. 选区非空、密码框、提权前台（Windows UIPI）、前台应用变化、超出作用域时间窗 → 全部拒绝并提示；拒绝路径**零文本修改**。

### 17.3 实现落点（双端同构）

| 层 | macOS | Windows |
|---|---|---|
| Core 纯逻辑 | `Sources/VoiceStickCore/VoiceTargetedEdit.swift`（Parser/Matcher/Planner + 失败码） | `src/voice_targeted_edit.{h,cc}` + 共享 `src/utf8_grapheme.h`（从 voice_edit_command.cc 抽出） |
| 平台选区 | `FocusedFieldAXReader.writeSelectedRange/readSelectedText`；协调器闭包 `writeTargetedSelection/readTargetedSelectedText` | `ITargetedFieldEditor`（focus_text_reader.h/.cc），`SetTargetedFieldEditor` 注入 |
| 协调器 | `handleVoiceTargetedEditIfMatched` / `evaluateVoiceTargetedCommand` / `executeVoiceTargetedEdit`（VoiceStickCoordinator.swift） | `HandleVoiceTargetedEditIfMatched` / `ExecuteVoiceTargetedEdit` / `FinalizeVoiceTargetedEdit`（voice_stick_coordinator_tail.cc） |
| 拦截点 | `finishWithFinalText`（旧解析器之后） | `FinishWithFinalText` + finalizing watchdog（两处） |
| 文案 | L10nKey `voiceTargeted*` ×7（中英双表） | StringId `kVoiceTargeted*` ×7（末尾追加，中英双表） |
| 测试 | `VoiceTargetedEditTests.swift`（纯逻辑向量）+ CoordinatorFsmTests targeted 段（调用顺序/安全闸/stale） | core_tests_voice_edit.cc targeted 段（纯逻辑同向量 + _WIN32 协调器级 Fake） |

日志纪律：只记动作、目标长度、匹配数/occurrence、结果码——不记目标词/替换词/字段内容。

### 17.4 验证证据与待补

- macOS：`swift build` 通过；`swift run VoiceStickTests` PASSED 1525/1525（基线 1351）。协调器级 Fake 断言完整调用顺序：L1 读 → `select(loc,len)` → `selected`（==target）→ 一次 `backspace` 或 `paste(false)` → `select(end,0)` 恢复 → 终验 `state` 读；读回不符只在 `select(end,0)` 前停止；stale generation 不弹旧 toast 不污染新记录；off 模式不可验证仍拒绝；updateConfig 关闭功能清记录同时递增代际（在途终验被丢弃）；remaining 自身以换行结尾（口述多行 + auto_enter）选区/终验拆解正确（删「你好」→ 字段「\n\n」、remaining「\n」）。
- Windows 纯逻辑：`clang++ -std=c++20 -Wall -Wextra -Werror` 编译 + 本地运行全部解析/matcher/planner 向量通过；**ASan+UBSan 构建同样通过**（`LastTokenIndex` 命中推进到 `text.size()` 后不再越界读）；仓库既有 voice edit 套件（重构 grapheme 抽取后）本地对拍 PASSED。
- 解析器返修向量（双端同表）：`把刚才的删除按钮改成红色` → replace(删除按钮, 红色)；`把刚才的北京改成删除` → replace(北京, 删除)（**分隔符优先**：句中存在替换分隔符即按替换解析，删除分支仅在无分隔符且动词恰在整句末尾时成立）；`把刚才的删除删掉` → delete(删除)、`把刚才的删掉删除` → delete(删掉)（lastTokenIndex 命中即推进、不额外跳 grapheme，紧邻动词可被扫描为 best）；`请把删除按钮改成红色` 保持普通文本负例。
- **待补（阻塞发布）**：Windows `build_win.bat`/CTest（本机无 Win32 工具链，UIA/COM 代码未编译；含 512 窗口变长替换终验的 `_WIN32` Fake 用例待 Windows 环境执行）；双端真机验收（§11.3 矩阵 + targeted 专项：TextEdit/备忘录/记事本/Chrome/Word/VS Code 的 AX/UIA 选区写入覆盖率、CJK/emoji/组合字符选区、输入法组合态、auto_enter 换行场景、长文档 512 窗口边界）。

### 17.5 返修记录（2026-10-27，评审后）

1. **C++ `LastTokenIndex` 越界**：token 命中推进 `index == text.size()` 后仍 `DecodeUtf8(text, index)` 读越界；修复为命中即推进 + 循环边界保证（ASan 复核通过）。同函数顺带修复"命中后额外跳一个 grapheme"导致紧邻动词漏扫的语义缺陷（双端对齐为"命中即推进到 token 后"）。
2. **把字句分支优先级**：分隔符优先于删除动词（两个最小复现见 §17.4）；删除分支要求无分隔符且动词恰在句尾。
3. **Windows 终验公共后缀比较**：tail 为 512 窗口且替换变长（expected_tail>512）时按双方末尾 min(byteCount) 比较；expected 为空要求 actual 为空；可读/折叠光标/caret_at_end 校验不放松。`_WIN32` Fake 用例：506 前缀 + 6 字注入、替换 +2 → 必须成功并更新记录。
4. **macOS remaining 换行结尾拆解**：planner 与协调器统一 pressedEnter 优先判 `remaining+"\n"`（对齐 Windows），`你好\n`/`你好\n\n` 场景选区偏移与终验文本正确。
5. **解析一致性**：Swift 动宾式补 `ContainsNegation`（与 C++ 同语义）；单行约束双端同时拒 `\r` 与 `\n`（防 CRLF 绕过）。
6. **macOS 配置清理代际**：`updateConfig` 关闭 voiceEdit / 离开 focused_app 清记录时同步 `voiceEditGeneration += 1`，在途 0.2s 终验不得弹旧成功 toast。
7. **失败路径安全收尾**：Windows `SelectAndReadBack` 失败尽力 `MoveCaretToEnd()`；Backspace KeySpec 解析失败按修改前失败处理（恢复光标、记录保留、不按结果未确认清记录）；macOS 同查（select 写失败尽力恢复光标）。
