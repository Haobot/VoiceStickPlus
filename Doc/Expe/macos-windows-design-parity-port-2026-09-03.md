# macOS 端全面对齐 Windows 设计：11 阶段大移植的成果与经验（2026-09-03）

- 日期：2026-09-03
- 相关位置：`desktop/macos/Sources/VoiceStickApp/`（22 改 + 12 新，约 +3500 行）、`Doc/Ref/protocol.md`、`Doc/Ref/desktop-config.md`、双 `README`、`CHANGELOG.md`（Unreleased）
- 前置阅读：`Doc/Expe/xiaomi-remote-macos-port-p5-2026-09-02.md`（P5 小米遥控器移植，方法论先文；本文 §3 是其 §4/§5 在大规模 UI/配置移植场景的扩展）
- 时效声明：文中文件:行号、常量值均为记录时点结论，引用前以当前源码为准。
- 验证程度声明：本次**仅** `swift build` + `swift run VoiceStickTests`（375/375）验证；runner 只链 VoiceStickCore，app 层全部新代码与真机链路（编码器/敲击/电量监测/腾讯 ASR/热键/本地化切换）**未经真机验收**。

## 1. 成果速览

用户诉求一句话：「macOS 端还是老设计，同步 Windows 端新设计，外观与功能尽可能一致」。按 11 阶段计划执行（会话级计划，未入 `Doc/Plan/`——设计权威来源是 Windows 源码本身，无独立方案文档）：

1. 配置基座：`AppConfig` 补齐 Windows 已有字段（LLM/腾讯/设备交互/编码器全局默认 + `[device.<id>.interaction|encoder]` 覆盖、`ui_language`、`show_imu_debug` 等），解析/序列化语义逐条对齐 `app_config.cc`。
2. `OverlayController` 悬浮窗重写（贴边/主题/尺寸/位置/双按钮提示）与 `SubtitleController` 字幕模式，视觉对齐 Windows D2D 浮窗与字幕窗。
3. 托盘菜单重构（`StatusController`）：按设备子菜单、交互模式/输出切换、电量后缀、固件更新入口。
4. 全局热键（`GlobalHotkeyManager`→`remote_button`）+ 登录自启（SMAppService）。
5. LLM 精修（`LLMRefinementClient` 非流式，prompt/热词守卫逐字对齐）与翻译；腾讯云 ASR 全链路（`TencentASRClient`+`TencentASRVocabClient`）。
6. 设置窗扩展对齐 Windows 设置对话框分区与开发者模式门控。
7. 四个设备级对话框：设备交互 / 编码器（13 项）/ 遥控器 / 电池电压监测（power_log 状态机 + CSV/PNG 导出 + `usb_auto_off`）。
8. 编码器交互全链路：`encoder_rotate` 旋转注入（`EncoderRotateSpeedEstimator` EWMA 快慢分档逐注释对齐 `encoder_speed.h`）、`tap` 敲击映射、编码器单双击动作、五项设置逐台单播下发。
9. 小米遥控器补齐：0x180F/0x2A19 电量读取合成 `battery_status`、遥控器设置对话框（200–600ms clamp 一致）。
10. UI 本地化：`Localization.swift` 243 键中英双表逐条对齐 Windows `localization.cc`，`ui_language` 配置 + 设置窗语言下拉 + 保存后菜单/窗口重建。
11. 文档口径修订（protocol/desktop-config/README×2/desktop-architecture/distilled/CHANGELOG）。

有意不移植（保持 Windows 独有）：体感鼠标、流式精修（SSE）、热词挖掘/划词加词、微信输入法模式（虚拟麦克风）、心跳探针（`battery_status_request` 周期心跳）、GAP 设备名 keepalive fallback。

## 2. 对齐方法论（本次新增，大规模移植场景）

- **对齐基准是 Windows 源码的当前行为，不是仓库文档**。移植期间发现 `Doc/Ref/desktop-config.md`/`protocol.md`/双 README 的平台标注本身已过时（「仅 Windows 消费」「macOS 无对话框」「所有设备都显示该菜单项」与实际两端代码均不符）。正确顺序：以 Windows 源码行级行为为规格移植 → 移植完回修文档。先信文档会移植错。
- **Windows 的 bug/quirk「修而不随」，并记录差异**。例如 Windows 设备交互对话框 Wake Sensitivity 下拉第一行是空串（bug）、灵敏度滑块数值不实时刷新（quirk）——macOS 版按正常行为实现，在委托规格与代码注释里写明偏离点，避免下次「对齐」时把 bug 当规格抄回来。
- **配置解析语义逐条对齐而非自行设计**：非法枚举值回默认（不保留 fallback）、按键字段仅 `KeySpec.parse` 成功才覆盖（`press_key` 唯一允许空）、数值字段越界/解析失败回落 defaults、设备表以全局默认填平、「与全局默认相等则不落盘」。这些都在 `app_config.cc` 里有现成答案，照抄语义比自己定规则省事且跨端配置兼容。
- **本地化双表防漂移机制**：稳定语义 key 枚举（`CaseIterable`）+ 中英两张字典 + `tablesAreComplete()` 运行期 assert（启动时）+ python 静态 diff 校验 key 集合——对齐 Windows `LocalizationTablesAreComplete` 思路；不用英文原文做 key，文案微调不破坏索引。
- **大型移植的阶段切分**：配置基座 → UI 壳 → 功能链路 → 本地化 → 文档，每阶段 `swift build` 卡点过才进下一阶段；本地化（~10 文件文案点位替换）独立委托后台 coder 执行，主线程做文档收尾。

## 3. 工程坑（判据式）

- **后台 coder 子代理改代码树期间，主代理不要并行跑 `swift build`**：判据是构建报 `error: input file '<路径>' was modified during the build`——不是代码问题，是子代理正在写该文件。等子代理完成通知后再统一构建验证；测试二进制可能是旧构建产物，结果不代表最新源码。
- **Edit 的 `old_string` 跨函数边界时小心吞掉下一行**：长 `old_string` 若以函数结尾的 `}` 收尾、实际文本含紧跟的下一行，替换后易丢行。跨边界替换后立刻重读该区域核对。
- **「文档说某端未实现」要用源码复核**：本次三处文档标注（编码器/交互「仅 Windows 消费」、遥控器「macOS 无对话框」、交互菜单「所有设备都显示」）与代码事实不符。经验：`Doc/Ref/` 承载事实但仍可能滞后，涉及「某端有没有」的断言，grep 两端源码再引用。
- **macOS 无 GAP 设备名读取的 keepalive 机制**：Windows 电量监测里「GAP 设备名 keepalive fallback」是平台特有机制，移植时明确跳过并记录，不要为对齐而强行造等价物。

## 4. 验证

- `cd desktop/macos && swift build`：Build complete（2026-09-03 时点）。
- `swift run VoiceStickTests`：375/375（含 ATVV golden fixtures 回放、Opus 回环 SNR 51dB）。
- 本地化双表静态校验：en/zh 各 243 条，key 集合 diff 为空；运行期 `tablesAreComplete()` assert 兜底。
- **未验证**（待真机/手动验收）：编码器旋转快慢分档与单双击动作、敲击映射、电池电压监测（锚定/导出 CSV/PNG/`usb_auto_off` 回推）、腾讯 ASR 识别与热词表、全局热键、登录自启、语言切换后全点位文案、字幕模式显示。

## 5. 遗留 / 观察项

- 真机验收清单见上节「未验证」；建议按「编码器交互 → 电量监测 → 腾讯 ASR → 语言切换」顺序过。
- app 层无自动化测试的老问题进一步放大：本次 +3500 行几乎全在 VoiceStickApp（runner 只链 VoiceStickCore），配置解析/窗口逻辑只能靠编译 + 人工。若再扩 app 层逻辑，值得把 `AppConfig` 解析下沉 Core 或引入可跑 XCTest 的环境。
- Windows 端与固件本次零改动；全部改动未 git 提交（含 12 个新文件未 add）。
- macOS 仍未消费火山 `boosting_table_id`/`correct_table_id`（config 键都未解析），热词裁剪/候选挖掘仍为 Windows 独有——见 `Doc/Expe/hotword-two-pass-and-candidate-mining-2026-07-28.md` 追加节。

## 6. 本次文档同步清单（供复查）

- `Doc/Ref/protocol.md`：`battery_status`/`power_mgmt`/`remote_button_*`/`battery_status_request` 补录，控制事件表 Direction 列按两端实际发送修正，`encoder_status` 用途、USB 自动关机、ATVV 电量小节更新。
- `Doc/Ref/desktop-config.md`：编码器/设备交互两节标题与「仅 Windows」口径修订、小米 `double_click_ms` 对话框 clamp、`ui_language`/`show_imu_debug` 补录、火山表 ID 标注 Windows-only。
- `README.md` / `README.zh-CN.md`：特性清单平台标签同步（编码器/按设备覆盖/敲击映射/小米遥控器去「Windows」标签，LLM 精修翻译标双端）。
- `desktop/macos/Config/config.example.toml`：设备交互/编码器/按设备覆盖示例，`ui_language`/`show_imu_debug` 注释项。
- `Doc/Agent/desktop-architecture.md`：macOS 模块清单更新。
- `Doc/Expe/claude-memory-distilled.md`：精修状态两条（§3.8/§8）修正、§7.3 点 10、§10 速查追加（见下）。
- `CHANGELOG.md`：Unreleased 大条目。
