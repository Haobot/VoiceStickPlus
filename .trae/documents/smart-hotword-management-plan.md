# 智能热词库管理功能实现计划

## Context（背景与目标）

现有热词分区管理（设置-热词区的分类下拉手动切换，`HotwordCategory` 每分类上限 25 词）基础上，开发智能热词库管理：

1. **上下文识别自动切换**：关键词加权规则引擎分析最终识别文本，检测到程序等主题内容时自动切换活跃热词分类（满足 ≤300ms，不用 LLM）。
2. **容量动态管理**：软限 5 词（超出即启动"监控"），硬上限沿用 25 词。
3. **活跃度评估与动态调整**：复用 `hotword_usage.json` 统计，活跃度 = HotwordScore（次数×30 天时间衰减+manual 加成）；达 25 上限时逐出最低活跃度词（manual 来源最后逐出）；被逐出词进回收池，再次出现时自动回归（活跃度回升→重新加入）。
4. **可视化**：设置-热词区新增"热词活跃度"按钮 → 独立对话框（ListView 报表）。
5. **已确认决策**：语义分析=关键词加权规则；切换反馈=悬浮窗临时消息（ShowTimedMessage，会话活跃时实现内部自动回退托盘气泡）；`smart_hotword_enabled` 默认开启。

**"程序内容不误识别为语音命令"的解释**：本产品无语音命令子系统；保障 = 自动切换只改热词分类、绝不触碰文本纠错/注入路径，且分类器需 ≥3 个不同关键词命中 + 连续 2 次决策 + 冷却期才切换，偶发技术词不触发。

**关键代码事实**：
- `TransformText`（voice_stick_coordinator.cc:2201）是最终文本统一漏斗，运行在 ASR 后台线程；`RecordHotwordUsageFromText`（:2412）目前只统计活跃分类词。
- `UpdateConfig`（:199）会取消活跃会话——自动切换**不得**经外壳 UpdateConfig 回流，协调器直接改 `config_` + `SavePreservingDiskCredentials()`，经新回调 `on_config_mutated` 同步外壳副本。
- 回收池用独立 `hotword_evicted.json`（hotword_usage.json 数组格式是与 scripts/e2e_test/asr_bench/hotword_select.py 的兼容契约，不污染）。
- `Win32App::ShowTimedMessage`（win32_app.cc:2636）已内置 DispatchToUi 封送 + 会话活跃回退托盘气泡。
- 切换时序天然安全：热词在会话 Start 时经 `RankedHotwordsForAsr`（:2359）取用，会话收尾切换、下一会话生效。

## 一、新建文件（3 组）

### 1. `src/hotword_context.h/cc`（进 voicestick_core）— 上下文分类器
```cpp
// 常量
inline constexpr int kHotwordCtxSoftLimit = 5;           // 软限：超此词数进入监控
inline constexpr int kHotwordCtxMinHits = 3;             // 防误触发最少命中词数
inline constexpr double kHotwordCtxScoreThreshold = 2.5; // 切换得分阈值
inline constexpr int kHotwordCtxConsecutive = 2;         // 连续 N 次决策才切换
inline constexpr int kHotwordCtxCooldownS = 300;         // 切换冷却 5 分钟
inline constexpr double kHotwordCtxHysteresisBonus = 1.0;// 当前分类滞后加成

struct HotwordContextRule { std::string category; std::vector<std::string> keywords; double weight; };
class HotwordContextClassifier {
public:
    explicit HotwordContextClassifier(std::vector<HotwordContextRule> rules);
    std::map<std::string, double> Score(const std::string& text) const; // 子串匹配×权重求和
    // 状态机：全部满足才切换——①目标分类得分≥阈值且为最高分（当前分类有滞后加成）；
    // ②命中词数≥3；③当前分类热词未出现在文本；④连续 2 次决策指向同一分类；
    // ⑤距上次切换>300s；⑥目标分类存在且非当前。任一不满足清零 pending。
    std::optional<std::string> DecideCategory(const std::string& text,
                                              const std::string& current_category,
                                              const std::vector<std::string>& current_words,
                                              std::int64_t now_s);
    static std::vector<HotwordContextRule> DefaultRules(); // 程序/AI/数学/语文关键词表
private:
    std::int64_t last_switch_ts_ = 0;
    std::string pending_category_;
    int pending_count_ = 0;
};
```
纯决策模块，不碰 config_（可独立单测）。

### 2. `src/hotword_activity.h/cc`（进 voicestick_core）— 活跃度/逐出/回收池
```cpp
inline constexpr double kHotwordActivityColdThreshold = 0.3; // 冷淡判定线
inline constexpr std::int64_t kHotwordActivityTauS = 30LL * 24 * 3600;

struct EvictedWord { std::string word; std::string category; std::int64_t evicted_ts; HotwordUsage usage; };
using EvictedPool = std::vector<EvictedWord>;

// 活跃度直接复用 hotword_selector.h 的 HotwordScore(usage, now)（1.0*log1p(count)+0.5*exp(-age/30d)+2.0*manual）
bool IsCold(const HotwordUsage& usage, std::int64_t now_s);
// 逐出候选：非 manual 词中最低分；全 manual 时降级逐出最低分 manual
std::optional<std::string> PickEvictionWord(const std::vector<std::string>& words,
                                            const HotwordUsageStore& store,
                                            bool protect_manual, std::int64_t now_s);
EvictedPool LoadEvictedPool(const std::filesystem::path& path);
void SaveEvictedPool(const std::filesystem::path& path, const EvictedPool& pool);
// 回收判定：池内词在最终文本中出现即视为活跃度回升
std::optional<std::string> PickReclaimWord(const EvictedPool& pool, const std::string& text);
```

### 3. `src/hotword_activity_dialog.h/cc`（进 VoiceStickApp target）— 可视化对话框
参照 pair_device_dialog.cc 的 ListView 模式（L473-488 创建 `WC_LISTVIEWW + LVS_REPORT|LVS_SINGLESEL|LVS_NOSORTHEADER`，L174-190 列插入/子项文本）：
- 列：热词 / 使用次数 / 活跃度 / 最近使用 / 状态（活跃|冷淡|已清理待恢复）
- 头部 STATIC：当前分类 · 词数 (n/25) · 监控状态（>5 词显示"监控中"）；分类 COMBO 可切换查看（只读视图）
- DPI：`Dp()` 缩放 + `WM_DPICHANGED` 重排；`CreateUiFont`/`DeleteObject`
- 数据来源：协调器新方法 `HotwordActivitySnapshot()` 返回 {AppConfig 拷贝, HotwordUsageStore 拷贝, EvictedPool 拷贝}（锁 `smart_hotword_mutex_`/`hotword_usage_mutex_` 取快照）
- 构造：`HotwordActivityDialog(HINSTANCE, HWND owner, snapshot, UiLanguage)`

## 二、修改文件

| 文件 | 改动 |
|---|---|
| `src/app_config.h` | L242 附近加 `bool smart_hotword_enabled = true;` |
| `src/app_config.cc` | 仿 `hotword_mining_enabled` 三处：TOML 行解析（~L389）、表读取（~L631）、Save 输出（~L935） |
| `src/voice_stick_coordinator.h` | 回调区加 `std::function<void(AppConfig)> on_config_mutated;`；成员 `HotwordContextClassifier ctx_classifier_`、`EvictedPool evicted_pool_`、`mutable std::mutex smart_hotword_mutex_`、`std::atomic<int64_t> last_evict_notify_ms_{0}`；方法 `MaybeSmartHotwordUpdate(const std::string&)`、`HotwordActivitySnapshot()` |
| `src/voice_stick_coordinator.cc` | ① `RecordHotwordUsageFromText`（:2412）统计范围扩为**全部分类词+回收池词**（跨分类监控与回收判定依赖）；② 新函数 `MaybeSmartHotwordUpdate`：`smart_hotword_enabled` 关则跳过 → 锁内：分类器 DecideCategory → 命中则 `SetActiveHotwordCategory(config_, target)` + `config_.SavePreservingDiskCredentials()` + `on_config_mutated(config_)` + `ui_->ShowTimedMessage("已切换到「程序」热词库")`；活跃分类达 25 词时 `PickEvictionWord` 逐出入池 + `SaveEvictedPool` + 节流托盘通知（>5min 一次）；回收词出现 → 分类未满直接回归、满则先逐冷再回归；分类词数 >5 置监控标记（仅呈现+日志）。③ `UpdateConfig` 中 `config_ = std::move(config)` 前后注意与 smart_hotword_mutex_ 的协同（赋值整体替换即可，锁内做） |
| `src/win32_app.cc` | 回调接线区（~L459-492）：`coordinator_->on_config_mutated = [this](AppConfig cfg){ DispatchToUi([this, cfg]{ config_ = std::move(cfg); }); };`——**绝不调 UpdateConfig/ApplyUpdatedConfig**（会取消活跃会话）；设置对话框 `on_open_hotword_activity` 接线创建 HotwordActivityDialog |
| `src/win32_app.h` | 成员 `std::unique_ptr<HotwordActivityDialog> hotword_activity_dialog_;` |
| `src/settings_dialog.h` | `kIdHotwordActivity = 2045; kIdSmartHotwordEnable = 2046;`；成员 `hotword_activity_button_`、`smart_hotword_check_`；回调 `std::function<void()> on_open_hotword_activity;` |
| `src/settings_dialog.cc` | 热词分类行（L768-799）加第 4 个按钮"活跃度"（压缩 cat_combo_w）；热词块后新增"智能热词管理"checkbox 行（仿 selection_hotword_check_ 的 LayoutEntry 模式）；命令分发 case；LoadConfigIntoControls/SaveSettings 读写 checkbox |
| `src/localization.h/cc` | 新增 ~12 个 StringId（开关/按钮/对话框列头/状态/通知文案），EnglishStrings 与中文表双份（LocalizationTablesAreComplete 测试自动覆盖） |
| `CMakeLists.txt` | voicestick_core（L82-123）加 `src/hotword_activity.cc src/hotword_context.cc`；VoiceStickApp（L134-163）加 `src/hotword_activity_dialog.cc` |
| `resources/config.template.toml` | L48 附近加 `smart_hotword_enabled = true` 示例 |
| `Doc/Ref/desktop-config.md` | L19-23 热词段补 `smart_hotword_enabled`、`hotword_evicted.json` sidecar、软限/上限/阈值常量说明 |
| `tests/core_tests.cc` | 见测试清单 |

## 三、性能指标落实

| 指标 | 落实方式 |
|---|---|
| 识别准确率 ≥90% | 单测标注语料 ≥40 条（程序/AI/数学/语文/日常各 ≥8，含混淆样本），断言一致率 ≥90% |
| 切换响应 ≤300ms | 纯关键词子串匹配（500 字×100 词 <1ms）+ 内存 config 变更；落盘在后台线程完成不阻塞 |
| 统计更新 ≤1s | `RecordHotwordUsageFromText` 同步完成（25 词×子串匹配 <1ms） |
| 资源 ≤10% | 无常驻线程、无轮询，全部挂在既有会话收尾路径 |

## 四、实施顺序

1. **app_config 字段** + TOML 读写 + 字段测试 → 构建
2. **hotword_context / hotword_activity 模块** + CMake + 单测 → 构建
3. **协调器集成**（统计扩展、MaybeSmartHotwordUpdate、on_config_mutated、快照方法）
4. **UI 层**（settings 开关/按钮、HotwordActivityDialog、本地化）
5. **文档同步**（desktop-config.md、config.template.toml）

## 五、验证

- 每步：根目录 `build_win.bat`（构建后自动跑 ctest；或手动 `ctest --test-dir desktop\windows\build-x64 --output-on-failure`）
- 单测清单（core_tests.cc，纯 assert 风格，注册于 main() TestHotwordCategories 后）：
  - `TestHotwordContextClassifier`：准确率语料 ≥90%；冷却期内不切；当前分类热词命中不切；连续性需 2 次；单次偶发词不切
  - `TestHotwordActivityAndEviction`：manual 保护（先逐 mined）、全 manual 降级、逐出入池、池词命中回归、满 25 先逐冷再回收、冷淡阈值边界
  - `TestEvictedPoolJson`：Load/Save 往返、损坏容错
  - `TestSmartHotwordConfig`：默认值、TOML 往返
- 构建通过后按仓库惯例：结束运行中的 VoiceStick.exe → 重启 → 查 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`
- 手动验收：说一段编程相关话术两轮 → 观察悬浮窗提示"已切换到「程序」热词库" + 下一轮识别生效；活跃度对话框数据正确；勾选/取消设置开关生效

## 六、边界与已知取舍

- 设置对话框打开期间若发生自动切换，用户点"保存"会用旧副本覆盖自动切换结果（既有staleness 模式，接受；关闭重开即恢复）
- 分类器只覆盖内置四分类（程序/AI/数学/语文），自定义分类不参与自动切换
- `RecordHotwordUsageFromText` 统计范围扩到全部分类后，评分数据更全，对现有 `RankedHotwordsForAsr` 行为无负向影响（仍只对活跃分类词排序）
