# 豆包输入法逆向结论驱动的用户数据飞轮方案（RFC）

> 状态：**待实施**（本文件是交接文档，由豆包输入法 macOS 1.0.2.7 静态逆向分析 + 本仓库现状差距分析产出，2026-10-15）
> 目标：建立"越用越准"的用户数据闭环——每次 LLM 精修 diff、每次划词修正、每次跨轮纠错，都沉淀为带精确率统计的本地资产，逐步降低对 LLM 调用的依赖。
> 前置阅读：`hotword-eval-and-prioritization.md`（热词评分裁剪现状）、`local-asr-accuracy-and-cross-turn-refinement.md`（跨轮纠错 M0~M4，已实施）。

---

## 0. 给实施者的速读（TL;DR）

1. 本方案**不引入任何新模型、新云端依赖**。核心是新增一张「改写对表」（SQLite），把仓库里已有的三个 diff 源（LLM 精修、跨轮纠错、划词修正）的产物从"一次性调用"变成"可累积的确定性规则资产"。
2. 抄豆包输入法的三个机制设计（不是抄它的数据）：
   - **改写对存储**：错误形式→正确形式的映射，带 `use_count`/`recall_count` 双计数（实测精确率）；
   - **拼音约束纠错**：同音/近音封闭集校验，收窄纠错空间，降低误改（直接针对"鱼器渍"类同音混淆）；
   - **反馈环**：用户回退→规则降权；精确率低于阈值→自动淘汰/提示删除。
3. 豆包的 825MB 离线声学模型**不可复用**（全文件加密），170MB NER 模型**不值得复用**（私有二进制权重格式），详见 §5，不要在这上面花时间。
4. 一个重要的交叉验证：豆包（LLM 级声学模型 + 完整后处理管线）与本仓库 M0 spike 结论（方案 B：LLM 文本层纠错 > 换更大 ASR 模型）一致——**后处理管线的投产回报高于升级 ASR 模型**，本方案是后处理管线的资产化延续。

---

## 1. 背景：豆包输入法的证据（逆向结论摘要）

分析对象：`DoubaoImeInstaller_v1000207.zip`（v1.0.2.7）及安装后数据 `~/Library/Application Support/DoubaoIme/`。方法：纯静态（脚本/plist/符号/字符串/SQLite schema），未执行目标程序。

### 1.1 直接证据：豆包的"越用越准"数据结构

**`~/Library/Application Support/DoubaoIme/ASRContext/context.db`**（明文 SQLite，可直接 `sqlite3` 查看 schema）：

```sql
CREATE TABLE modify_pair (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    src_text VARCHAR NOT NULL,      -- ASR 错误形式
    dst_text VARCHAR NOT NULL,      -- 用户认可的正确形式
    src_text_enc TEXT, dst_text_enc TEXT,  -- 字段级加密副本（内容隐私）
    src_text_tag TEXT, dst_text_tag TEXT, pair_tag TEXT,
    key_version INTEGER NOT NULL DEFAULT 1,
    update_at VARCHAR NOT NULL,
    use_count INTEGER NOT NULL DEFAULT 0,   -- 该改写被命中应用的次数
    recall_count INTEGER NOT NULL DEFAULT 0, -- 应用后被用户保留的次数
    create_at VARCHAR NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(src_text, dst_text),
    UNIQUE(pair_tag)
);
-- 同库还有 user_word 表（用户词）
```

关键洞察：**`recall_count / use_count` 就是每条改写规则的实测精确率**。这是豆包防"越改越错"的反馈环基础。

**引擎侧词典家族**（OimeEngine C++ 符号，`oime_engine::pinyin::PyDictManager` 管理的单例集合）：
`SysBigDict`（系统词库）、`SysBigramDict`/`UsrBigramDict`/`UsrHistoryBigramDict`（系统/用户 bigram 语言模型）、`CorrectDict`（纠错）、`AsrNgramDict`（ASR 专用 n-gram）、`AsrUserCorrectDict`/`AsrModifyExemptWordDict`（ASR 用户纠错 + **改写豁免**词典）、`CloudHotWordDict`/`CloudPrefetchDict`（云热词 + **云纠错结果落地缓存**）、`UsrEnhanceDict`、`ContactDict`、`QuantifierDict`、`SimTradDict`、`SensitiveWordFilter`。

三个对 VoiceStick 有直接参考价值的设计：
- **用户 bigram**：不只记词频，记词的共现——"部署到"之后大概率跟"服务器/K8s"；
- **CloudPrefetchDict**：云端纠错结果落地为本地规则，同样错误模式下次不再请求云端（省延迟、省 token）；
- **AsrModifyExemptWordDict**：专门一张"豁免表"防止过度纠错——说明豆包踩过"用户想要的'错误'被反复纠正"的坑。

**拼音数据**：`~/Library/Application Support/DoubaoIme/ContextAuxModel/versions/1_1/context_aux_model/pinyin_dict/hzpy-utf8.txt`（690KB 纯文本），每行格式：

```
汉字,全拼,声母,韵母,Unicode码,声调
一,yi,y,i,4E00,1
丁,ding,d,ing,4E01,2
```

同目录还有 cppjieba 标准分词词典（`jieba.dict.utf8`/`hmm_model.utf8`，即 MIT 开源 cppjieba 项目的原样打包）。

### 1.2 已确诊的问题（本仓库自己的证据）

`local-asr-accuracy-and-cross-turn-refinement.md` §1："鱼器渍"（=语气词）是**同音近音混淆**（yǔqìcí→yúqìzì，声韵全同、仅声调与一字韵腹微差），SenseVoice-Small int8 对低频领域词的固有弱点，且 SenseVoice 不支持热词偏置（sherpa-onnx 热词仅 transducer）。**改进方向在文本层，不在音频/解码层**——本方案 §3.2 的拼音约束正是文本层的确定性方案。

---

## 2. 现状盘点与差距

| 能力 | 本仓库现状 | 豆包做法 | 差距 |
|---|---|---|---|
| 热词资产 | 扁平词表 + 评分裁剪（`hotword_selector`，频率×新近度×手动，Windows+macOS） | 热词表 + 用户 bigram + 云热词缓存 | 缺词间关系与云结果落地 |
| 纠错沉淀 | `hotword_candidate_miner` 只提取**标识符样式**的词候选（阈值 3 次入表，CJK 暂不挖掘） | **改写对映射**（任意文本→文本，含 CJK）+ 双计数 | 缺映射级资产，CJK 空白 |
| 反馈环 | 用户确认制（dismissed 集合，防负反馈） | 精确率可计算（recall/use），坏规则自动淘汰 | 缺自动降权 |
| 存储 | JSON 文件（`hotword_candidates.json`） | 加密 SQLite（WCDB/SQLCipher）+ 账号云同步 | 数据量天花板（多写者已用事务互斥收敛，但 JSON 无索引/无聚合能力） |
| 场景感知 | 有 `FrontmostAppProvider`（macOS，当前仅用于粘贴目标） | 按输入场景分区统计词典 | 没接上 |

**平台现状注意**：划词加词与候选挖掘目前是 **Windows-only**（README 实证），评分裁剪进 prompt 是双端。实施时先核实 macOS 端 diff 源的实际覆盖（`desktop/macos/Sources/` 下与 `AsrServing`/精修相关的 diff 产物），本文件不预设结论。

---

## 3. 方案设计

### 3.1 改写对存储（ModifyPairStore）——核心地基

**新表**（SQLite，与 `config.toml` 同目录，文件名建议 `modify_pairs.db`；Windows 放 `%APPDATA%`，macOS 放 `~/Library/Application Support/VoiceStick/`——与现有 DebugAudio 等路径约定一致，实施时对齐 `AppConfig` 的路径分发）：

```sql
CREATE TABLE modify_pair (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    src_text TEXT NOT NULL,           -- ASR/精修前形式（如"鱼器渍"）
    dst_text TEXT NOT NULL,           -- 用户认可形式（如"语气词"）
    pinyin_src TEXT,                  -- 冗余列：src 的无声调拼音串（见 3.2，建索引用）
    pinyin_dst TEXT,
    app_bucket TEXT,                  -- 前台应用 bundle/进程名分桶（见 3.4），空=全局
    source TEXT NOT NULL,            -- 'refine' | 'cross_turn' | 'selection' | 'manual'
    use_count INTEGER NOT NULL DEFAULT 0,
    recall_count INTEGER NOT NULL DEFAULT 0,
    exempt INTEGER NOT NULL DEFAULT 0, -- 豁免标记（见 3.3）
    last_used_at TEXT,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(src_text, dst_text, app_bucket)
);
CREATE INDEX idx_modify_pair_pinyin ON modify_pair(pinyin_src);
CREATE TABLE modify_exempt (          -- 豁免表（学豆包 AsrModifyExemptWordDict）
    text TEXT PRIMARY KEY,
    reason TEXT,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
```

与豆包 schema 的差异及理由：去掉字段级加密列与 `key_version`（单机版暂不做加密，见 §6 远期）；增加 `source`/`app_bucket`/`pinyin_*` 三个可观测列。

**三个数据源接入点**（全部已有产物，只缺"落库"这一步）：

| 源 | 现有代码位置（Windows） | diff 内容 |
|---|---|---|
| LLM 精修 | `hotword_candidate_miner.cc` 的 `MineRefinementCandidates(original, refined, ...)` 调用点（协调器内精修回调） | original vs refined 的完整文本对 |
| 跨轮纠错 | Qwen3-4B M3 协调器接线处（`refine_cross_turn` 开关路径，见 `local-asr-accuracy-and-cross-turn-refinement.md` §3.6） | 跨轮修正前后文本对 |
| 划词修正 | `selection_hotword_manager.cc`（划词通道已有，当前产物是"加词"） | 需扩展：划词**替换**场景记录 原文→改写 |

**入表规则**（关键设计，防止垃圾进垃圾出）：
- diff 先做**最小编辑对齐**（词/短语级，不是整句入表）：只提取被修改的连续片段对；相同片段对累计出现（计数列复用 use_count 之前的命中计数，或加 `seen_count`）；
- **拼音校验前置**（§3.2）：src 与 dst 拼音距离在阈值内的对优先入表（同音纠错是最安全、复用价值最高的）；非同音对（语序调整、删口头语）标记 `source` 但初始权重低；
- 与热词候选挖掘的关系：`hotword_candidate_miner` 继续负责"词"维度，本表负责"改写"维度，互不替代。

> **落地注记（2026-10-15）**：应用位置 1（全句精确命中 → 直接替换跳过 LLM）已实施——双端 `ExactMatchCorrectionPair`（去首尾空白全串比较，多条命中取 count 最高，确定性）+ `MarkCorrectionPairUsedOnDisk`（use_count 落盘，P3 反馈环召回侧基底）；位置 2（拼音片段近邻）与位置 3（few-shot 已先行）随 P2 拼音约束落地。

**应用位置**（读取侧）：ASR final 文本 → 精修 LLM 调用**之前**，先查改写表：
1. 全句精确匹配 `src_text` → 直接替换（毫秒级，跳过 LLM）；
2. 分词后逐片段拼音匹配（走 `pinyin_src` 索引）→ 生成"建议替换集"；
3. 未命中部分才进入现有精修管线，且把命中过的改写对作为 few-shot 示例拼进精修 prompt（豆包 `CloudPrefetchDict` 思路：确定性结果落地，LLM 只处理增量）。

### 3.2 拼音约束纠错

**数据源**：不要用豆包的 `hzpy-utf8.txt` 文件本身（版权洁净度考虑），用开源等价数据：`mozillazg/pinyin-data`（MIT，`pinyin.txt` 无声调 + `pinyinTone.txt` 带调）或 pinyin-pro 的数据。豆包那份格式（`汉字,全拼,声母,韵母,Unicode,声调`）可作为字段设计参考与交叉校验样本。

**核心算法**（文本层，无模型）：
1. 汉字串 → 无声调拼音串（多音字取全部候选，生成集）；
2. **同音判定**：src 与 dst 拼音串完全相等 → 同音替换（最高置信）；
3. **近音判定**：拼音编辑距离 ≤ 1（允许声调差、zh/z ch/c sh/s、n/l、前后鼻音 in/ing en/eng an/ang——这组模糊音对是 ASR 错误的主力，豆包引擎里同样有 FuzzyType 模糊音机制，`AsrWordConversion` 符号实证）；
4. 应用场景一（入表闸门）：§3.1 的入表规则；
5. 应用场景二（纠错收窄）：LLM 精修 prompt 从"开放式改写"升级为"封闭集选择"——对每个疑似错误片段给出拼音近邻候选集，要求 LLM 只在候选集内选或输出 `KEEP`。这直接降低误改率，尤其保护专有名词不被"顺手"改掉。

**"鱼器渍"用例走查**（验收基准）：`yu qi zi` vs `yu qi ci`，声母 z/c 属于平翘舌模糊对 → 近音判定通过 → 改写对"鱼器渍→语气词"入表 → 下次同错误全句片段命中，零 LLM 调用完成纠错。

### 3.3 精确率反馈环

- **命中计数**：每次规则应用 → `use_count += 1`；
- **保留信号**：应用后用户没有回退（自动粘贴流程正常走完 / 手动确认时选了"确认"）→ `recall_count += 1`；用户回退（现有侧键"恢复上一次输入确认"通道可以拦截到回退事件——macOS 侧键双击恢复语义已有状态机，Windows 有对应取消路径）→ 不增 recall；
- **降权与淘汰**：`use_count >= 10 && recall/use < 0.5` → 从自动应用池摘除，进设置页"低置信改写"列表等用户裁决；连续 3 次应用即回退 → 直接置 `exempt = 1`；
- **豁免表写入**：用户在设置页删除某条改写 → 同步写 `modify_exempt`，且该 (src,dst) 永不再自动入表（学豆包独立豁免词典；也呼应 `hotword_candidate_miner.h` 里"为什么不做全自动入表"的既有设计哲学——本方案把该哲学从"入表"延伸到"应用"环节）。

### 3.4 用户 bigram 与分应用分桶

> **落地注记（2026-10-15）**：分桶已在纠错对上先行落地（macOS 先行，feat/correction-app-bucket）——采集打 `app` 桶 + 查询「当前应用桶 ∪ 全局桶、当前桶优先」应用于 few-shot 与全句精确命中两处；bigram 未做（依赖 P2-1 SQLite 化）。

**分桶（先做，成本低）**：
- `FrontmostAppProvider`（macOS）/ Windows 前台进程检测（现有粘贴注入目标检测可复用）取 app 标识；
- 改写对与热词记录时打 `app_bucket`；查询时先查当前应用桶，miss 再查全局桶；
- 预期：微信聊天桶与 IDE 桶的热词/改写几乎不重叠（豆包 `KeyboardRecordRegionProvider` 分区统计的等价物）。

**bigram（后做，在改写表数据之上）**：
- 新表 `user_bigram(prev_token, next_token, app_bucket, count, last_seen_at)`，分词用 cppjieba（直接引开源上游，MIT；C++ 端 cppjieba/jieba 头文件库，Swift 端可用 jieba-swift 或最简前向最大匹配 + §3.2 拼音表做生词回退——不必引入重依赖）；
- 应用：精修 prompt 的上下文段从"最近 3~5 轮对话"扩展为"当前片段前缀的高频后继词 top-N"（例：检测到"部署到"→ prompt 附注"该用户此语境高频后继：服务器、集群、测试环境"）；
- 这一步让"跨轮窗口"的知识在重启后留存，是热词表（点）到语言模型（面）的中间形态。

---

## 4. 与现有管线的集成图

```text
ASR final 文本
   │
   ▼
[新] ModifyPairStore 查询（精确/拼音近邻匹配，带 app_bucket 过滤）
   │ 命中 → 直接替换（跳过 LLM，计数 use_count）
   │ 未命中 ↓
   ▼
现有精修管线（LLM / Qwen3-4B 跨轮）
   │ 产出 original vs refined diff
   ▼
[新] diff → 最小对齐 → 拼音校验 → modify_pair 入表（seen 累计）
   │
   ▼
现有 hotword_candidate_miner（标识符词候选，保持不动）
   │
   ▼
粘贴 → 用户保留/回退 → recall_count / 豁免反馈（§3.3）
```

---

## 5. 明确不可复用的豆包资产（避免走弯路）

| 资产 | 结论 | 证据 |
|---|---|---|
| 825MB Flute 离线声学模型（`ASRModel/versions/1_8/asr_model.model`，下载槽位名 `acllm_main_model_mac`） | ❌ 不可行 | 头部 5 字节魔数 `FLUTE` 后**全文件熵 7.99 bits/byte**（加密）；主程序含 `expand 32-byte kflute-v3-key-mix`（ChaCha20/Salsa20 常量+密钥方案名）；SAMIAuth 在线授权握手；推理器 `flute::Inference` 闭源。三重锁死 |
| 170MB 领域 NER 模型（`ContextAuxModel/.../ner_model/*.convert`） | ❌ 不值得 | 头部是 CRF++ 模板/标签（明文可读），但权重段是剪枝后的私有二进制格式（`compression_result.json` 实证 theta 剪枝压缩），标准 CRF++ 库不可加载；且本仓库 LLM 精修已覆盖其职责 |
| OimeEngine 拼音词库（安装包内 152MB 哈希名资源） | ❌ | 自定义 trie 格式 + MurmurHash2 索引 + 哈希文件名，无引擎运行时无法解析；且属输入法候选排序资产，与本方案后处理管线不同构 |
| 豆包 `hzpy-utf8.txt` / cppjieba 词典 / 简繁表 | ⚠️ 思路复用，数据换开源源 | 内容本身是公共知识且有 MIT 开源等价物（`mozillazg/pinyin-data`、cppjieba 上游）；豆包文件仅作格式参考与交叉校验 |

---

## 6. 分阶段落地

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P1 改写对表** | SQLite 表 + 三源 diff 落库（先 Windows 后 macOS，跟随现有 diff 源覆盖）+ 设置页查看/删除 | "鱼器渍→语气词"经 3 次精修后自动入表，可在设置页看到 |

> **落地注记（2026-10-15）**：改写对资产已先行以 JSON 形态落地（`correction_pairs.json`，精修 diff 单源，few-shot 注入），SQLite 迁移与设置页查看/删除随本表 P2-1/设置页阶段补齐。
| **P2 拼音约束** | 开源拼音数据接入 + 同音/近音判定 + 入表闸门 + 精修 prompt 封闭集改造 | 同音错误第二次出现时零 LLM 调用完成纠错；误改率不高于基线 |
| **P3 反馈环** | use/recall 计数 + 回退拦截 + 降权/豁免 | 构造连续回退用例，规则被自动摘除并写入豁免表 |
| **P4 分桶+bigram** | app_bucket 过滤 + bigram 表 + prompt 上下文扩展 | 切换前台应用后热词/改写命中率分布可见分桶效应 |
| **远期** | 存储加密（SQLCipher）、多设备同步（参考豆包 WaveAccountSync 的分模块同步：词库/设置/改写对分表同步） | — |

依赖关系：P1→P2→P3 串行（P2 依赖表结构，P3 依赖应用路径）；P4 可与 P3 并行。P1/P2/P3 均不引入新模型与新云端依赖。

---

## 7. 防护设计与注意事项

1. **防负反馈**（最高优先约束）：改写规则一旦错误会系统性带偏后续识别。三道闸：入表需拼音校验 + 次数累计；应用后回退即不记 recall；低精确率自动摘除。**永远不做无反馈的自动入表**——这是 `hotword_candidate_miner.h` 既有哲学的延续。
2. **热词容量预算不变**：本方案不改变直传 ASR 的热词预算约束（火山双向流式 ~100 tokens / 本仓库 `kHotwordCorpusTokenBudget = 80`，腾讯临时热词 128 词——见 `hotword-eval-and-prioritization.md`）。改写对在**文本层**生效，不占 ASR 侧预算，两者互补不互斥。
3. **多写者并发**：改写表会有精修回调线程、跨轮线程、设置页 UI 三个写者。沿用 `hotword_candidate_miner` B13 的既有模式：**单一写入口 + 进程级互斥 + 每次从磁盘重读后改写**，SQLite 打开 `WAL` 模式。
4. **隐私**：改写对内容是用户语音/输入痕迹。单机版暂不加密但**绝不遥测**；若未来上云同步，参考豆包的字段级加密（`src_text_enc`/`key_version` 列设计）。
5. **平台对齐**：Windows（C++20）与 macOS（SwiftPM）实现同一 schema、同一判定算法；拼音编辑距离与模糊音对表建议两侧共享一份 JSON 常量，避免双实现漂移。

---

## 附：证据出处备查

- 豆包安装包/安装后分析工作目录（如需复核）：`~/Library/Application Support/DoubaoIme/`（context.db、ASRModel、ContextAuxModel）；安装包原件 `/Users/hwp/Dev/DVG/DoubaoIme/`。
- 本仓库现状代码：`desktop/windows/src/hotword_candidate_miner.{h,cc}`、`hotword_selector.{h,cc}`、`hotword_extractor.{h,cc}`、`selection_hotword_manager.{h,cc}`；macOS 端 `desktop/macos/Sources/`（`FrontmostAppProvider.swift` 等）。
- 相关既有文档：`Doc/Plan/hotword-eval-and-prioritization.md`、`Doc/Plan/local-asr-accuracy-and-cross-turn-refinement.md`、`Doc/Plan/selection-hotword-correction-and-asr-hotword-spike.md`、`Doc/Plan/local-text-refinement.md`。
