import CryptoKit
import Foundation

/// 本地模型下载器（对齐 Windows `model_downloader.cc` 的语义）：
/// - 多源回退：逐源尝试，失败/校验不过自动换下一源；
/// - 断点续传：落盘 `<dest>.part`，重试带 `Range` 头续传（206 追加、200 重下、
///   416 按已就位候选走校验）；
/// - 全量 SHA-256：direct 文件校验下载物本身；归档校验解包成员
///  （归档整体哈希不锁定，成员哈希即完整性）；
/// - 原子落地：校验通过后才 `rename` 到目标路径，失败不留半成品
///  （`.part` 保留供续传，取消也保留——与 Windows「已下载部分会保留」同口径）；
/// - 磁盘预检：所需字节 + 64MB 余量对比目标卷可用空间。
///
/// 线程模型：会话在后台串行执行（一次一个下载物）；progress 在 worker 队列回调
/// （UI 侧自行 hop 主线程）；completion 一律主线程回调。
///
/// URLSession 的 delegate 是**会话级**的——每次下载尝试独立建一个以 StreamSink
/// 为 delegate 的会话，结束即 invalidate（会话数个位数，无复用负担）。
public final class ModelDownloader {

    // MARK: - 错误

    public enum DownloadError: LocalizedError, Equatable {
        /// 用户取消（.part 保留，下次续传）。
        case canceled
        /// 磁盘空间不足（need/free 为字节）。
        case diskSpace(needed: Int64, available: Int64)
        /// 所有源都失败（附最后错误描述）。
        case allSourcesFailed(item: String, lastError: String)
        /// 归档解包/成员缺失。
        case archiveBroken(item: String)
        /// 安装（移动/建目录/校验）失败。
        case installFailed(String)

        public var errorDescription: String? {
            switch self {
            case .canceled:
                return "已取消。已下载部分会保留，下次继续（断点续传）。"
            case .diskSpace(let needed, let available):
                return "磁盘空间不足：需要约 \(ByteSizeFormatter.string(needed))，"
                    + "可用 \(ByteSizeFormatter.string(available))"
            case .allSourcesFailed(let item, let lastError):
                return "下载失败（已尝试全部源）：\(item) — \(lastError)"
            case .archiveBroken(let item):
                return "下载的归档损坏或缺少预期文件：\(item)"
            case .installFailed(let detail):
                return "安装模型文件失败：\(detail)"
            }
        }
    }

    // MARK: - 进度

    /// 会话级进度：当前下载物 + 全会话累计（UI 聚合条用）。
    public struct Progress {
        /// 当前下载物标签（文件名或归档名）。
        public let currentItem: String
        /// 当前下载物已接收字节（含续传起点）。
        public let receivedBytes: Int64
        /// 当前下载物总字节；0 = 未知（HTTP 无 Content-Length）。
        public let totalBytes: Int64
        /// 全会话已完成字节数（跳过项计入）。
        public let completedBytes: Int64
        /// 全会话计划字节数（估算）。
        public let plannedBytes: Int64

        /// 聚合完成比例（0...1，planned 未知时为 0）。
        public var fraction: Double {
            guard plannedBytes > 0 else { return 0 }
            let current = totalBytes > 0 ? min(receivedBytes, totalBytes) : receivedBytes
            let value = Double(completedBytes + current) / Double(plannedBytes)
            return min(max(value, 0), 1)
        }
    }

    /// 一个下载计划项：清单条目 + 实际安装根目录（asr 条目指向设置解析出的模型目录，
    /// runtime 条目指向固定 Application Support 路径——由调用方决定）。
    public struct PlanItem {
        public let entry: ModelManifest.EntrySpec
        public let installRoot: URL

        public init(entry: ModelManifest.EntrySpec, installRoot: URL) {
            self.entry = entry
            self.installRoot = installRoot
        }
    }

    // MARK: - 状态

    private let stateLock = NSLock()
    private var canceled = false
    private var running = false
    private var activeSession: URLSession?

    public init() {}

    public func cancel() {
        stateLock.lock()
        canceled = true
        stateLock.unlock()
        activeSession?.invalidateAndCancel()
    }

    private var isCanceled: Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return canceled
    }

    private func setCurrentSession(_ session: URLSession?) {
        stateLock.lock()
        activeSession = session
        stateLock.unlock()
    }

    // MARK: - 入口

    /// 执行下载计划。已就位（size+SHA 校验通过）的下载物自动跳过；全部就位
    /// 直接成功返回（对齐 Windows「已存在即秒完成」）。
    public func download(
        plan: [PlanItem],
        progress: @escaping (Progress) -> Void,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        stateLock.lock()
        guard !running else {
            stateLock.unlock()
            DispatchQueue.main.async {
                completion(.failure(DownloadError.installFailed("下载会话已在进行")))
            }
            return
        }
        running = true
        canceled = false
        stateLock.unlock()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            let result = self.run(plan: plan, progress: progress)
            self.stateLock.lock()
            self.running = false
            self.stateLock.unlock()
            DispatchQueue.main.async { completion(result) }
        }
    }

    // MARK: - 会话主流程（worker 队列）

    private func run(plan: [PlanItem], progress: @escaping (Progress) -> Void) -> Result<Void, Error> {
        // 0) 磁盘预检：未就位下载物的估算字节 + 64MB 余量（Windows 同口径）。
        var needed: Int64 = 0
        for item in plan {
            for artifact in item.entry.artifacts
            where !Self.artifactPresent(artifact, installRoot: item.installRoot) {
                needed += artifact.estimatedBytes
            }
        }
        if needed > 0, let volumePath = plan.first?.installRoot.path {
            let free = Self.volumeAvailableBytes(volumePath: volumePath)
            if free >= 0, free < needed + 64 * 1024 * 1024 {
                return .failure(DownloadError.diskSpace(needed: needed, available: free))
            }
        }

        let plannedBytes = plan.reduce(Int64(0)) { sum, item in
            sum + item.entry.artifacts.reduce(0) { $0 + $1.estimatedBytes }
        }
        var completedBytes: Int64 = 0

        for item in plan {
            for artifact in item.entry.artifacts {
                if isCanceled { return .failure(DownloadError.canceled) }
                let label = artifact.label
                if Self.artifactPresent(artifact, installRoot: item.installRoot) {
                    completedBytes += artifact.estimatedBytes
                    emitProgress(progress, label: label, received: artifact.estimatedBytes,
                                 total: artifact.estimatedBytes,
                                 completed: completedBytes, planned: plannedBytes)
                    continue
                }
                do {
                    try downloadArtifact(artifact, installRoot: item.installRoot) {
                        [weak self] received, total in
                        guard let self, !self.isCanceled else { return }
                        emitProgress(progress, label: label, received: received, total: total,
                                     completed: completedBytes, planned: plannedBytes)
                    }
                    completedBytes += artifact.estimatedBytes
                    emitProgress(progress, label: label, received: artifact.estimatedBytes,
                                 total: artifact.estimatedBytes,
                                 completed: completedBytes, planned: plannedBytes)
                } catch let error as DownloadError {
                    return .failure(error)
                } catch {
                    return .failure(DownloadError.allSourcesFailed(
                        item: label, lastError: error.localizedDescription))
                }
            }
        }
        return .success(())
    }

    private func emitProgress(
        _ progress: (Progress) -> Void, label: String,
        received: Int64, total: Int64, completed: Int64, planned: Int64
    ) {
        progress(Progress(
            currentItem: label, receivedBytes: received, totalBytes: total,
            completedBytes: completed, plannedBytes: planned))
    }

    // MARK: - 单下载物：多源回退（worker 队列）

    private func downloadArtifact(
        _ artifact: ModelManifest.ArtifactSpec,
        installRoot: URL,
        onProgress: @escaping (_ received: Int64, _ total: Int64) -> Void
    ) throws {
        let itemLabel = artifact.label
        var lastError = "无可用源"
        for urlString in artifact.urls {
            if isCanceled { throw DownloadError.canceled }
            do {
                try fetchArtifactOnce(
                    artifact, from: urlString, installRoot: installRoot,
                    onProgress: onProgress)
                return
            } catch DownloadError.canceled {
                throw DownloadError.canceled
            } catch {
                lastError = error.localizedDescription
                NSLog("ModelDownload source failed (%@): %@ — trying next",
                      urlString, lastError)
            }
        }
        throw DownloadError.allSourcesFailed(item: itemLabel, lastError: lastError)
    }

    /// 单源一次尝试（含续传、校验、安装）。失败时清理损坏的 .part 后换源。
    private func fetchArtifactOnce(
        _ artifact: ModelManifest.ArtifactSpec,
        from urlString: String,
        installRoot: URL,
        onProgress: @escaping (_ received: Int64, _ total: Int64) -> Void
    ) throws {
        guard let url = URL(string: urlString) else {
            throw DownloadError.installFailed("无效 URL: \(urlString)")
        }

        if artifact.isArchive {
            // 归档：下载到临时目录，解包后按成员校验安装。
            let workDir = FileManager.default.temporaryDirectory
                .appendingPathComponent("voicestick-model-dl-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: workDir) }
            let archivePart = workDir.appendingPathComponent("archive.tar.bz2.part")
            let archivePath = workDir.appendingPathComponent("archive.tar.bz2")
            try streamDownload(
                url: url, toFile: archivePart, moveTo: archivePath,
                expectedBytes: nil, sha256: "", onProgress: onProgress)
            if isCanceled { throw DownloadError.canceled }
            try Self.extractAndInstallArchive(
                artifact, archivePath: archivePath, workDir: workDir, installRoot: installRoot)
        } else {
            guard let file = artifact.files.first else {
                throw DownloadError.installFailed("清单条目无文件")
            }
            let dest = installRoot.appendingPathComponent(file.relPath)
            let partURL = Self.partURL(for: dest)
            try FileManager.default.createDirectory(
                at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
            try streamDownload(
                url: url, toFile: partURL, moveTo: dest,
                expectedBytes: file.bytes, sha256: file.sha256, onProgress: onProgress)
        }
    }

    // MARK: - 流式下载（Range 续传 + SHA + 原子改名）

    /// 从 url 流式下载到 partURL（存在则 Range 续传），完成后对 part 做
    /// size/SHA 校验（expectedBytes/sha256 非空时——归档传 nil/空跳过，完整性由
    /// 成员校验保证），再原子改名到 dest。
    private func streamDownload(
        url: URL,
        toFile partURL: URL,
        moveTo dest: URL,
        expectedBytes: Int64?,
        sha256: String,
        onProgress: @escaping (_ received: Int64, _ total: Int64) -> Void
    ) throws {
        let fm = FileManager.default
        var resumeFrom: Int64 = 0
        if let attrs = try? fm.attributesOfItem(atPath: partURL.path),
           let size = attrs[.size] as? Int64, size > 0 {
            resumeFrom = size
        }

        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData)
        request.timeoutInterval = 60
        if resumeFrom > 0 {
            request.setValue("bytes=\(resumeFrom)-", forHTTPHeaderField: "Range")
        }

        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 60
        config.timeoutIntervalForResource = 3600
        let delegateQueue = OperationQueue()
        delegateQueue.maxConcurrentOperationCount = 1
        let sink = StreamSink(
            partURL: partURL, resumeFrom: resumeFrom,
            canceled: { [weak self] in self?.isCanceled ?? true },
            onProgress: onProgress)
        let session = URLSession(configuration: config, delegate: sink, delegateQueue: delegateQueue)
        setCurrentSession(session)
        defer { setCurrentSession(nil) }

        let task = session.dataTask(with: request)
        sink.task = task
        task.resume()
        let outcome = sink.waitUntilDone()
        session.finishTasksAndInvalidate()

        switch outcome {
        case .failed(let error):
            // 网络/HTTP 错误：保留 .part 供续传（取消亦如此——Windows 同口径）。
            throw error
        case .succeeded:
            break
        }
        if isCanceled { throw DownloadError.canceled }

        // 校验（对 part，未落地前）+ 原子改名。
        if let expectedBytes {
            let actual = (try? fm.attributesOfItem(atPath: partURL.path)[.size] as? Int64) ?? -1
            guard actual == expectedBytes else {
                try? fm.removeItem(at: partURL)   // 尺寸不对：part 不可信，清掉换源
                throw DownloadError.installFailed(
                    "大小不匹配: \(partURL.lastPathComponent)（期望 \(expectedBytes)，实际 \(actual)）")
            }
        }
        if !sha256.isEmpty {
            let digest = try Self.sha256File(at: partURL.path)
            guard digest == sha256.lowercased() else {
                try? fm.removeItem(at: partURL)   // 校验不过：part 可能被污染，清掉
                throw DownloadError.installFailed("SHA-256 校验失败: \(partURL.lastPathComponent)")
            }
        }
        if fm.fileExists(atPath: dest.path) {
            try fm.removeItem(at: dest)   // 就地升级：先删旧文件再改名
        }
        try fm.moveItem(at: partURL, to: dest)
    }

    // MARK: - 归档解包安装

    /// tar xjf 解包（系统 /usr/bin/tar，bsdtar 原生支持 bzip2）；成员按
    /// archiveMember 后缀匹配定位（归档顶层目录名不锁定），size+SHA 校验后
    /// 经 .part 原子安装到 installRoot。
    static func extractAndInstallArchive(
        _ artifact: ModelManifest.ArtifactSpec,
        archivePath: URL,
        workDir: URL,
        installRoot: URL
    ) throws {
        let extractDir = workDir.appendingPathComponent("extracted")
        try FileManager.default.createDirectory(at: extractDir, withIntermediateDirectories: true)
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        process.arguments = ["-xjf", archivePath.path, "-C", extractDir.path]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            throw DownloadError.archiveBroken(item: archivePath.lastPathComponent)
        }
        guard process.terminationStatus == 0 else {
            throw DownloadError.archiveBroken(item: archivePath.lastPathComponent)
        }

        func locateMember(_ member: String) -> URL? {
            // 后缀匹配：member = "lib/libsherpa-onnx-c-api.dylib"，归档内实际路径
            // 前缀多一层顶层目录（sherpa-onnx-v1.13.7-osx-arm64-shared-lib/）。
            let fm = FileManager.default
            let parts = member.split(separator: "/").map(String.init)
            guard let enumerator = fm.enumerator(at: extractDir, includingPropertiesForKeys: nil) else {
                return nil
            }
            for case let fileURL as URL in enumerator {
                let pathParts = fileURL.pathComponents
                guard pathParts.count >= parts.count else { continue }
                if pathParts.suffix(parts.count) == parts { return fileURL }
            }
            return nil
        }

        for file in artifact.files {
            guard let memberURL = locateMember(file.archiveMember) else {
                throw DownloadError.archiveBroken(item: file.archiveMember)
            }
            let dest = installRoot.appendingPathComponent(file.relPath)
            try FileManager.default.createDirectory(
                at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
            let partURL = Self.partURL(for: dest)
            if FileManager.default.fileExists(atPath: partURL.path) {
                try FileManager.default.removeItem(at: partURL)
            }
            try FileManager.default.copyItem(at: memberURL, to: partURL)
            guard Self.fileMatches(path: partURL.path, bytes: file.bytes, sha256: file.sha256) else {
                try? FileManager.default.removeItem(at: partURL)
                throw DownloadError.installFailed("归档成员校验失败: \(file.relPath)")
            }
            if FileManager.default.fileExists(atPath: dest.path) {
                try FileManager.default.removeItem(at: dest)
            }
            try FileManager.default.moveItem(at: partURL, to: dest)
        }
    }

    // MARK: - 就位检测与哈希

    static func partURL(for dest: URL) -> URL {
        dest.appendingPathExtension("part")
    }

    /// 下载物是否已就位（size+SHA 全量校验；worker 线程调用）。
    static func artifactPresent(
        _ artifact: ModelManifest.ArtifactSpec, installRoot: URL
    ) -> Bool {
        artifact.files.allSatisfy { file in
            let dest = installRoot.appendingPathComponent(file.relPath).path
            return fileMatches(path: dest, bytes: file.bytes, sha256: file.sha256)
        }
    }

    /// size 一致且 SHA-256 匹配（文件不存在返回 false）。
    static func fileMatches(path: String, bytes: Int64, sha256: String) -> Bool {
        let fm = FileManager.default
        guard let attrs = try? fm.attributesOfItem(atPath: path),
              let size = attrs[.size] as? Int64, size == bytes else { return false }
        return (try? sha256File(at: path)) == sha256.lowercased()
    }

    /// 流式 SHA-256（1MiB 分块，避免 239MB 模型整读进内存）。
    static func sha256File(at path: String) throws -> String {
        let handle: FileHandle
        do {
            handle = try FileHandle(forReadingFrom: URL(fileURLWithPath: path))
        } catch {
            throw DownloadError.installFailed("无法读取 \(path)")
        }
        defer { try? handle.close() }
        var hasher = SHA256()
        let chunkSize = 1 << 20
        while true {
            let chunk = try handle.read(upToCount: chunkSize) ?? Data()
            if chunk.isEmpty { break }
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func volumeAvailableBytes(volumePath: String) -> Int64 {
        let url = URL(fileURLWithPath: volumePath)
        if let values = try? url.resourceValues(forKeys: [.volumeAvailableCapacityKey]),
           let capacity = values.volumeAvailableCapacity {
            return Int64(capacity)
        }
        return -1
    }
}

// MARK: - 流式接收（会话级 delegate + 信号量驱动，worker 线程同步等待）

/// 单次下载尝试的接收器：处理 206/200/416 与 Range 语义，流式写 .part。
private final class StreamSink: NSObject, URLSessionDataDelegate {

    enum Outcome {
        case succeeded
        case failed(Error)
    }

    private let partURL: URL
    private let canceled: () -> Bool
    private let onProgress: (_ received: Int64, _ total: Int64) -> Void
    private let semaphore = DispatchSemaphore(value: 0)
    private var outcome: Outcome = .succeeded

    private var fileHandle: FileHandle?
    private var received: Int64 = 0
    private var total: Int64 = 0
    private var appendMode = false
    private var lastProgressAt = Date.distantPast

    weak var task: URLSessionDataTask?

    init(partURL: URL, resumeFrom: Int64, canceled: @escaping () -> Bool,
         onProgress: @escaping (_ received: Int64, _ total: Int64) -> Void) {
        self.partURL = partURL
        self.canceled = canceled
        self.onProgress = onProgress
        super.init()
        self.received = resumeFrom
    }

    func waitUntilDone() -> Outcome {
        semaphore.wait()
        try? fileHandle?.close()
        fileHandle = nil
        return outcome
    }

    private func finish(_ result: Outcome) {
        outcome = result
        semaphore.signal()
    }

    // MARK: 响应决策（Range 语义）

    func urlSession(
        _ session: URLSession, dataTask: URLSessionDataTask,
        didReceive response: URLResponse,
        completionHandler: @escaping (URLSession.ResponseDisposition) -> Void
    ) {
        guard let http = response as? HTTPURLResponse else {
            // file:// 等非 HTTP 场景（单测夹具）：按整包处理（等价 200 路径）。
            appendMode = false
            received = 0
            total = Int64(max(response.expectedContentLength, 0))
            completionHandler(.allow)
            return
        }
        switch http.statusCode {
        case 206:
            // 续传命中：Content-Range 尾段是远程总长。
            appendMode = true
            if let range = http.value(forHTTPHeaderField: "Content-Range"),
               let totalPart = range.split(separator: "/").last,
               let remoteTotal = Int64(totalPart) {
                total = remoteTotal
            }
            completionHandler(.allow)
        case 200:
            // 源不支持 Range（或无 .part）：整包重下，丢弃旧 part。
            appendMode = false
            received = 0
            if let expected = http.value(forHTTPHeaderField: "Content-Length"),
               let length = Int64(expected) {
                total = length
            }
            completionHandler(.allow)
        case 416:
            // 请求区间不可满足：part 可能已完整——交上层校验（size+SHA 不匹配
            // 会清理换源）；part 为空则直接按整包重下失败处理。
            completionHandler(.cancel)
            let partSize = (try? FileManager.default.attributesOfItem(
                atPath: partURL.path)[.size] as? Int64) ?? -1
            if partSize > 0 {
                finish(.succeeded)
            } else {
                finish(.failed(ModelDownloader.DownloadError.installFailed("源不支持续传")))
            }
        default:
            completionHandler(.cancel)
            finish(.failed(ModelDownloader.DownloadError.installFailed("HTTP \(http.statusCode)")))
        }
    }

    func urlSession(
        _ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data
    ) {
        if canceled() {
            task?.cancel()
            finish(.failed(ModelDownloader.DownloadError.canceled))
            return
        }
        do {
            if fileHandle == nil {
                let fm = FileManager.default
                if !appendMode, fm.fileExists(atPath: partURL.path) {
                    try fm.removeItem(atPath: partURL.path)   // 整包重下：清旧 part
                }
                if !fm.fileExists(atPath: partURL.path) {
                    fm.createFile(atPath: partURL.path, contents: nil)
                }
                fileHandle = try FileHandle(forWritingTo: partURL)
                if appendMode {
                    _ = try fileHandle?.seekToEnd()
                } else {
                    try fileHandle?.truncate(atOffset: 0)
                }
            }
            try fileHandle?.write(contentsOf: data)
            received += Int64(data.count)
            // 节流：进度回调不超过 10Hz（大文件下避免 UI 洪泛）。
            let now = Date()
            if now.timeIntervalSince(lastProgressAt) > 0.1 {
                lastProgressAt = now
                onProgress(received, total)
            }
        } catch {
            task?.cancel()
            finish(.failed(error))
        }
    }

    func urlSession(
        _ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?
    ) {
        if let error {
            finish(.failed(error))
        } else {
            onProgress(received, max(total, received))
            finish(.succeeded)
        }
    }
}

// MARK: - 下载物辅助

extension ModelManifest.ArtifactSpec {

    /// 进度/日志展示标签。
    var label: String {
        isArchive ? "sherpa-onnx 运行时" : (files.first?.relPath ?? "?")
    }

    /// 体积估算（磁盘预检与聚合进度用）。
    var estimatedBytes: Int64 {
        isArchive ? approxArchiveBytes : files.reduce(0) { $0 + $1.bytes }
    }
}

/// 字节数格式化（下载器错误文案用；UI 侧用自己的 ByteCountFormatter）。
enum ByteSizeFormatter {
    static func string(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .binary
        return formatter.string(fromByteCount: bytes)
    }
}
