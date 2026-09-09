# 本地文本精修（SenseVoice 口水词过滤）设计与实施方案

- 日期：2026-09-09
- 状态：已定案（基于 m0/refine spike 实测数据）
- 分支：feat/voice-recognition-option
- 关联：`Doc/Plan/local-asr-streaming-partial.md`（本地识别流式 partial）、`Doc/Plan/asr-settings-local-provider-merge.md`（本地识别并入服务提供方）

## 1. 背景与目标

本地 SenseVoice 路径的 final 文本直通注入，口水词（嗯/啊/呃/那个/就是/然后/叠字等）无过滤。实测确证（`m0/refine/wavs/` 真实 TTS→SenseVoice 转写）：

- 句首"嗯，""啊，那个"、句中"就是""然后呢"、叠字"我我我"全部原样输出；
- 专有名词形态被破坏（"SenseVoice"→"sense voice"、"Windows"→"windows"）。

现有 LLM 精修（`llm_refinement_client.cc`）走云端且默认关闭，与"本地识别断网可用"的定位矛盾。

**目标**：本地识别会话的 final 文本经本地精修后注入——断网可用、延迟可控（净推理 ≤1s 量级）、永不比不精修更差（守卫回退原文）。

**非目标**：macOS 端（本地识别本身仅 Windows）；替换云端精修（云端识别会话仍用云端精修，两者正交）。

## 2. Spike 结论（m0/refine/，llama.cpp b10868 CPU 本机实测）

评测集 25 条（`m0/refine/testset_refine.json`），few-shot prompt 与真实 SenseVoice 输出形态校准：

| 配置 | 达标率 | 红线失误 | 备注 |
|---|---|---|---|
| 0.6B-Q4_K_M + 指令式 prompt | 0% | 角色错乱（当聊天回复） | 指令式对 0.6B 无效 |
| 0.6B-Q4_K_M + few-shot 重写 | 13/25 | 2 丢内容 + 1 改写 | 378MB |
| 0.6B-Q8_0 + few-shot 重写 | 12/25 | 同样 2 丢 + 1 改写 | 量化非瓶颈，模型能力上限 |
| 0.6B + 删除指令模式 | ~2/25 | 大面积漏删 | 任务要求更高，不适用 |
| 1.7B-Q4_K_M + few-shot 重写 | **21/25** | 1 轻度误删（守卫可拦） | **定案**，1.1GB |
| 1.7B + 删除指令模式 | ~10/25 | 幻觉格式（守卫全拦） | 不采用 |

速度（Zen4 CPU、8 线程、llama-bench）：pp768=360 t/s，tg=55 t/s。

- 无缓存每句总代价 ≈ prefill(~750tk system+few-shot)=2.1s + 生成 0.3~0.7s，**不可接受**；
- **KV 前缀复用**（system+few-shot 只算一次，逐句清后缀）：净延迟 ≈ user prefill(~0.1s) + 生成 0.3~0.8s ≈ **0.4~0.9s**，达标。

失败模式数据回放（守卫设计的直接依据）：

- 0.6B："好的好的，我知道了。"→"好的"（丢"我知道了"）；"…process_data，注意是下划线。"→ 丢后半句；"吃饭了没有啊？"→"吃饭了吗？"（改写）；
- 1.7B：仅"就是，我想问一下就是，…"→"就是，这个支持 Windows 吗？"（误删"我想问一下"）。

## 3. 架构

三层防御，逐层降级，任何一层失败回退上一层结果（最终回退原文）：

```text
ASR final text
  └─ L1 规则引擎（纯函数，微秒级，总是执行）
       句首语气词删除 / 中文字符间空格清除 / 叠字规整 / 重复标点规整 / 孤立标点清理
  └─ L2 本地 LLM 精修（本地识别会话 && refine 开 && 模型就绪）
       llama.cpp 常驻引擎 + few-shot prompt + 贪心解码 + 流式 token 回调（复用 60ms 节流 UI）
  └─ L3 守卫（纯函数，放行或整句回退 L1 结果）
       a. 子序列校验：refined 忽略大小写/空白/标点后必须是 original 的子序列
          （防加字改字；同时天然放行热词大小写纠正 sense voice→SenseVoice）
       b. 删除片段校验：双指针 diff 出的每个被删片段必须整体命中口水词模式表
          （防删实词；"我知道了""注意是下划线""我想问一下"均被拦）
       c. 热词守卫：复用现有 RefineResultKeepsHotwords（原文已有的热词精修后必须仍在）
  └─ 注入（CompletePendingPaste 既有链路不动）
```

### 组件划分（desktop/windows/src/）

| 组件 | 职责 | 测试形态 |
|---|---|---|
| `text_refiner.h/.cc` | L1 规则引擎 + L3 守卫（`RefineText`/`RefineResultSafe` 纯函数） | 全量单测（spike 失误样本作回归用例） |
| `local_llm_engine.h` | 引擎抽象（TDD 注入点，仿 `SenseVoiceEngine` 先例）：`Generate(user_text, on_token, cancel) → bool` + 懒加载/线程/超时 | FakeEngine 驱动调度测试 |
| `llama_cpp_engine.h/.cc` | 生产引擎：llama.cpp C API 封装（chat 模板、KV 前缀复用、贪心采样、Qwen3 thinking 关闭） | 真模型 smoke（模型在位才跑） |
| `local_refinement_client.h/.cc` | 精修编排：L1→L2→L3 串接 + few-shot prompt 构建（含热词段）+ 失败回退 | FakeEngine 全链路单测 |
| 协调器接线 | `TransformText` 分流：本地会话走 local refiner（会话建立时钉住，与 `session_asr_` 路由对称） | 协调器级集成测试 |

### 与现有精修的关系

- 云端精修（`refine_enabled` + `refiner_`）：仅云端识别会话使用，不动。
- 本地精修：`[local_asr]` 段新配置，本地识别会话专用。会话开始时按 ASR 路由结果同步钉住精修路径（本地 ASR → 本地精修；云端 ASR → 云端精修），与会话内切换 provider 的竞态隔离（对称 fc1ce78f 的 `session_asr_` 钉住模式）。
- few-shot prompt 独立于云端 `BuildRefinePrompt`（spike 证明小模型必须用 few-shot 形态）；热词段拼接规则沿用现有设计（top-N 裁剪、只纠错不插入）。

## 4. 模型与推理框架

- **模型**：Qwen3-1.7B-Q4_K_M GGUF（~1.1GB，unsloth 转换，Apache-2.0）。放置：`models_dir/Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf`（目录名=模型名，与 SenseVoice 同风格；`refine_model` 可显式指定，相对路径锚 models_dir）。手动放置模式与 SenseVoice 一致，缺失自动降级纯规则并在设置 UI 明示。
  - 体积说明：超出 0.6B 档预算的决策依据是质量红线——0.6B 两档量化实测均不达标（丢内容/改写），守卫拦下等于白精修；1.7B 是质量达标的最小档。
- **框架**：llama.cpp vendored 源码（pin b10868，与 spike 同版），`third_party/llama.cpp-src`（gitignored，经 `FETCHCONTENT_SOURCE_DIR_LLAMA_CPP` 或镜像 URL 喂 FetchContent），只编 `ggml`+`llama` 静态库（`LLAMA_BUILD_EXAMPLES/SERVER/TESTS=OFF`、CPU only、无 backend-dl），静态链入 `voicestick_core`。无新增 DLL，与 sherpa-onnx 的 onnxruntime 零冲突。
- **运行形态**：外壳装配时同步加载（`SyncLocalRefiner`，~2.5s mmap，启动/设置保存各承担一次；失败注入 nullptr 退化直通）；常驻上下文；上下文长度 2048 = 前缀(~800) + user(≤512) + 生成(≤256)；贪心解码（temp=0）；Qwen3 thinking 关闭——**手拼 ChatML**（`llama_chat_apply_template` 不走 jinja，assistant 段起始注入空 `<think>\n\n</think>` 等价 enable_thinking=false）。
- **线程**：默认 6（SenseVoice 2 线程 + UI/注入留余量），`[local_asr] refine_num_threads` 可调。
- **KV 前缀复用**：system+few-shot 前缀只 decode 一次；逐句 `llama_memory_seq_rm(seq, prefix_len, -1)` 清后缀后 decode user。净延迟 0.4~0.9s/句（Release；见 §8 Debug 陷阱）。
- **取消与超时**：复用 `refinement_cancel_token_` 机制；单句生成上限 256 token + finalizing watchdog（15s，on_token 持续 Touch）双保险，超时回退粘贴 ASR 原文。
- **llama.cpp C API 集成坑**（b10868 实测，改代码前先读）：
  - `llama_tokenize` 缓冲不足时返回**负的所需长度**（非错误）；0 才是空文本。
  - `llama_sampler_sample(smpl, ctx, idx)` 的 idx 非序列 id：非负=batch token 位（该位无 logits 直接 GGML_ABORT）、负值=倒数第 |idx| 个 output 行；官方用法传 `-1`（每次 decode 仅末 token 带 logits 时恒指它）。
  - `LU8` 宏依赖 `__cplusplus`：MSVC 不加 `/Zc:__cplusplus` 恒报 199711L，与 `/std:c++20` 的 char8_t 组合走错分支（llama-chat.cpp C2664）——llama 目标加 `/Zc:char8_t-`。
  - ggml 子项目 `option(BUILD_SHARED_LIBS)` 默认 ON 且写 CMakeCache，regenerate 时回灌把本项目未显式指定类型的 `add_library`（opus/voicestick_core）变 DLL（符号不导出 LNK2019 + 运行时 DLL 依赖）——CMakeLists 顶部 `set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)` 全树钉死 + core 显式 STATIC。

## 5. 配置与设置 UI

`[local_asr]` 新增（`Doc/Ref/desktop-config.md` 同步）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `refine_enabled` | `true` | 本地识别会话的本地精修开关（与顶层云端 `refine_enabled` 正交） |
| `refine_model` | `""` | GGUF 路径；空 = `models_dir`（或 exe 旁 `models/`）下 `refine/qwen3-1.7b-q4_k_m.gguf` |
| `refine_num_threads` | `6` | CPU 推理线程数 |
| `refine_prompt` | `""` | 本地精修 system prompt（含 few-shot 示例），多行文本；空 = 内置默认 |

`refine_prompt` 用户可编辑（2026-09-09 增）：设置 → 本地语音识别区块「本地精修提示词」多行框，初始显示当前生效提示词（空配置填内置默认全文）；保存时 CRLF 归一 + 与默认等值归空（清空即恢复默认）。提示词参与 `SyncLocalRefiner` 幂等键：变化即重建引擎重算 KV 前缀（system+few-shot 前缀 ~320 token，重建含 1.1GB 模型加载 ~1s）。语义对齐云端顶层 `refine_prompt`（用户改 few-shot 补充口水词示例即可加强过滤，L3 守卫仍保底不劣化）。

设置对话框"本地语音识别"区块：精修复选框 + 模型状态回显（存在/缺失，仿 SenseVoice 模型校验行）。默认开的理由：选本地识别的用户目的就是干净文本；模型缺失时自动降级 L1 规则并明示（不静默假装精修）。

## 6. 测试策略（TDD）

1. **红-绿-重构**：`text_refiner` 规则与守卫纯函数先行（每规则正/边界/负例）；spike 真实失误样本（S12 丢"我知道了"、S19 丢"注意是下划线"、S23"没有啊→吗"、S11 误删"我想问一下"、热词大小写纠正放行）全部作为守卫回归用例。
2. **FakeLlmEngine**：注入式驱动 `local_refinement_client` 编排（流式回调/取消/超时/失败回退/L1 结果兜底）。
3. **协调器集成**：本地会话 final → 精修链路 → 注入断言（对称 `TestCoordinatorDeviceSessionRoutesToLocalAsrWhenEnabled` 模式）。
4. **真模型 smoke**：模型在位才跑（env `VOICESTICK_REFINE_MODEL_DIR` 优先回退 `m0/models/`，仿 `DetectSenseVoiceDir`），断言典型句含/不含关键词 + KV 复用后二次调用延迟上限。缺模型 printf SKIPPED。
5. **CTest 全量回归** + 真机闭环（按 AGENTS.md：构建后自动重启 VoiceStick.exe，本机麦克风模式实测口水词句子，看悬浮窗流式输出与最终注入文本）。

## 7. 交付切分

1. `text_refiner` 纯函数（规则+守卫）+ 单测——独立可合入；
2. llama.cpp vendored 接入（CMake 编译通过 + 冒烟）；
3. `LocalLlmEngine`/`llama_cpp_engine` + Fake 单测 + 真模型 smoke；
4. `local_refinement_client` 编排 + 协调器接线 + 配置/UI + 文档同步；
5. 全量回归 + 真机验证 + 性能调优（线程数/KV 复用实测）。

## 8. 风险与对策

- llama.cpp vendored 的 MSVC 编译时长（增量 3~6 分钟）与兼容性：pin b10868（官方 CI 含 MSVC）；裁剪 targets；失败可退官方预编译 DLL 方案（次选，多 3 个 DLL 分发）。
- 内存常驻 ~1.1GB（模型权重）：装配时加载 + 未启用不加载；后续可加空闲卸载。
- 1.7B 剩余失误（连接词漏删 S09 类）：接受（无害），守卫保证不劣化。
- few-shot 示例与热词段叠加后的前缀变化：前缀长度运行时测量后固定，KV 复用按实测长度切分。
- **Debug 构建性能陷阱**：无优化（/Od /RTC1）下 GGML 推理慢 20~40 倍（实测 prefix prefill 700ms/token vs Release ~3ms/token，单句 4 分钟起）。真模型 smoke 仅 NDEBUG 跑（Debug SKIP）；**真机性能验证与发布构建必须 Release/RelWithDebInfo**。
