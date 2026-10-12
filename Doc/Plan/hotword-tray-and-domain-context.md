# 热词碟与领域上下文推理方案

状态：已实施（Windows 端，2026-10-07）。实施偏差三处（其余按本方案落地）：① `BuildHotwordTray` 落在 `hotword_activity.{h,cc}` 而非 §3.1 原定的 `hotword_selector.*`（避免 hotword_selector.h → app_config.h 循环依赖）；② 配置用平铺键 `hotword_tray_enabled/size/min_score` + 顶层 `[hotword_domain_map]`，而非 §5 原定的 `[hotword_tray]` 子表（与代码库平铺键惯例一致）；③ §6.2 评测承载为 C++ CTest 锚点单测（`core_tests.cc` 内 120 条标注集 + 碟/深度/提示语单测），不建 Python `context_bench.py`（避免 Python 复刻评分逻辑漂移）。
范围：Windows 桌面端（macOS 端智能热词体系暂缺，本方案不覆盖，见 §8 开放问题）
关联：`Doc/Plan/hotword-eval-and-prioritization.md`、`Doc/Expe/smart-hotword-pool-reclaim-2026-09-26.md`、`Doc/Expe/hotword-two-pass-and-candidate-mining-2026-07-28.md`

---

## 1. 需求与现状映射

产品需求两条：

1. **全自动上下文推理**：实时分析输入上下文，识别知识领域并自动切换，提供领域适配的专业内容；指标——响应 ≤300ms、领域识别准确率 ≥95%、内容适配相关性 ≥85 分、支持 ≥20 个核心领域、多级内容深度适配、内容质量控制。
2. **热词碟**：设置界面语音识别模块下方新增托盘组件，自动聚合各热词分类中活跃度较高的热词；识别时优先匹配碟内热词，未命中再检索各分类；活跃度标准明确、内容随使用频率动态更新。

### 1.1 现状盘点（可复用资产）

| 能力 | 现状 | 位置 |
|---|---|---|
| 上下文领域识别 | 关键词加权规则引擎，内置 4 分类（程序/AI/数学/语文），阈值 2.5 + 最少 3 命中 + 连续 2 次一致 + 冷却 300s + 滞后加成 1.0，500 字 <1ms | `hotword_context.h/.cc`（`HotwordContextClassifier`） |
| 热词活跃度 | `score = 1.0*log1p(count) + 0.5*exp(-age/30d) + 2.0*manual`，冷淡线 0.3 | `hotword_selector.h`（`HotwordScore`）、`hotword_activity.h`（`IsCold`） |
| 使用统计 | 每条 ASR 最终文本落账全部分类词 + 回收池词，`hotword_usage.json` 持久化 | 协调器 `RecordHotwordUsageFromText` |
| 逐出/回收池 | 分类 25 词上限，满员逐出最冷词，池内词回升自动回归 | `hotword_activity.h/.cc`、协调器 `MaybeSmartHotwordUpdate` |
| ASR 热词直传 | 活跃分类词按活跃度排序 → corpus 80 token 预算裁剪 → 火山 corpus / 腾讯 vocab | 协调器 `RankedHotwordsForAsr`、`asr_protocol.h`（`FitHotwordsToCorpusBudget`） |
| 精修热词兜底 | 精修 prompt 热词段（top-50）+ 结果守卫 `RefineResultKeepsHotwords` | `llm_refinement_client.h` |
| 可视化 | 活跃度对话框（各分类词活跃度 + 回收池管理） | `hotword_activity_dialog.h/.cc` |
| 设置页热词块 | 分类下拉 + 词表编辑 + 智能热词开关 + 活跃度入口 + 候选词区，声明式布局 | `settings_dialog.cc`（语音识别区内） |

### 1.2 缺口

| 需求 | 缺口 |
|---|---|
| ≥20 领域 | 内置仅 4 分类，规则表硬编码在 `DefaultRules()` |
| 多级内容深度适配 | 精修 prompt 与领域无关，无分档机制 |
| 热词碟 UI | 无跨分类高活跃词聚合视图 |
| 碟优先匹配 | corpus 组装只取活跃分类词，碟词（跨分类高分词）不参与 |
| 指标度量 | 无领域识别标注评测集与相关性评审口径 |

### 1.3 需求落点映射（产品语言 → 工程机制）

| 产品要求 | 本方案落点 |
|---|---|
| 实时上下文分析 ≤300ms | 本地规则引擎 <1ms（现有承诺），LLM 复核走异步不阻塞注入链路（§4.3） |
| 领域识别准确率 ≥95% | 标注集上 `DecideCategory` 一致率（含"不切换"标签），`context_bench` 评测（§6.2） |
| 内容适配相关性 ≥85 分 | 领域化精修 prompt 的抽样人工评审量表（§6.3） |
| ≥20 核心领域 | `DefaultRules()` 扩展至 20 个内置领域规则表（§4.3） |
| 多级深度适配 | 文本复杂度启发式分三档，选择精修 prompt 模板与热词段来源（§4.4） |
| 质量控制体系 | 现有守卫 + 领域切换审计日志 + 标注评测回归（§4.5） |
| 热词碟聚合展示 | 跨分类按活跃度 Top-N 动态计算，设置页只读 chips 块（§4.1/§4.2） |
| 碟优先匹配 | corpus 预算分配碟词在前 + 精修热词段碟词在前（§4.1.3） |
| 动态更新 | 每条 ASR 最终文本落账后增量重算（§4.1.4） |

**边界声明**：VoiceStick 是语音输入设备，"内容适配"限于 ASR/精修链路的领域适配（术语保护、领域语境纠错、领域规范），**不新增内容生成式助手能力，不触碰文本注入路径**——与现有红线一致（自动切换只改热词分类与精修 prompt 构建参数）。

---

## 2. 总体架构

```text
ASR 最终文本
    │
    ├─► RecordHotwordUsageFromText        （现有：全分类词 + 回收池词落账）
    │        │
    │        └─► 热词碟重算（内存缓存 + 版本号）   ← 新增：跨分类 Top-N 活跃词
    │
    ├─► MaybeSmartHotwordUpdate           （现有：领域判定 + 自动切换 + 回收 + 逐出）
    │        │
    │        ├─► [现有] HotwordContextClassifier::DecideCategory（本地快路径，<1ms）
    │        └─► [新增] 临界文本 → LLM 领域复核（异步慢路径，结果只作下次判定输入）
    │
    ├─► RankedHotwordsForAsr              （改造：碟词在前 + 活跃分类词补足 → 预算裁剪）
    │
    └─► LLM 精修                          （改造：按深度档位选 prompt 模板 + 热词段碟优先）
```

线程与锁序沿用现有约定：`smart_hotword_mutex_` → `hotword_usage_mutex_`；碟缓存归 usage 锁管辖；UI 取快照模式与 `TakeHotwordActivitySnapshot` 一致。

---

## 3. 热词碟

### 3.1 数据层

**定义**：碟（Tray）= 全部分类词 ∪ 回收池词 中按 `HotwordScore` 降序排列的前 N 个词（N 默认 12），**动态计算、不落盘**——它是活跃度统计的一个投影视图，不是独立数据源。这保证单一事实源（`hotword_usage.json` + 分类词表），碟随统计天然动态更新。

新增纯函数（放 `hotword_selector.h/.cc`，可单测）：

```cpp
struct HotwordTrayEntry {
    std::string word;
    std::string category;   // 来源分类（回收池词显示"回收池"）
    double score = 0.0;
    int count = 0;
    bool in_active_category = false;  // 是否同时属于当前活跃分类
};

// 跨分类聚合 + 按活跃度降序取前 max_size 个；min_score 过滤冷淡词；
// 同分按 (count 降序, 字典序) 保证确定性。
std::vector<HotwordTrayEntry> BuildHotwordTray(
    const HotwordUsageStore& store,
    const std::vector<HotwordCategory>& categories,
    const std::string& active_category,
    const EvictedPool& evicted,
    int max_size,
    double min_score,
    std::int64_t now_s);
```

**活跃度标准（明确口径，UI 中展示）**：

- 评分公式即现有 `HotwordScore`：`1.0 × log1p(count) + 0.5 × exp(-距上次使用/30天) + 2.0 × manual`；
- 入碟门槛：`score ≥ hotword_tray_min_score`（默认 0.5，即 30 天内至少用过一次，或 manual 词近乎必入）；
- 展示档位：≥3.0 高活跃、1.0–3.0 活跃、<1.0 观察（UI 用三档颜色/星标区分，避免裸数字不直观）。

协调器侧：

- `EnsureHotwordTrayLocked()`：usage 落账后置 `tray_dirty_ = true`；取用时惰性重算（O(全词数·log N)，词量千级 <1ms）；
- `TakeHotwordTraySnapshot()`：供设置页 UI，锁内拷贝，模式同 `TakeHotwordActivitySnapshot`；
- 回调 `on_hotword_tray_changed`（可选）：碟内容变化时通知外壳刷新已打开的设置页。

### 3.2 碟优先匹配的落地方式

ASR 直传 corpus 是**偏置注入**而非逐词"检索"，因此"碟优先 → 分类兜底"的语义映射为预算分配顺序：

1. **corpus 组装改造**（`RankedHotwordsForAsr`）：
   - 现状：`RankHotwords(hotword_usage_, config_.asr_hotwords)`（仅活跃分类）→ `FitHotwordsToCorpusBudget` 保序裁剪；
   - 改为：碟词（跨分类，按活跃度降序）在前 + 活跃分类中未入碟的词（保持活跃度序）在后 → 拼接后仍走现有 `FitHotwordsToCorpusBudget`（保序逐个装入，超 80 token 预算丢弃尾部）。
   - 效果：预算紧张时，高频词不因"不在当前活跃分类"而落选——这正是"碟未命中再检索各分类"的预算等价物：碟词必占预算，剩余预算给分类词。
2. **精修热词段**（`HotwordsForLlmPrompts`）：同规则重排后再 `TrimHotwordsForPrompt`（top-50），碟词在前。
3. **不变项**：火山二遍覆盖热词丢失的 LLM 兜底纠正、`RefineResultKeepsHotwords` 守卫、腾讯 vocab 提交路径均不感知碟的存在，只消费重排后的列表。

### 3.3 UI 层（设置页语音识别模块下方）

位置：`settings_dialog.cc` 语音识别区内、候选热词块之后、区块分隔线之前（即现有热词块的下方延续），符合"语音识别功能模块下方"。

组件形态：**只读 chips 流式面板**（非树/表格，保持轻量）：

```text
热词碟                                      [刷新]
按活跃度自动聚合各分类热词，识别时优先匹配（n/12）
┌────────────────────────────────────────────┐
│ [GLM ★★★] [ESP-IDF ★★☆] [鲁迅 ★★★] [流式★★☆] …│
└────────────────────────────────────────────┘
提示：碟内容随使用频率自动更新，可在"活跃度…"中查看明细
```

实现要点：

- 面板用只读多行 RICHEDIT 或自绘静态控件行内 chips（推荐后者：`WS_EX_TRANSPARENT` 静态控件 + 按词着色，与现有 `CreateLabel` 风格一致，避免引入新控件类型）；
- 每个词 chip：`词名 + 活跃度档位图标（★★★/★★☆/★☆☆）`，悬停 tooltip 显示 `来源分类 · count · 分值`；
- 数据：对话框打开时与"刷新"按钮点击时经 `on_get_hotword_tray` 回调取快照；`on_hotword_tray_changed` 触发已打开对话框的轻量重绘（防抖 500ms）；
- 布局走现有 `LayoutEntry` 声明式机制，`hotword_tray_enabled=false` 时整块隐藏不占位（同候选词块模式）；
- 本地化：`localization.h/.cc` 新增 StringId（`kSettingsHotwordTray`、`kSettingsHotwordTrayHint`、`kSettingsHotwordTrayRefresh` 等），zh-CN/en-US 同步；
- 与"活跃度…"对话框的关系：碟是摘要视图，活跃度对话框是全量明细，入口并存，碟提示行引导跳转。

---

## 4. 领域上下文推理升级

### 4.1 领域规则表扩展（4 → 20+）

- `HotwordContextClassifier::DefaultRules()` 扩展至 20 个内置领域（现有 4 个保留 + 新增 16 个，示例：英语、历史、地理、物理、化学、生物、医学、法律、金融财经、音乐、美术设计、体育、烹饪生活、游戏电竞、汽车交通、航空航天）。每个领域 8–15 个关键词，权重 1.0（强词 1.5）；
- **数据与引擎分离**：规则表移入独立数据文件（`resources/hotword_domains.toml` 随资源打包，`HotwordContextClassifier` 支持从数据构造），关键词可热迭代不改引擎；
- **用户自定义分类不参与自动切换**（现状约束保留）；提供内置领域 → 用户分类的可选映射（`config.toml` 中 `[hotword_domain_map]`，如 `语文 = "我的文学词库"`），判定命中内置领域后切换到映射的分类，未映射则不切换；
- 防抖参数（阈值/最少命中/连续一致/冷却/滞后）全部沿用现有值，不因领域数增加而调整——判定是"每分类独立得分取最高"，复杂度仍是 O(分类数×关键词数×文本长度)，20 领域 × 15 词 × 500 字仍在 1ms 量级。

### 4.2 LLM 领域复核慢路径（可选，默认关）

- 触发条件（三者同时）：智能热词开启、连续 2 次判定"无切换"但最高得分落在阈值 0.6–1.0 倍区间（临界文本）、距上次复核 >60s；
- 实现：后台线程调 `LLMChatClient`，prompt 只输出领域名（从 20 领域 + "无"中选），**不传原文全文只传文本**（与精修同隐私级别），结果写入 `ctx_classifier_` 的外部提示缓存，作为下一次 `DecideCategory` 的加成输入（+0.5，可被硬条件否决）；
- 失败静默，不影响主链路时序。此项是 95% 准确率指标在边界文本上的兜底手段，默认关闭，A/B 验证后决定是否默认开。

### 4.3 多级内容深度适配

**深度档位判定**（纯函数，可单测，放 `hotword_context.h`）：

```cpp
enum class ContentDepth { kLight, kStandard, kProfessional };
// 复杂度启发式：文本长度 + 领域词密度（领域关键词命中数/长度）→ 三档
ContentDepth ClassifyContentDepth(const std::string& text,
                                  const HotwordContextScore& score);
```

- `kLight`：≤30 字或零领域命中 → 默认精修 prompt（现状不变）；
- `kStandard`：31–120 字且有领域命中 → 精修 prompt 追加一行领域语境提示（如"文本属于语文学科语境，人名/篇名/术语按规范写法保留"）；
- `kProfessional`：>120 字或领域词密度高 → 领域语境提示 + 术语保护清单（该领域碟内词 top-10 注入热词段首位）。

**接入点**：`LLMRefinementClient::BuildRefinePrompt` 增加可选参数 `domain_hint`/`depth`，协调器在发起精修前计算档位；`RefineResultKeepsHotwords` 守卫范围自动覆盖碟词（它接收的就是重排后的热词表）。

**模板管理**：三档 prompt 模板与 20 领域语境提示语一并放 `resources/hotword_domains.toml`，与规则表同文件管理。

### 4.4 质量控制体系

| 环节 | 机制 | 状态 |
|---|---|---|
| 精修不丢热词 | `RefineResultKeepsHotwords` 守卫，违规则回退原文 | 已有，碟词自动纳入 |
| 提炼防臆造 | `ParseHotwordExtractionResponse` 候选必须在原文出现 | 已有 |
| 领域切换可审计 | 审计日志只记 `旧分类 → 新分类、得分、命中词数`，**不落文本**（沿用现有 `auto-switched to category` 日志风格） | 已有，补记得分字段 |
| 领域判定回归 | `context_bench` 标注评测集纳入 CTest 可选目标（§6.2） | 新增 |
| 内容相关性评审 | 量表抽样评审（§6.3） | 新增 |
| 碟变化可观测 | 碟成员增删打 debug 日志（词名+分值） | 新增 |

---

## 5. 配置与兼容性

`config.toml` 新增字段（`app_config.h/.cc` + `config.template.toml` + `Doc/Ref/desktop-config.md` 同步）：

```toml
[hotword_tray]
enabled = true        # 碟总开关（关闭 = 现状行为）
size = 12             # 碟容量（6..24）
min_score = 0.5       # 入碟活跃度门槛
```

- 协议、固件、网站零改动；火山 corpus / 腾讯 vocab 消费方式不变；
- macOS 端无智能热词体系，本方案不涉及；
- 旧配置无 `[hotword_tray]` 段时取默认值，无迁移成本；
- 碟开启改变 corpus 组装顺序，属行为变更：先经 `scripts/e2e_test/asr_bench` 离线回归确认无 CER 回退（先例：`Doc/Expe/volc-hotword-ablation-2026-08-01.md`），再默认开启。

---

## 6. 指标度量与验收

### 6.1 响应时间 ≤300ms

- 本地判定路径（文本落账 → 领域判定 + 碟重算）：单测计时断言 500 字文本 <5ms（含 20 领域），实测预期 <1ms；
- LLM 复核慢路径为异步后台，不占用注入链路时序；精修 prompt 构建为字符串拼接，可忽略；
- 验收：`core_tests.cc` 新增计时断言 + 协调器日志打点（判定耗时 µs 级）。

### 6.2 领域识别准确率 ≥95%

- 构建标注集：20 领域 × 每域 30 条正样本（短/中/长各 10）+ 100 条跨域干扰样本 + 100 条无域样本（标签"不切换"），共 800 条；
- 评测口径：`DecideCategory` 输出分类（或"不切换"）与标注一致的比例；边界文本允许"不切换"与标注域二选一算对（防抖本意即保守）；
- 承载：`scripts/e2e_test/asr_bench/context_bench.py`（复刻评分逻辑离线跑，规则表数据文件双端消费）+ C++ 侧核心样本单测锚定；
- 达标路径：先看纯规则引擎基线，不足 95% 时迭代关键词表，仍不足再开 LLM 复核慢路径（§4.2）。

### 6.3 内容适配相关性 ≥85 分

- 评审量表（0–100）：术语正确性 40 / 领域风格规范 30 / 信息保全（原文语义无损）20 / 无幻觉 10；
- 抽样：真实语料 30 条（3 领域 × 10 条），开启领域适配精修 vs 现状精修双盲对比；
- 达标线：领域适配组均分 ≥85 且不低于对照组；由开发者双人评审取均值，评审记录归档 `Doc/Expe/`。

### 6.4 功能验收清单

- [ ] 碟面板展示跨分类 Top-N 词与活跃度档位，刷新即时生效；
- [ ] 使用某分类词后碟排序变化（动态更新可演示）；
- [ ] 预算紧张场景下碟词不落选（asr_bench 断言）；
- [ ] 20 领域标注集一致率 ≥95%（或记录基线与差距）；
- [ ] `build_win.bat` + `ctest --test-dir desktop\windows\build-x64 --output-on-failure` 全绿。

---

## 7. 实施计划

| 阶段 | 内容 | 交付物 | 验证 |
|---|---|---|---|
| P1 碟数据层 | `BuildHotwordTray` 纯函数 + 协调器缓存/快照 + corpus 组装重排 + 单测 | `hotword_activity.{h,cc}`（实施偏差①）、协调器、`core_tests.cc` | 构建 + CTest + asr_bench 回归 |
| P2 碟 UI | 设置页热词碟块 + 刷新/快照回调 + 本地化 + config 字段 | `settings_dialog.*`、`localization.*`、`app_config.*`、`config.template.toml` | 构建 + 手动验收 |
| P3 领域 20+ | 规则表数据化 + 20 领域关键词 + 标注集一致性单测（实施偏差③：CTest 锚点而非 Python context_bench） | `hotword_context.*`、`core_tests.cc` | CTest 标注集 ≥95% 基线 |
| P4 深度适配 | 档位判定 + 精修 prompt 分档 + 评审 | `hotword_context.h`、`llm_refinement_client.*` | 单测 + 双盲评审 ≥85 |
| P5 收尾 | desktop-config.md / AGENTS.md 索引同步，Expe 沉淀 | 文档 | — |

依赖关系：P1 → P2；P3 独立可并行；P4 依赖 P1（碟词注入热词段）。跑 Windows 单测前先结束运行中的 VoiceStick.exe（既有教训，时序断言并发漂移）。

---

## 8. 风险与开放问题

**风险（通俗版）**：

1. **碟词太杂可能互相"抢戏"**：热词碟会把不同领域的词混在一起发给 ASR，领域跨度大时可能互相稀释、反而影响识别。对策：碟只装 12 个高分词 + 高分门槛；上线前先跑离线回归对比，效果变差就默认关闭（有独立开关，随时可退回现状）。
2. **95% 准确率不保证一次达标**：有些话本身就说不清属于哪个领域（一句话既提编程又提做菜），纯关键词规则在边界文本上可能达不到 95%。对策：评测口径允许"不切换"算对；规则表迭代后仍不足，再开 LLM 复核兜底。
3. **性能当前无忧，词量暴涨才需优化**：碟的重算与热词统计共用一把锁，当前词量（千级）重算 <1ms，无感；若未来热词涨到上万才需要改增量算法。现在不做，避免过度设计。
4. **设置窗口变高一点**：新增碟面板约占 6 行高度。项目已有窗口高度自适应机制，直接复用，只需测试滚动边界不裁字。

**修复方案（与风险一一对应）**：

1. **防"抢戏"（三道闸）**：① 体量控制——碟默认 12 词 + 活跃度门槛；② 领域内聚——`BuildHotwordTray` 加约束：碟内词来源领域最多 3 个，超出者即使分高也不入碟；③ 先测再开——上线前置 asr_bench 离线 A/B（碟开 vs 关比 CER），回退超阈值则默认关闭（`[hotword_tray].enabled` 独立开关随时退回现状）。
2. **保 95%（三步递进）**：① context_bench 混淆矩阵定位易混领域对（如历史↔语文），定向补关键词、调权重迭代到收敛；② 口径校准——允许"不切换"算对、剔除真二义样本；③ 仅临界文本（得分落在阈值 0.6–1.0 倍区间）触发 LLM 异步复核兜底（60s 冷却控成本）。迭代后仍差 2–3 点时，拿实测数据与需求方重新约定可达口径，不硬凑数字。
3. **保性能（先观测后优化）**：碟重算加耗时打点，>10ms 记 warning；真到万级词量再改增量堆维护（落账只更新受影响词，O(log N)），现在明确不做，避免过度设计。
4. **保布局（纯复用）**：直接复用现有动态高度机制；碟关闭时整块不占位、窗口高度自动回落；验收覆盖 96/144/192 DPI 三档滚动到底不裁字、间距正常。

**开放问题（已决议 2026-10-07）**：

1. macOS 端**不同步**建设智能热词体系，本方案以 Windows 为唯一实施范围；
2. 碟 chip **初版只读**，无点击交互，留待用户反馈再评估；
3. 20 领域清单与关键词表按真实语料分布校准，**标注集初版由开发者标注**（已确认）；
4. 领域语境提示语（精修 prompt 段）文案**逐领域人工评审**（已确认）。
