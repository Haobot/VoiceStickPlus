import Foundation

/// 本地模型分发清单（**编译期内置常量，不做远端清单**——对齐 Windows
/// `model_manifest.cc` 的决策：条目稳定、可离线校验，发新模型走发版）。
///
/// 常量同源锁定（改动须四处同步）：
/// 1. 本文件；2. `desktop/windows/src/model_manifest.cc`；
/// 3. `scripts/pack_local_models.py`；4. `scripts/setup-mac-local-asr.sh`。
/// VoiceStickTests 的清单对拍测试直接解析 pack_local_models.py 常量与本文件比对。
///
/// 分发源与桶布局见 `Doc/Ref/cos-distribution.md`：回退顺序 = 正式域名直出 →
/// myqcloud 直出（**新桶 1329978361**；Windows manifest 里的 1259040144 旧桶
/// 已停用，Swift 版不沿用）→ ModelScope 免费分流 → GitHub Release 海外回退。
public enum ModelManifest {

    // MARK: - 数据模型

    /// 一个下载物（普通文件或 tar.bz2 归档）。
    public struct ArtifactSpec: Equatable {
        /// 下载源（按序回退）。
        public let urls: [String]
        /// 落地文件（direct = 下载物本身；tarball = 解包成员）。
        public let files: [FileSpec]
        /// true = urls 指向 tar.bz2，files 按归档内相对路径（后缀匹配）解出安装。
        public let isArchive: Bool
        /// 归档体积估计（字节；仅用于磁盘预检与 UI 体积文案，进度以 HTTP
        /// Content-Length 为准）。direct 下载物此字段忽略（用 files[0].bytes）。
        public let approxArchiveBytes: Int64

        public init(
            urls: [String], files: [FileSpec], isArchive: Bool, approxArchiveBytes: Int64
        ) {
            self.urls = urls
            self.files = files
            self.isArchive = isArchive
            self.approxArchiveBytes = approxArchiveBytes
        }
    }

    /// 单个落地文件。
    public struct FileSpec: Equatable {
        /// 相对条目安装根目录的目标路径（"/" 分隔）。
        public let relPath: String
        /// 期望字节数（安装后校验）。
        public let bytes: Int64
        /// 期望 SHA-256（小写 hex；安装后校验）。
        public let sha256: String
        /// isArchive 条目：归档内成员路径（后缀匹配定位）；direct 条目忽略。
        public let archiveMember: String

        public init(relPath: String, bytes: Int64, sha256: String, archiveMember: String = "") {
            self.relPath = relPath
            self.bytes = bytes
            self.sha256 = sha256
            self.archiveMember = archiveMember
        }
    }

    /// 一个功能位（下载向导中的一个勾选项）。
    public struct EntrySpec: Equatable {
        public enum Kind: Hashable {
            /// SenseVoice 识别模型（必选）。
            case asr
            /// sherpa-onnx dylib 运行时（必选，macOS 专属——Windows 静态链入无此条目）。
            case runtime
            /// 本地 LLM GGUF（可选；id 如 "qwen2.5-1.5b"，Phase B 填充）。
            case llm(String)
        }

        public let kind: Kind
        /// 必选项（下载向导锁定勾选，不可取消）。
        public let required: Bool
        /// 安装根目录：相对模型根（`~/Library/Application Support/VoiceStick/models`）；
        /// runtime 条目固定，asr 条目实际落在设置里解析出的模型目录（默认即此根下）。
        public let installRoot: String
        public let artifacts: [ArtifactSpec]

        public init(kind: Kind, required: Bool, installRoot: String, artifacts: [ArtifactSpec]) {
            self.kind = kind
            self.required = required
            self.installRoot = installRoot
            self.artifacts = artifacts
        }

        public var totalBytes: Int64 {
            artifacts.reduce(0) { sum, artifact in
                sum + (artifact.isArchive ? artifact.approxArchiveBytes
                                          : artifact.files.reduce(0) { $0 + $1.bytes })
            }
        }
    }

    // MARK: - 分发源

    static let cosPrimaryBase = "https://dl.davenger.cloud/models"
    /// 新桶直连（2026-09-26 起现行；旧桶 1259040144 已停用）。
    static let cosDirectBase =
        "https://voicestick-dl-1329978361.cos.ap-shanghai.myqcloud.com/models"
    static let modelScopeAsrBase =
        "https://modelscope.cn/models/pengzhendong/"
        + "sherpa-onnx-sense-voice-zh-en-ja-ko-yue/resolve/master"
    static let githubModelPackBase =
        "https://github.com/Haobot/VoiceStickPlus/releases/download/model-pack-v1"
    /// sherpa-onnx 官方 release（运行时 tarball 的权威源；COS 镜像待发布侧上传，
    /// 未上传期间 404 快速失败自动回退 GitHub，国内用户需代理——见 D2 发布配套）。
    static let sherpaTarballURL =
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.7/"
        + "sherpa-onnx-v1.13.7-osx-arm64-shared-lib.tar.bz2"
    static let sherpaTarballPath = "models/sherpa-onnx/sherpa-onnx-v1.13.7-osx-arm64-shared-lib.tar.bz2"

    // MARK: - 条目

    /// SenseVoice int8 识别模型（与 Windows 在发同款；字节数/SHA 权威源 =
    /// scripts/pack_local_models.py）。
    static let asrModelBytes: Int64 = 239_233_841
    static let asrModelSHA256 =
        "c71f0ce00bec95b07744e116345e33d8cbbe08cef896382cf907bf4b51a2cd51"
    static let asrTokensBytes: Int64 = 315_894
    static let asrTokensSHA256 =
        "f449eb28dc567533d7fa59be34e2abca8784f771850c78a47fb731a31429a1dc"

    /// sherpa-onnx v1.13.7 osx-arm64 dylib（tarball 内两个成员；SHA 取自本机
    /// setup-mac-local-asr.sh 安装产物的实测哈希，与 GitHub release 资产同源）。
    static let runtimeCapiBytes: Int64 = 4_153_504
    static let runtimeCapiSHA256 =
        "29f43b6d737f75a19c153e94aa16451404a3e29d20c07f462dbbbc8888fcf5bd"
    static let runtimeOrtBytes: Int64 = 28_661_744
    static let runtimeOrtSHA256 =
        "59665a56e6a95118606bebe583efa8a3528362fc1078f69fc27f36def905bb2c"

    /// 全部条目（下载向导按此渲染勾选列表；顺序即展示序）。
    public static let entries: [EntrySpec] = [asrEntry, runtimeEntry]

    static let asrEntry = EntrySpec(
        kind: .asr,
        required: true,
        installRoot: "sense-voice-int8-2024-07-17",
        artifacts: [
            ArtifactSpec(
                urls: [
                    cosPrimaryBase + "/sense-voice-int8-2024-07-17/model.int8.onnx",
                    cosDirectBase + "/sense-voice-int8-2024-07-17/model.int8.onnx",
                    modelScopeAsrBase + "/model.int8.onnx",
                    githubModelPackBase + "/sense-voice-int8-2024-07-17/model.int8.onnx",
                ],
                files: [
                    FileSpec(
                        relPath: "model.int8.onnx", bytes: asrModelBytes,
                        sha256: asrModelSHA256, archiveMember: ""),
                ],
                isArchive: false, approxArchiveBytes: 0),
            ArtifactSpec(
                urls: [
                    cosPrimaryBase + "/sense-voice-int8-2024-07-17/tokens.txt",
                    cosDirectBase + "/sense-voice-int8-2024-07-17/tokens.txt",
                    modelScopeAsrBase + "/tokens.txt",
                    githubModelPackBase + "/sense-voice-int8-2024-07-17/tokens.txt",
                ],
                files: [
                    FileSpec(
                        relPath: "tokens.txt", bytes: asrTokensBytes,
                        sha256: asrTokensSHA256, archiveMember: ""),
                ],
                isArchive: false, approxArchiveBytes: 0),
        ])

    static let runtimeEntry = EntrySpec(
        kind: .runtime,
        required: true,
        installRoot: "sherpa-onnx",
        artifacts: [
            ArtifactSpec(
                urls: [
                    cosPrimaryBase + "/sherpa-onnx/sherpa-onnx-v1.13.7-osx-arm64-shared-lib.tar.bz2",
                    cosDirectBase + "/sherpa-onnx/sherpa-onnx-v1.13.7-osx-arm64-shared-lib.tar.bz2",
                    sherpaTarballURL,
                ],
                files: [
                    FileSpec(
                        relPath: "lib/libsherpa-onnx-c-api.dylib",
                        bytes: runtimeCapiBytes, sha256: runtimeCapiSHA256,
                        archiveMember: "lib/libsherpa-onnx-c-api.dylib"),
                    FileSpec(
                        relPath: "lib/libonnxruntime.dylib",
                        bytes: runtimeOrtBytes, sha256: runtimeOrtSHA256,
                        archiveMember: "lib/libonnxruntime.dylib"),
                ],
                // 解包后两 dylib 合计 ~31MB；tar.bz2 压缩后以此为体积文案估计。
                isArchive: true, approxArchiveBytes: 31_000_000),
        ])

    // MARK: - 自洽校验（对齐 Windows ModelEntriesWellFormed）

    /// 清单结构自检：路径非空且无反斜杠、字节数非零、SHA 为 64 位小写 hex、
    /// 每个下载物至少一源。VoiceStickTests 调用；ture = 通过。
    public static func entriesWellFormed(_ entries: [EntrySpec]) -> Bool {
        if entries.isEmpty { return false }
        for entry in entries {
            if entry.artifacts.isEmpty { return false }
            for artifact in entry.artifacts {
                if artifact.urls.isEmpty { return false }
                for url in artifact.urls
                where !url.hasPrefix("http://") && !url.hasPrefix("https://") {
                    return false
                }
                if artifact.files.isEmpty { return false }
                for file in artifact.files {
                    if file.relPath.isEmpty || file.relPath.contains("\\") { return false }
                    if file.bytes <= 0 { return false }
                    if !isLowerHex64(file.sha256) { return false }
                }
            }
        }
        return true
    }

    static func isLowerHex64(_ text: String) -> Bool {
        guard text.count == 64 else { return false }
        return text.allSatisfy { c in
            (c.isASCII && c.isNumber) || ("a"..."f").contains(c)
        }
    }
}
