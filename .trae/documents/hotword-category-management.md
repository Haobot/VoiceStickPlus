# 热词库分区管理（分类下拉 + 每分类 25 词上限）实施方案

## Context

热词现为单一列表（`config.toml` 顶层 `asr_hotwords` 字符串，`AppConfig::asr_hotwords` 向量）。火山 ASR corpus 热词直传有服务端预算 `kHotwordCorpusTokenBudget = 80` token（约 25 个短词，[asr_protocol.h:89-94](e:\coding\VoiceStickPlus\desktop\windows\src\asr_protocol.h#L89-L94)），超出的词被 `FitHotwordsToCorpusBudget` 静默裁剪——这是用户感知到的"25 字上限"，客户端无法突破服务端预算。

**方案**：热词库按分类分区（默认/语文/数学/程序/AI 首次预置，可自定义增删）。**当前选中的"活跃分类"决定 ASR 识别用的热词**（仅当前选中分类，用户已确认）——切分类即切换识别场景，每分类 ≤25 词基本落在 80 token 预算内完整生效，总容量 = 分类数 × 25，实现扩容。

**核心兼容设计**：`hotword_categories` 为分区数据的事实源；`asr_hotwords` 保持为"活跃分类热词"的投影（继续写盘），所有下游消费方（ASR corpus 直传、腾讯 session、LLM 精修 prompt、协调器排名/统计/裁剪）**零改动**自动生效；老版本 exe 读新配置也只认 `asr_hotwords`，无损回滚。

## 改动文件

### 1. `desktop/windows/src/app_config.h` — 数据模型

- 新增结构体与常量：
  ```cpp
  struct HotwordCategory {
      std::string name;                // 空字符串 = "默认"分类（UI 本地化显示，避免硬编码中文）
      std::vector<std::string> words;  // 上限 25
  };
  static constexpr std::size_t kMaxHotwordsPerCategory = 25;  // 放 HotwordCategory 内
  ```
- `AppConfig` 新增字段（[app_config.h:240](e:\coding\VoiceStickPlus\desktop\windows\src\app_config.h#L240) 旁）：
  - `std::vector<HotwordCategory> hotword_categories;`
  - `std::string active_hotword_category;`
- 新增辅助函数（与 `ParseHotwordList` 同处，[app_config.h:425](e:\coding\VoiceStickPlus\desktop\windows\src\app_config.h#L425) 旁）：
  - `std::vector<HotwordCategory> DefaultHotwordCategories();` — 返回 {""默认, "语文", "数学", "程序", "AI"} 五个空分类
  - `std::string EncodeHotwordCategories(const std::vector<HotwordCategory>&);` / `std::vector<HotwordCategory> ParseHotwordCategories(std::string_view json);` — cJSON 编解码（项目已广泛使用，如 `asr_protocol.cc`）
  - `HotwordCategory* FindHotwordCategory(AppConfig&, const std::string& name);`
  - `void SetActiveHotwordCategory(AppConfig&, const std::string& name);` — 切活跃分类并同步 `asr_hotwords = 活跃分类 words`
  - `bool AddHotwordToActiveCategory(AppConfig&, const std::string& word);` — 活跃分类内去重 + 25 上限 + 同步 `asr_hotwords`；返回 false=重复或已满（调用方按需区分：先查重再调用）

### 2. `desktop/windows/src/app_config.cc` — 解析/迁移/序列化

- **解析**（两条路径都要加：[app_config.cc:393](e:\coding\VoiceStickPlus\desktop\windows\src\app_config.cc#L393) 的 ParseConfigLine 与 [app_config.cc:629](e:\coding\VoiceStickPlus\desktop\windows\src\app_config.cc#L629) 的 TOML table 路径）：
  ```cpp
  if (key == "hotword_categories") config.hotword_categories = ParseHotwordCategories(value);
  if (key == "active_hotword_category") config.active_hotword_category = value;
  ```
- **Load 后对齐**（Load 成功返回前）：
  - `hotword_categories` 为空 → `DefaultHotwordCategories()`；旧 `asr_hotwords` 非空 → 迁移进默认分类（name=""）
  - `active_hotword_category` 找不到对应分类 → 重置为第一个分类（默认分类，name=""）
  - `asr_hotwords = 活跃分类 words`（强制对齐投影）
- **序列化**（[app_config.cc:923-928](e:\coding\VoiceStickPlus\desktop\windows\src\app_config.cc#L923-L928) 旁）：
  - `asr_hotwords = "..."` 照旧写（= 活跃分类词，向后/向前兼容）
  - 新增 `hotword_categories = "<JSON>"`（`[{"name":"语文","words":"词1,词2"},...]`，TomlEscape 转义）
  - 新增 `active_hotword_category = "..."`

### 3. `desktop/windows/src/settings_dialog.h` — UI 成员

- 新控件成员：`hotword_category_combo_`、`hotword_category_add_button_`、`hotword_category_delete_button_`
- 新状态：`std::string loaded_hotword_category_;`（编辑框当前展示的分类名）
- 新方法：`OnHotwordCategoryChanged()` / `OnHotwordCategoryAdd()` / `OnHotwordCategoryDelete()` / `RefreshHotwordCategoryCombo()` / `StoreCurrentCategoryWords()`（编辑框 → 当前分类，含 25 截断）
- 新控件 ID：`kIdHotwordCategoryCombo = 2042`、`kIdHotwordCategoryAdd = 2043`、`kIdHotwordCategoryDelete = 2044`（接续 [settings_dialog.h:171](e:\coding\VoiceStickPlus\desktop\windows\src\settings_dialog.h#L171)）

### 4. `desktop/windows/src/settings_dialog.cc` — UI 逻辑

- **BuildControls 热词块**（[settings_dialog.cc:638-651](e:\coding\VoiceStickPlus\desktop\windows\src\settings_dialog.cc#L638-L651)）：块内最上方插入分类行（复用 `CreateCombo`/`CreateButton` helper）：
  - 布局：`分类label + combo(约 ctrl_w-2×70-2×8) + [新增分类(70)] + [删除分类(70)]`，下行接原 label/编辑框/hint；块 `advance` 相应 +1 行
- **LoadConfigIntoControls**：`RefreshHotwordCategoryCombo()`（下拉项显示 `名称 (n/25)`，默认分类显示本地化"默认"）+ 选中 `active_hotword_category` + `loaded_hotword_category_` 初始化 + 编辑框填活跃分类词
- **HandleMessage**（[settings_dialog.cc:216-284](e:\coding\VoiceStickPlus\desktop\windows\src\settings_dialog.cc#L216-L284) 模式）：
  - `kIdHotwordCategoryCombo` + `CBN_SELCHANGE` → `OnHotwordCategoryChanged()`
  - `kIdHotwordCategoryAdd` / `kIdHotwordCategoryDelete` + `BN_CLICKED` → 对应处理
- **OnHotwordCategoryChanged**：`StoreCurrentCategoryWords()` 存回旧分类 → 加载新分类词到编辑框 → 更新 `loaded_hotword_category_` → 刷新计数
- **OnHotwordCategoryAdd**：新增辅助 `PromptForText(hwnd, title, prompt)`（匿名 namespace，内存 DLGTEMPLATE 模态输入框，参照 `BuildDialogTemplate` 先例，EDIT + OK/Cancel 约 70 行）；校验：Trim 非空、≤30 字符、不与现有分类重名 → 插入空分类并选中之（仅内存，随"保存"落盘，取消丢弃，符合对话框语义）
- **OnHotwordCategoryDelete**：确认框；最后一个分类不可删；删的是活跃分类 → 切到剩余第一个；同步 `asr_hotwords`
- **SaveSettings**（[settings_dialog.cc:1240](e:\coding\VoiceStickPlus\desktop\windows\src\settings_dialog.cc#L1240)）：`StoreCurrentCategoryWords()`（>25 截断 + MessageBox 提示被截断）→ `config_.active_hotword_category = loaded_hotword_category_` → `config_.asr_hotwords = 活跃分类 words`
- **候选热词块**（`OnHotwordCandidateAdd`）不动：候选写入编辑框即活跃分类视图，语义自动成立

### 5. `desktop/windows/src/localization.h` / `.cc` — 文案（中英两表同步）

新增 StringId（建议）：`kSettingsHotwordCategory`（分类）、`kSettingsHotwordCategoryDefault`（默认/Default）、`kSettingsHotwordCategoryAddButton`（新增分类）、`kSettingsHotwordCategoryDeleteButton`（删除分类）、`kSettingsHotwordCategoryAddTitle`、`kSettingsHotwordCategoryNamePrompt`、`kSettingsHotwordCategoryEmptyName`、`kSettingsHotwordCategoryDuplicate`、`kSettingsHotwordCategoryNameTooLong`、`kSettingsHotwordCategoryDeleteConfirm`、`kSettingsHotwordCategoryLastOne`、`kSettingsHotwordCategoryTrimmed`（超出 25 已截断提示）、`kSelectionHotwordCategoryFullTitle/Body`（划词添加时活跃分类已满 25）

更新 `kSettingsHotwordsHint`：说明逗号/换行分隔 + 每分类上限 25 + 仅当前选中分类参与识别。

### 6. `desktop/windows/src/win32_app.cc` — 运行时写路径（统一走辅助函数）

- **划词添加**（[win32_app.cc:1295-1302](e:\coding\VoiceStickPlus\desktop\windows\src\win32_app.cc#L1295-L1302)）：改为先查重（重复提示不变）→ `AddHotwordToActiveCategory` 失败即已满 25 → 新文案通知
- **纠错保存**（[win32_app.cc:1336-1340](e:\coding\VoiceStickPlus\desktop\windows\src\win32_app.cc#L1336-L1340)）：`AddHotwordToActiveCategory`（满 25 静默跳过，保持 best-effort 风格）
- **LLM 提炼**（[win32_app.cc:1956-1957](e:\coding\VoiceStickPlus\desktop\windows\src\win32_app.cc#L1956-L1957)）：逐词 `AddHotwordToActiveCategory`，满 25 停止；`DiffNewHotwords` 基于活跃分类去重（`asr_hotwords` 即活跃分类，语义自动成立）

### 7. `desktop/windows/tests/core_tests.cc` — 单测

新增 `TestHotwordCategories`（assert 风格，参照 [core_tests.cc:1132](e:\coding\VoiceStickPlus\desktop\windows\tests\core_tests.cc#L1132) `TestAsrHotwordCorpusBudget` 并注册）：
- `Encode/ParseHotwordCategories` 往返（含中文分类名、空默认分类、特殊字符）
- `AddHotwordToActiveCategory` 去重 / 第 25 词成功第 26 词失败 / `asr_hotwords` 同步
- `SetActiveHotwordCategory` 切换后 `asr_hotwords` 跟随
- `DefaultHotwordCategories()` 五分类

### 8. `Doc/Ref/desktop-config.md` — 文档

新增 `hotword_categories`（JSON 格式说明）、`active_hotword_category` 字段说明；`asr_hotwords` 补充语义（= 活跃分类热词投影，旧版兼容字段）。

## 不改动

- ASR 消费链路（`asr_protocol.cc` / `asr_client_*.cc` / `llm_refinement_client.cc` / `voice_stick_coordinator.cc`）：`asr_hotwords` 语义不变，零改动
- macOS 端、固件、网站（Windows-only 功能，参照小米遥控器先例）
- 托盘快捷切换分类（本次不做，界面下拉已满足需求）

## 边界与风险

- 服务端 80 token 预算仍在：单分类 25 个**长词**仍可能被裁剪（日志与"热词预算裁剪"通知已有），属服务端限制
- 双写冗余（`hotword_categories` ↔ `asr_hotwords`）：Load 强制对齐 + 所有写点经辅助函数同步，事实源唯一
- 分类名存 UTF-8 原文，JSON 编码无字符限制；默认分类以空名 `""` 存储、UI 本地化显示

## 验证

1. 构建：根目录 `build_win.bat`（按 build-windows skill）
2. 测试：`ctest --test-dir desktop\windows\build-x64 --output-on-failure`（含新 `TestHotwordCategories`）
3. 按 AGENTS.md 自动重启 VoiceStick.exe，手动验收：
   - 设置 → 热词区出现分类下拉（默认/语文/数学/程序/AI，含 `(n/25)` 计数）；旧热词已迁移到"默认"
   - 切换分类，编辑框内容独立切换；新增/删除分类；最后一个分类不可删
   - 输入 26 个词保存 → 截断提示，仅存 25
   - 划词添加 → 进入当前活跃分类；分类满 25 → 满额提示
   - 保存后检查 `%APPDATA%\VoiceStick\config.toml`：`hotword_categories` JSON 与 `active_hotword_category` 正确
   - 重启应用 → 活跃分类与各分类热词保持
   - 切换到"语文"分类后语音识别，日志确认 corpus 只携带该分类热词
