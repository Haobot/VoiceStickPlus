# 用户数据飞轮新功能手动测试计划（T1~T11）

> 创建：2026-10-16。被测代码：分支 `feat/hotword-usage-negative`（含飞轮全部 10 项新功能：纠错对资产化/全句精确命中/热词候选挖掘/会话历史/dialog_ctx/编辑追踪/App 分域/负信号事件流/拼音闸门/反馈环降权/评分负项）。
> 测试实例：dev 构建 `desktop/macos/.build/out/Products/Debug/VoiceStickApp`（**非** /Applications 的 2.4.10 正式版——正式版不含新功能，测试认准 dev 实例）。
> 方案出处：`Doc/Plan/doubaoime-user-data-flywheel.md`、`Doc/Plan/typeless-lessons-and-user-data-flywheel.md`、`Doc/Plan/user-data-flywheel-wetype-benchmark.md`。

## 环境与工具

数据目录（下文命令直接粘贴可用）：

```bash
D=~/Library/Application\ Support/VoiceStick
```

| 数据文件 | 内容 |
|---|---|
| `$D/session_history.db` | 会话历史（SQLite，每轮识别一行；edited_* 编辑追踪） |
| `$D/correction_pairs.json` | 纠错对资产（source=refine/edit、same_pronunciation、app 分桶、use/recall） |
| `$D/usage_events.jsonl` | 负信号事件流（确认/取消/重录/停留时长；只记长度不记文本） |
| `$D/hotword_usage.json` | 热词使用统计（count/negative_count/last_used_ts/source） |
| `$D/hotword_candidates.json` | 热词候选（counts/dismissed/notified） |
| `$D/VoiceStickApp.log` | 排障日志（8MB 轮转） |

日志关键 grep：

```bash
grep -E "correction pair applied|hotword candidates suggested|edit tracking (captured|finished)" $D/VoiceStickApp.log | tail -10
```

## 进度总表（测完打勾）

| 用例 | 内容 | 状态 | 备注 |
|---|---|---|---|
| T1 | 会话历史落库 | ✅ 已过 | 2026-10-16 三行真实数据，id/device/duration 均正常 |
| T2 | 编辑追踪 | 🔄 部分 | EXTRACTED/TIMEOUT 均已见；待验证 edited_text 非空路径 |
| T3 | 纠错对采集（精修 diff 源） | ⬜ | |
| T4 | 拼音闸门标注 | ⬜ | |
| T5 | App 场景分域 | ⬜ | 基线已有 dev.zcode.app / com.kimi.code.desktop 两桶真实数据 |
| T6 | 全句精确命中（零 LLM） | ⬜ | |
| T7 | use/recall 反馈环 + 降权 | ⬜ | |
| T8 | 热词候选挖掘 | ⬜ | |
| T9 | 负信号事件流 | ⬜ | |
| T10 | 热词评分负项 | ⬜ | |
| T11 | dialog_ctx（行为观察） | ⬜ | |

---

## T1 会话历史落库（每次识别必产）

- **操作**：任意语音输入一轮。
- **验证**：
  ```bash
  sqlite3 ~/Library/Application\ Support/VoiceStick/session_history.db \
    "SELECT id,device_id,asr_provider,mode,duration,edited_text_status FROM history ORDER BY created_at DESC LIMIT 3"
  ```
- **预期**：新行出现，device_id=设备号、provider=当前 ASR、edited_text_status 初始 NOT_EXTRACTED。`id` 是会话主键——每轮一行、多行同设备属正常。
- **结果记录**：

## T2 编辑追踪（typeless P0-2，重点）

- **操作**：语音输入一段可编辑文本；粘贴后 **48 秒内**在目标应用里修改其中几个字（建议在备忘录/VSCode 等 AX 友好应用中操作；粘贴目标无文本焦点会 TIMEOUT，属预期降级）。
- **验证**：
  ```bash
  sqlite3 ~/Library/Application\ Support/VoiceStick/session_history.db \
    "SELECT edited_text, edited_text_status, edited_text_attempts FROM history ORDER BY created_at DESC LIMIT 1"
  grep "edit tracking captured" ~/Library/Application\ Support/VoiceStick/VoiceStickApp.log | tail -2
  python3 -c "import json;d=json.load(open('$HOME/Library/Application Support/VoiceStick/correction_pairs.json'));print([p for p in d['pairs'] if p['source']=='edit'][-1])"
  ```
- **预期**：edited_text=改动区间（非全字段——隐私红线）；status=EXTRACTED；correction_pairs 新增 `source:"edit"` 的对。
- **结果记录**：

## T3 纠错对采集（精修 diff 源）

- **操作**：说同音错词让精修纠正，例：「这个**鱼器渍**是什么」（精修应纠为「语气词」）。
- **验证**：
  ```bash
  python3 -c "import json;d=json.load(open('$HOME/Library/Application Support/VoiceStick/correction_pairs.json'));print([(p['before'],p['after'],p.get('same_pronunciation')) for p in d['pairs']][-3:])"
  ```
- **预期**：`鱼器渍→语气词` 入表且 `same_pronunciation=true`。非同音对（语序调整等）也入表但标注 false——不丢弃只降权。
- **结果记录**：

## T4 拼音闸门标注

- **操作**：随 T3 顺带——再说「克劳德」被纠为「Claude」。
- **验证**：同 T3 命令，对比两对的 same_pronunciation。
- **预期**：CJK 同音对 = true；拉丁对照 = false。
- **结果记录**：

## T5 App 场景分域

- **操作**：在 IDE/编辑器前台与浏览器前台，各说一遍同样的同音错词句。
- **验证**：correction_pairs.json 出现**同 before/after、不同 app 字段**的两条各自累计。
- **预期**：对齐 doubao UNIQUE(src,dst,app_bucket)——同对在不同桶各自独立计数。（基线已有 dev.zcode.app 与 com.kimi.code.desktop 两桶真实数据。）
- **结果记录**：

## T6 全句精确命中（零 LLM 纠错，体感核心）

- **操作**：T3 入表后，**单独说**「鱼器渍」四个字（与 before 全句一致）。
- **验证**：
  ```bash
  grep "correction pair applied exact" ~/Library/Application\ Support/VoiceStick/VoiceStickApp.log | tail -1
  ```
- **预期**：粘贴出「语气词」，同一会话**没有** `refine begin` 日志（毫秒级直出，跳过 LLM）。
- **附带**：再说含错词的完整句子验证 few-shot 注入路径（精修正常、纠得更稳）。
- **结果记录**：

## T7 use/recall 反馈环 + 降权（doubao P3）

- **操作**：T6 命中后**等确认粘贴完成** → recall+1；再做一次但**粘贴前取消** → recall 不增。
- **验证**：
  ```bash
  python3 -c "import json;d=json.load(open('$HOME/Library/Application Support/VoiceStick/correction_pairs.json'));print([(p['before'],p['use_count'],p['recall_count']) for p in d['pairs'] if p['before']=='鱼器渍'])"
  ```
- **降权构造法**（应用 10 次门槛高，可手造）：退出 app → 手改该对为 `"use_count":10,"recall_count":4` → 重启后再说「鱼器渍」→ **预期不再自动替换**（降权对从自动应用池摘除），日志无 `correction pair applied exact`。测完把字段改回。
- **结果记录**：

## T8 热词候选挖掘（macOS 新增）

- **操作**：说「编辑 agentsdmd 这个文件」让精修纠为「AGENTS.md」——**重复 3 次**。
- **验证**：第 3 次出现悬浮提示「AGENTS.md 已多次出现…」+ 日志 `hotword candidates suggested`；设置 → 「热词候选」区出现该词。
- **加入路径**：点「加入热词」→ 热词框自动追加 → 点保存生效、候选移除。
- **忽略路径**：另挖一个新词点「忽略」→ 永不回弹（dismissed）。
- **结果记录**：

## T9 负信号事件流

- **操作 a**：识别完成后、粘贴前按取消；**操作 b**：录音中途取消重录。
- **验证**：
  ```bash
  tail -3 ~/Library/Application\ Support/VoiceStick/usage_events.jsonl
  ```
- **预期**：`paste_canceled`（带 dwell_s）/ `recognition_canceled`（无文本）。隐私红线：所有事件只有 text_length 与热词名单，**绝无文本内容**。
- **结果记录**：

## T10 热词评分负项

- **前置**：「语气词」已加入热词表（T8 加入或手填 config）。
- **操作**：说一段**包含该热词的文本**，粘贴前取消。
- **验证**：
  ```bash
  python3 -c "import json;d=json.load(open('$HOME/Library/Application Support/VoiceStick/hotword_usage.json'));print([(x['word'],x.get('negative_count')) for x in d if x.get('negative_count')])"
  ```
- **预期**：该词 `negative_count+1`，正样本 count 不变。长期效果：负样本多的词在 80-token 直传预算中排序靠后。
- **结果记录**：

## T11 dialog_ctx（行为观察）

- **操作**：第一轮说「我们在聊语气词这个话题」；紧接着第二轮说依赖上下文的同音短句。
- **验证**：history 连续两行 refined 文本即第二轮 ASR 请求的 dialog_ctx 载荷来源；行为上第二轮同音错误第一遍 ASR 即纠对。无直接观测口，可关掉精修开关做对照实验加强结论。
- **结果记录**：

---

## 回归提醒与数据回滚

- T9 的取消操作顺带覆盖老功能回归：侧键「恢复上次输入」仍应正常。
- 数据回滚：退出 app → 删除对应 json/db 文件 → 重启即从零积累（纠错对与历史都是本地资产，删除不可恢复）。
- 全部测完后：5 个功能分支按序合并 main（fix/windows-correction-pairs-parity → feat/windows-usage-events → feat/windows-session-history → feat/correction-recall-loop → feat/hotword-usage-negative）。
