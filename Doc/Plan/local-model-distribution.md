# 本地模型分发（按需下载器）设计与实施方案

- 日期：2026-09-09
- 状态：迭代一已交付（清单 + 下载器核心 + 回环集成测试）；迭代二/三待做
- 分支：feat/voice-recognition-option
- 关联：`Doc/Plan/local-mic-mode.md`（本地识别）、`Doc/Plan/local-text-refinement.md`（本地精修）、`Doc/Plan/asr-settings-local-provider-merge.md`（设置入口）、`Doc/Plan/windows-local-firmware-ota.md`（固件 OTA 清单先例）

## 1. 背景与决策记录

本地模式依赖两个模型文件，合计约 **1.34GB**：

| 模型 | 文件 | 体积 | 许可证 |
|---|---|---|---|
| SenseVoice int8（sherpa-onnx 2024-07-17 打包） | `model.int8.onnx` + `tokens.txt` | 239,233,841 + 315,894 B | MIT（FunASR 生态，目录内 LICENSE 指 FunASR） |
| Qwen3-1.7B-Q4_K_M | `Qwen3-1.7B-Q4_K_M.gguf` | 1,107,409,472 B | Apache-2.0 |

当前安装包 28MB（便携 zip）。**决定不把模型打进 MSI/便携包**：

1. WinSparkle 全量更新（appcast 无差分）× 1.4GB = 每次发版全用户重下，1 万活跃用户月更一版年流量约 168TB；
2. 云端 ASR 用户不需要为本地模式付 1.34GB；
3. 模型迭代与应用发版解耦；GitHub Release 单文件上限 2GB，GGUF 已占 1.03GB。

**方案**：瘦安装包不变，首次启用本地模式时由应用内**模型下载向导**按需下载，多源回退 + SHA-256 校验 + 断点续传。分发成本（每完整本地用户一次性 1.34GB）：OSS 直出约 0.67 元、套 CDN 约 0.32 元、ModelScope 分流部分为零。

## 2. 目标与非目标

**目标**：设置页选「本地语音识别」后，模型缺失时一键下载到本机缓存并自动接线；断点续传；哈希校验；多源回退；中英双语文案。

**非目标**：macOS 端（本地识别本身仅 Windows）；远端动态清单（模型极少变，编译期内置；将来需要再加）；推理上云。

## 3. 架构

新模块 `model_downloader`（进 `voicestick_core`，无 UI 依赖），复用 `firmware_manifest.cc` 已验证的 WinHTTP + BCrypt 配方；差异：固件下载是整包进内存（2MB），模型必须**流式落盘**。

```text
设置页「下载模型…」按钮（本地识别选中且状态 ✗ 时可见）
  └─ ModelDownloadDialog（模态向导，迭代二）
       ├─ 条目：ASR 240MB（必选锁定）/ 精修 1.1GB（可选，默认勾选；跳过=纯规则精修）
       ├─ 磁盘空间预检 GetDiskFreeSpaceExW
       └─ 后台线程逐文件下载，PostMessage 报进度
            └─ ModelDownloader::DownloadFile(spec, dest, progress, cancel)
                 多源顺序：OSS → ModelScope → GitHub Release
                 WinHttp 流式读（64KB 块）→ 追加写 <dest>.part + BCrypt 增量 SHA-256
                 .part 已存在 → Range: bytes=N- 续传（206 续 / 200 重下）
                 完成：哈希匹配 → 原子改名 .part → dest；不匹配 → 删 .part 换下一源
       └─ 全部完成 → 回填 models_dir 编辑框 → 用户点保存走既有热更链路（零新接线）
```

**缓存目录**（`SHGetKnownFolderPath(FOLDERID_LocalAppData)`）：

```text
%LOCALAPPDATA%\VoiceStick\models\
  sense-voice-int8-2024-07-17\        ← 下载后写入 [local_asr].models_dir
    model.int8.onnx
    tokens.txt
    Qwen3-1.7B-Q4_K_M\Qwen3-1.7B-Q4_K_M.gguf   ← 恰为精修默认档位（见 §6-P3）
```

布局与现有解析**零改动**兼容：`ValidateSenseVoiceModelsDir` 校验 models_dir 直接含两个 ASR 文件；`ResolveLocalRefineModelPath` 默认档位锚 `models_dir/Qwen3-1.7B-Q4_K_M/`。目录名带版本，升级新模型 = 新目录 + config 指过去，旧目录留给用户清理。便携版用户已有 exe 旁 `models/` 则状态 ✓，向导按钮隐藏，互不干扰。

## 4. 数据模型（编译期内置清单，不做 JSON）

```cpp
enum class ModelKind { kAsr, kRefine };

struct ModelFileSpec {
    std::string rel_path;      // 相对 models_dir，如 "Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf"
    std::uint64_t bytes;       // 期望字节数（预检+截断判定）
    std::string sha256;        // 小写 hex
    std::vector<std::string> urls;  // 按优先级回退
};

struct ModelEntrySpec {
    ModelKind kind;
    bool required;             // ASR true / 精修 false（缺省降级规则精修）
    std::vector<ModelFileSpec> files;
};

// model_manifest.cc：GetBundledModelEntries() 返回两entry常量表。
// URL/哈希占位，迭代三上传 OSS 后回填实测值（ModelScope 文件须先核实与本仓
// m0/models 副本逐字节一致，不一致则按源各记哈希）。
```

不做远端清单的理由：模型源变动频率远低于应用发版；编译期常量表免 JSON 解析与版本协商，单测直接构造。

## 5. 接口契约（model_downloader.h）

```cpp
struct DownloadProgress { std::uint64_t downloaded, total; };
using DownloadProgressFn = std::function<void(const DownloadProgress&)>;
using DownloadCancelFn = std::function<bool()>;   // 轮询点：每次读块后

enum class DownloadResult { kOk, kCancelled, kNetworkError, kHashMismatch,
                            kDiskFull, kDiskSpaceInsufficient };
struct DownloadOutcome { DownloadResult result; std::string error; std::string url_used; };

DownloadOutcome ModelDownloader::DownloadFile(
    const ModelFileSpec& spec, const fs::path& dest,
    DownloadProgressFn progress, DownloadCancelFn cancelled);
```

纯函数（core_tests.cc 可测，不碰网络）：

- `ParseModelUrl(url)` → WinHttpCrackUrl 包装，非法 URL 早失败；
- `PlanResume(std::uint64_t part_bytes, std::uint64_t expected)` → kResume / kRestart（.part 超过期望即损坏重下）；
- `InterpretRangeResponse(status, content_length, part_bytes, expected)` → 206 续传 / 200 重下 / 异常中止；
- `FinalizePartFile(.part, dest, digest, expected)` → 哈希匹配原子改名，不匹配删除返回 kHashMismatch；
- `RequiredDiskBytes(entries)` → 向导预检用。

## 6. 关键边界与异常流

1. **磁盘**：下载前对目标盘 `GetDiskFreeSpaceExW` 预检（条目合计 + 64MB 余量）；写盘中 ENOSPC → 删本次 .part、kDiskFull。
2. **哈希不匹配**：删 .part，换下一源重试；所有源耗尽 → 报错列出各源失败原因。防投毒/防损坏的底线，源优先级不影响安全性。
3. **取消**：向导关闭置 cancel 旗标，下载线程当前块完成后退出，.part 保留供续传。
4. **重定向/代理**：WinHTTP 默认跟随重定向（ModelScope resolve 302 到 CDN）与继承系统代理，无需额外处理。
5. **源选择策略**：仅当上一源网络失败或哈希不匹配才回退；不做测速。3 源顺序常量表固定。
6. **并发**：向导运行期间设置页「保存」禁用（模态已天然互斥）；同一时刻仅一个下载线程。
7. **写回 config**：下载完成只改编辑框文本，用户点「保存」才落盘——与设置页现有「保存即生效」语义一致，不新增隐式写配置路径。
8. **失败重试语义**：ASR 条目失败 → 整体失败（本地识别不可用）；仅精修失败 → 成功但状态行明示「精修模型未就绪，仅规则精修」。

## 7. 分发源与成本（迭代三落地）

| 优先级 | 源 | 角色 | 备注 |
|---|---|---|---|
| 1 | 阿里云 OSS `models/` 前缀 | 主源，体验可控 | ossutil 手动上传（CI 无 OSS 凭据，模型不常变）；量大再套 CDN |
| 2 | ModelScope 直链 | 免费分流 + 回退 | sense-voice：pengzhendong 镜像；GGUF：unsloth/Qwen3-1.7B-GGUF；**须先核实与本仓副本哈希一致** |
| 3 | GitHub Release asset | 国际回退 | 上传 `model-pack-<version>.zip` 拆分条目文件，单文件 ≤2GB 约束内 |

另出全量离线 zip（`voicestick-models-full-v1.zip` ≈1.34GB）放 OSS 顶层，供内网/手动分发，文档挂官网 FAQ。存储成本忽略（<1 元/月）；流量成本见 §1。两模型许可证均允许再分发，随包保留原 LICENSE 文本。

## 8. 测试策略（TDD，先红后绿）

- **单元（core_tests.cc，无 I/O）**：清单常量自洽（rel_path/bytes/sha256 非空、urls 非空）；`PlanResume`/`InterpretRangeResponse`/`FinalizePartFile` 全分支；`RequiredDiskBytes`；下载结果到用户文案的映射。
- **集成（仍 core_tests.cc，本机回环）**：进程内起 127.0.0.1 临时端口微型 HTTP 服务（Winsock，真 socket 真文件，支持 Range/206/200/404/慢速），临时目录落盘——覆盖：全量下载成功、断点续传（预置半截 .part）、服务器不支持 Range 回 200 重下、哈希不匹配换源、404 换源、取消、磁盘路径不存在。遵守「不伪造」：全部真实 HTTP 交互，不 mock socket。
- **真机 smoke（可选，需外网）**：`--download-models` 隐藏命令行开关直连真实三源各下一次，校验哈希；无网络环境 SKIP。发布前人工跑一次。

## 9. 实施迭代

1. **迭代一（纯核心，TDD）**：`model_manifest` 常量 + `model_downloader` 纯函数与 WinHTTP 流式实现 + 本地回环集成测试。全绿后提交。
2. **迭代二（UI 接线）**：设置页「下载模型…」按钮（状态 ✗ 且本地提供方选中时可见）→ 模态向导（条目勾选/进度/取消/错误重试）→ 完成回填 models_dir。`localization.cc` 双语新增约 15 条 StringId。真机验证：下载→保存→状态 ✓→按住说话出文本。
3. **迭代三（托管收尾）**：算哈希回填清单 → ossutil 上传 OSS + GitHub Release → 核实 ModelScope 哈希一致性 → 离线 zip + `scripts/` 上传脚本 → 同步 `Doc/Ref/desktop-config.md`、`README` 双语、`CHANGELOG`。

## 10. 风险与开放问题

- ModelScope 第三方 repo 文件被删/换版本 → 哈希校验兜底（表现为该源失败回退），不构成安全问题；发布前核实并在清单里按源记哈希可完全规避。
- OSS bucket/区域选型与费用归属待定（`Doc/Ref/release.md` 记载 CI 已不携带 OSS 凭据，需手动链路）。
- `app_config.h:212` 注释（`refine/qwen3-1.7b-q4_k_m.gguf`）与实现默认档位（`Qwen3-1.7B-Q4_K_M/`）漂移，迭代二顺手修正注释。
- 1.34GB 对机械盘/低配机用户仍重：向导明示体积与「仅下载识别模型（240MB）」选项已覆盖最小路径。
