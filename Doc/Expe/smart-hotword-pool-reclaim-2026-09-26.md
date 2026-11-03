# 智能热词管理落地与回收池分类错位（回归目标重设计）

- 日期：2026-09-26
- 相关文件：`desktop/windows/src/hotword_activity.{h,cc}`、`hotword_context.{h,cc}`、`hotword_activity_dialog.{h,cc}`、`voice_stick_coordinator.{h,cc}`（MaybeSmartHotwordUpdate / AddHotwordWithEviction / RemoveEvictedWords）、`win32_app.cc`、`settings_dialog.cc`、`localization.{h,cc}`
- 配置事实源：`Doc/Ref/desktop-config.md` `smart_hotword_enabled` 条目；设计方案 `.trae/documents/smart-hotword-management-plan.md`
- 提交：当日改动未提交（工作树），引用行号以当前源码为准

## 0. 当日成果摘要

在手动热词分区（`hotword_categories`，25 词/分类）之上落地三层自动化，默认开启（`smart_hotword_enabled = true`）：

1. **上下文识别自动切换**：对 ASR 最终文本做四分类（程序/AI/数学/语文，共 ~166 关键词）加权评分（`hotword_context.cc`）；得分 ≥2.5 + 命中 ≥3 词 + 连续 2 次决策一致 + 300s 冷却 + **当前分类热词命中保护**（最终文本出现当前分类热词即拒绝切换）+ 滞后加成 1.0。45 条标注语料单测。
2. **容量动态管理**：软限 5 词（超出"监控中"，仅日志+可视化标注），硬上限 25；划词/提炼/纠错加词走 `AddHotwordWithEviction`，满员逐出最低活跃度词（manual 保护）腾位。
3. **活跃度评估与回收**：复用 `HotwordScore`（与 `scripts/e2e_test/asr_bench/hotword_select.py` 契约一致）；逐出词进独立回收池 `%APPDATA%\VoiceStick\hotword_evicted.json`（不污染 `hotword_usage.json` 数组契约）；池内词再现于最终文本即自动回归。

配套：`HotwordActivityDialog` 可视化（双 ListView 报表，仿 PairDeviceDialog 模式）、设置开关、zh/en 本地化 22+ 条、回收池行首 ✗ 点击删词 + "清空回收池"按钮（确认框）。

关键接线模式：**`on_config_mutated` 回调**——协调器后台线程直接改 `config_` + 落盘，拷贝传外壳，外壳只 `config_ = std::move(cfg)`；**严禁外壳调 UpdateConfig/ApplyUpdatedConfig**（会取消活跃会话）。锁序：`smart_hotword_mutex_` → `hotword_usage_mutex_`，不得反序。

## 1. 症状：回收池分类错位

用户报告（活跃度对话框）：回收池 6 个明显数学术语（三次根号/公比/内错角/原点/同位角/因式分解）"原分类"列全部显示"语文"，而语文分类空（0 词）、数学满 25、`active_hotword_category = "数学"`。

## 2. 日志判据

`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 中：

```
smart hotword: evicted '一次函数' to make room for '勾股定理'
smart hotword: evicted '原点' to make room for '分母有理化'
...（连续 17 条，10:56:34~35 一秒内密集出现）
```

`evicted 'X' to make room for 'Y'` = `AddHotwordWithEviction` 路径（划词连续加词满员逐出）。**密集连续逐出 + 逐出词与腾位词同属一个语义域** = 用户把某领域词批量划词加进了（语义不匹配的）当时活跃分类，旧词连环被逐。回收池 JSON 中逐出词 `category` 字段 = 逐出时所在分类（本例"语文"），不是词的语义分类。

## 3. 根因

`EvictWordToPool` 的 `entry.category = category.name` 记录的是**逐出时所在分类**。用户测试时活跃分类=语文且把数学词划词加了进去，满 25 后继续加触发连环逐出；之后语文被清空、数学填满。危害不止显示错乱：旧回收逻辑按 `reclaim->category` 回归——数学词再次被说出会回归到空的、非活跃的语文分类（ASR 只用活跃分类热词），**回收完全失效**。

**走不通的修复方向（先排除）**：逐出/回收时用语义分类器（`HotwordContextClassifier.Score(词)`）修正——实测数学关键词表判不出"因式分解/公比/内错角/同位角/原点/三次根号"任何一个（词表只有 37 个通用词如"方程/几何/概率"）。**单词级语义判定依赖关键词表命中，词表永远覆盖不全领域术语，此路不可依赖。**

## 4. 修复（机制）

`MaybeSmartHotwordUpdate` 中回收分支**从切换决策之前挪到之后**，回归目标从"原分类"改为**当前活跃分类**（`voice_stick_coordinator.cc` 回收分支）：

- 触发回收 = 用户正在说这个词 = 当前语境需要它；热词表只服务活跃分类，**可用性 > 语义归属**。主路径（活跃=原分类）行为不变，错位路径词立即可用。
- 挪到上下文切换之后：先切到最新语境分类再回收，词落在切换后的活跃分类；同时避免回收词先进当前分类、再被"当前分类热词命中保护"误判为停留信号拒绝切换。
- 池内 `category` 保留为"原分类"展示记录（对话框列名从"分类"改"原分类"），清池改按 word 匹配（跨分类残留一并清理）；词已在活跃分类时也清池（数据不一致自愈）。
- 用户存量 6 条错位数据**无需迁移**：下次数学语境说出即回归数学（满 25 自动逐冷腾位）。

## 5. 验证

构建通过、`voicestick_windows_tests.exe` EXIT=0、重启日志正常。✗ 图标视觉/点击删词/清空按钮交互为构建+启动级验证，UI 点击细节待用户日常使用确认。

## 6. 经验与教训

1. **"分类"是场景维度，不是词的固有属性**。任何"记录词归属分类"的设计都要先问：这个分类是词的语义标签，还是词当时所在的抽屉？回收/回归类功能的目标应该是"让词重新可用"（活跃分类），而非"回到原抽屉"。
2. **关键词表分类器只能用于文本级语境判定，不能用于单词级语义判定**——领域术语（因式分解/内错角）不在通用词表里，文本级判定靠的是上下文多词叠加，单词级没有这个冗余。
3. **回收/逐出/切换在同一函数内时注意执行顺序**：回收放在切换后，避免回收词污染切换决策的"当前分类热词命中保护"。
4. **localization 新增 StringId 必须同步 `kStringCount`**（localization.cc 顶部，= 枚举尾 +1）：漏改的报错是 `array::operator[]` 的 C2131/`_CrtDbgReport` 编译错（指向 localization.cc 的表赋值行），报错位置远离根因，认准这个特征。
5. **跑 Windows 单测前先结束运行中的 VoiceStick.exe**：测试含时序断言（编码器 EWMA/overlay 隐藏等），与真实应用并发时会**偶发失败且失败点漂移**（两次分别在 core_tests.cc:4062 编码器注入、:7893 overlay 隐藏），极易误判为代码回归。先 Stop-Process 再跑，全绿后重启应用。
6. 旧实例若以管理员运行（提权改名子进程/手动管理员启动），普通 `Stop-Process` Access denied——同 `claude-memory-distilled.md` §3.1 已有记录，`Start-Process powershell -Verb RunAs` 提权杀（会弹 UAC）。
7. **`git add -f` 教训当日重演（f75af4f5 同款）**：智能热词管理的 4 个功能提交（78bfabe 等）只含已跟踪文件的修改，6 个新源文件（hotword_activity / hotword_context / hotword_activity_dialog 的 .h/.cc）被 `.gitignore` 静默排除——CMakeLists、协调器、单测都已引用它们，**全新克隆无法构建**。当日总结时核对 `git ls-files` 才发现，a24f933 补提交（1075 行）。判据：`git status --short desktop/windows/` 干净 ≠ 完整，**新文件必须 `git ls-files` 逐个点名验证**；提交含 desktop/windows 新文件的改动后，立刻 `git show --stat` 核对新文件在列。

## 7. 遗留 / 观察项

- 回归活跃分类引入跨语义场景：活跃语文时说出数学词会进语文分类（语义污染），依赖活跃度竞争自然逐出回池，实际影响待观察。
- 智能切换的阈值参数（2.5/3 词/2 次/300s）为经验值，真实使用中的误切换/该切不切待反馈调优。
- ✗ 图标为软件绘制位图（深红 RGB(200,50,50)，16dp）；颜色/大小如需调整改 `CreateXBitmap` 常量即可。
