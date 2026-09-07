# P1 软件 MVP 技术方案（第一迭代：核心闭环）

> 对应路线图 v1.3 §四 P1「桌面客户端（Win 先行）」；M0 结论（SenseVoice int8 RTF 0.016 / CER 2.45%，四引擎评测）为选型基线。
> 本文档是 P1 第一迭代的实施方案；P1 后续迭代（50 人种子验证、耳机组跟踪）为运营动作，不在本文范围。

## 1. 目标与验收标准

**核心闭环**：全局热键按住说话 → 松手 → 本地 SenseVoice 识别 → 热词纠正 → AI 改写 → 文本注入当前焦点窗口，全程悬浮条状态反馈。

| 验收项 | 标准 | 来源 |
|---|---|---|
| 回填延迟 | 松手到文字落进焦点窗口 < 500ms | M0 未竟项 + 路线图 P1 |
| 断网可用 | 全链路（识别+规则改写）离线可跑 | 本地优先原则 |
| 热词纠正 | 注入热词后，已知错例（如 kuernetes）被纠正 | 路线图 §6.2 SenseVoiceAdapter |
| 热词资产 | SQLite 落盘密文（明文不可 grep）+ Schema v1 冻结 | 路线图 §6.4 |
| 单测 | 全绿；引擎/注入等外部边界按「不伪造」原则跳过或真跑 | 仓库安全红线 |

## 2. 架构分层（对齐路线图 §五）

```text
p1/src/p1/
├── config.py            # 配置加载（p1/config.toml，凭据不入仓库）
├── interaction/         # ① 交互层
│   ├── hotkey.py        #    全局热键（keyboard 库：按住=录音，松开=出字；Esc=取消）
│   ├── overlay.py       #    悬浮条（tkinter 无边框置顶：录音中/识别中/完成态）
│   └── injector.py      #    输出适配器（剪贴板 + Ctrl+V 模拟）
├── orchestration/       # ② 编排层
│   ├── session.py       #    录音会话（sounddevice 16k mono 采集，电平回调）
│   └── pipeline.py      #    松手后：识别 → 热词纠正 → 改写 → 注入（状态机）
├── flywheel/            # ③ 飞轮中间层
│   ├── hotword_store.py #    SQLite 热词库（Schema v1，字段级 AES-GCM 加密）
│   ├── corrector.py     #    热词后处理纠正（拼音相似度 + 编辑距离，替换式）
│   └── context_builder.py #  识别前组装热词上下文（top-N 按 weight×freq）
├── rewrite/             #    AI 改写（规则版本地必选；LLM 云端可选降级）
│   └── rewriter.py
└── engines/             # ④ 引擎层
    └── sense_voice.py   #    EngineAdapter 具象（sherpa-onnx，权重复用 m0/models/）
```

**与 M0 的关系**：P1 不 import `m0/`（验证工程不进产品链路）；SenseVoice 初始化要点借鉴 `m0/src/m0_asr/engine.py`；模型权重共享 `m0/models/`（路径可配，避免重复下载 3GB）。

## 3. 关键设计决策

### 3.1 热键：默认右 Ctrl 按住说话
单键长按最顺手且右 Ctrl 极少冲突（豆包 Fn / 讯飞 Ctrl 键同理）；`[hotkey] key` 可配置。Esc 全局取消当前会话。

### 3.2 注入：剪贴板 + Ctrl+V（不逐键打字）
`keyboard.write()` 对中文逐键不稳定；业界通行剪贴板回填。代价是覆盖用户剪贴板——P1 接受，P2 再做恢复。

### 3.3 热词纠正放后处理（非解码偏置）
sherpa-onnx 热词偏置仅 transducer 支持（M0 结论），SenseVoice 是非自回归模型无此能力；路线图 §6.2 明确 SenseVoiceAdapter 走「后处理管线编辑距离替换」。实现：识别文本切片 → 对每个 token 与热词库候选做拼音相似度（pypinyin）+ rapidfuzz 编辑距离双门限 → 过线替换并记录纠正事件（悬浮条反馈「已纠正 X→Y」）。

### 3.4 热词库 Schema v1 冻结 + 字段级加密
按路线图 §6.4 原样落 SQLite（surface/pron/tags/embedding 字段 AES-GCM 加密，密钥首次随机生成存 `%APPDATA%\VoiceStickP1\hw.key`）；只加字段不破坏兼容；`source` 区分 manual/correction/auto_mine；weight 按频次动态调整 + 久不用衰减。

### 3.5 AI 改写：规则版必选，LLM 可选
- **规则版（本地，默认）**：去口癖（嗯/呃/那个）、去重复词、补中英标点与分句——离线可跑，覆盖 P1 主路径；
- **LLM 版（可选）**：腾讯混元（凭据复用 M0 三级解析思路：env > p1/config.toml > VoiceStick config.toml），无凭据自动降级规则版，不 mock 不伪造。

### 3.6 线程模型
tkinter mainloop 主线程；sounddevice 音频回调线程；keyboard 钩子线程；三者经 `queue.Queue` 解耦到控制器状态机（idle → recording → recognizing → injecting → done），UI 用 `after()` 轮询刷新。

## 4. 配置（p1/config.toml，gitignored；example 进仓库）

```toml
[hotkey]
push_to_talk = "right ctrl"   # 按住说话
cancel = "esc"

[engine]
adapter = "sense_voice"       # P1 主路径本地；云端适配器二期
models_dir = "../m0/models"   # 共享 M0 权重

[rewrite]
enabled = true
provider = "rules"            # rules | tencent_hunyuan（无凭据自动降级 rules）

[hotword]
db_path = ""                  # 空 = %APPDATA%\VoiceStickP1\hotwords.db
decay_days = 30               # 超期未用 weight 衰减
```

## 5. 测试策略（TDD）

| 层 | 策略 |
|---|---|
| config / hotword_store / corrector / rewriter / pipeline | 纯单测（AAA，边界+错误路径）；加密落盘用「grep 明文」断言 |
| sense_voice 适配器 | 集成测试：模型就位真识别（复用 m0 测试音频），未就位 SKIP 不 mock |
| injector | 剪贴板往返计时单测；真窗口注入手动验收（延迟写日志） |
| overlay / hotkey / main | 薄胶水层，真机手动验收（状态流转 + 端到端延迟日志） |

## 6. 明确不做（本迭代）

- 云端 ASR 切换进产品主路径（评测平台已有，产品侧二期接 EngineAdapter 注册表）
- 剪贴板内容恢复、多方案热键映射、托盘菜单（P1 后续迭代）
- 50 人种子验证、埋点留存统计（运营阶段）
