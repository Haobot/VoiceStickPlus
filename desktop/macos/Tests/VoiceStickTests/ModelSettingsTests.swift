import Foundation
@testable import VoiceStickCore

/// 本地模型设置/下载 Phase A 单测：
/// 1. Provider 下拉映射纯函数（对齐 Windows provider_combo 语义）；
/// 2. 清单自洽 + 与 scripts/pack_local_models.py 常量对拍（第 4 处同步点用测试锁死）；
/// 3. 下载器纯函数（SHA/就位检测）；
/// 4. 下载器真实链路（file:// 夹具走同一代码路径：多源回退/SHA 校验/原子安装/
///    归档解包——file:// 无 Range 语义，206 续传路径留真机验收）。
func runModelSettingsTests() {
    testProviderPopupMapping()
    testManifestWellFormedAndBucketGuard()
    testManifestSyncWithPythonPack()
    testDownloaderHelpers()
    testDownloaderDirectFileMultiSourceFallback()
    testDownloaderChecksumMismatchCleansPart()
    testDownloaderArchiveInstall()
}

// MARK: - Provider 下拉映射

private func testProviderPopupMapping() {
    // 本地项恒为末位（含/不含老配置 Cloud 前插项两种形态）。
    check(ProviderPopupMapping.isLocal(index: 2, itemCount: 3), "PMM.isLocal.lastNoLegacy")
    check(ProviderPopupMapping.isLocal(index: 3, itemCount: 4), "PMM.isLocal.lastWithLegacy")
    check(!ProviderPopupMapping.isLocal(index: 1, itemCount: 3), "PMM.isLocal.middle")
    check(!ProviderPopupMapping.isLocal(index: -1, itemCount: 3), "PMM.isLocal.negative")
    check(!ProviderPopupMapping.isLocal(index: 3, itemCount: 3), "PMM.isLocal.outOfBounds")
    check(!ProviderPopupMapping.isLocal(index: 0, itemCount: 0), "PMM.isLocal.empty")

    // 无老配置：0=volcengine 1=tencent 2=local(nil)。
    checkEqual(ProviderPopupMapping.cloudProvider(at: 0, itemCount: 3, hasCloudLegacy: false),
               .volcengine, "PMM.cloud.noLegacy0")
    checkEqual(ProviderPopupMapping.cloudProvider(at: 1, itemCount: 3, hasCloudLegacy: false),
               .tencent, "PMM.cloud.noLegacy1")
    checkNil(ProviderPopupMapping.cloudProvider(at: 2, itemCount: 3, hasCloudLegacy: false),
             "PMM.cloud.noLegacyLocalIsNil")

    // 含老配置：0=cloud 1=volcengine 2=tencent 3=local(nil)。
    checkEqual(ProviderPopupMapping.cloudProvider(at: 0, itemCount: 4, hasCloudLegacy: true),
               .voiceStickCloud, "PMM.cloud.legacy0")
    checkEqual(ProviderPopupMapping.cloudProvider(at: 1, itemCount: 4, hasCloudLegacy: true),
               .volcengine, "PMM.cloud.legacy1")
    checkEqual(ProviderPopupMapping.cloudProvider(at: 2, itemCount: 4, hasCloudLegacy: true),
               .tencent, "PMM.cloud.legacy2")
    checkNil(ProviderPopupMapping.cloudProvider(at: 3, itemCount: 4, hasCloudLegacy: true),
             "PMM.cloud.legacyLocalIsNil")

    // indexOf 往返：云端枚举 ↔ 下拉索引。
    checkEqual(ProviderPopupMapping.indexOf(.volcengine, itemCount: 3, hasCloudLegacy: false), 0,
               "PMM.indexOf.volcNoLegacy")
    checkEqual(ProviderPopupMapping.indexOf(.tencent, itemCount: 3, hasCloudLegacy: false), 1,
               "PMM.indexOf.tencent")
    checkEqual(ProviderPopupMapping.indexOf(.voiceStickCloud, itemCount: 4, hasCloudLegacy: true), 0,
               "PMM.indexOf.cloudLegacy")
    checkNil(ProviderPopupMapping.indexOf(.voiceStickCloud, itemCount: 3, hasCloudLegacy: false),
             "PMM.indexOf.cloudWithoutLegacyIsNil")

    check(ProviderPopupMapping.needsCloudLegacyItem(currentProvider: .voiceStickCloud),
          "PMM.needsLegacy.cloud")
    check(!ProviderPopupMapping.needsCloudLegacyItem(currentProvider: .tencent),
          "PMM.needsLegacy.tencent")
}

// MARK: - 清单自洽 + 旧桶防御

private func testManifestWellFormedAndBucketGuard() {
    check(ModelManifest.entriesWellFormed(ModelManifest.entries),
          "Manifest.entriesWellFormed")

    // 旧桶 1259040144 已停用（Windows manifest 仍残留）：Swift 版任何 URL 不得引用。
    for entry in ModelManifest.entries {
        for artifact in entry.artifacts {
            for url in artifact.urls {
                check(!url.contains("1259040144"), "Manifest.noStaleBucket \(url)")
            }
        }
    }

    // 必选项与安装根。
    let kinds = ModelManifest.entries.map { $0.kind }
    check(kinds.contains(.asr), "Manifest.hasAsrEntry")
    check(kinds.contains(.runtime), "Manifest.hasRuntimeEntry")
    // asr/runtime 必选、llm 档可选（下载向导勾选语义）。
    check(ModelManifest.entries.filter { $0.kind == .asr || $0.kind == .runtime }
        .allSatisfy { $0.required }, "Manifest.requiredSet")
    check(ModelManifest.entries.filter { $0.kind != .asr && $0.kind != .runtime }
        .allSatisfy { !$0.required }, "Manifest.llmOptional")

    // dylib 成员路径与引擎搜索序的安装位一致（lib/ 前缀）。
    let runtime = ModelManifest.entries.first { $0.kind == .runtime }
    if let runtime {
        let relPaths = runtime.artifacts.flatMap { $0.files.map { $0.relPath } }
        check(relPaths.contains("lib/libsherpa-onnx-c-api.dylib"), "Manifest.runtimeCapiPath")
        check(relPaths.contains("lib/libonnxruntime.dylib"), "Manifest.runtimeOrtPath")
    } else {
        check(false, "Manifest.runtimeEntryExists")
    }
}

/// 与 scripts/pack_local_models.py 的 MODEL_FILES 常量对拍（ASR 两文件；
/// dylib 为 macOS 专属条目不在 py 清单内，GGUF 属 Phase B 后续同步）。
private func testManifestSyncWithPythonPack() {
    let testFile = URL(fileURLWithPath: #filePath)  // .../Tests/VoiceStickTests/ModelSettingsTests.swift
    let repoRoot = testFile.deletingLastPathComponent()   // Tests/VoiceStickTests
        .deletingLastPathComponent()                      // Tests
        .deletingLastPathComponent()                      // macos
        .deletingLastPathComponent()                      // desktop
        .deletingLastPathComponent()                      // 仓库根
    let pyPath = repoRoot.appendingPathComponent("scripts/pack_local_models.py")
    guard let source = try? String(contentsOf: pyPath, encoding: .utf8) else {
        check(false, "Manifest.pyPackReadable(\(pyPath.path))")
        return
    }
    // 元组格式："rel/path",\n        bytes,\n        "sha",
    let pattern = #""([^\"]+)",\s*(\d+),\s*"([0-9a-f]{64})""#
    guard let regex = try? NSRegularExpression(pattern: pattern) else {
        check(false, "Manifest.pyPackRegex")
        return
    }
    var byRelPath: [String: (bytes: Int64, sha: String)] = [:]
    let ns = source as NSString
    for match in regex.matches(in: source, range: NSRange(location: 0, length: ns.length)) {
        let rel = ns.substring(with: match.range(at: 1))
        let bytes = Int64(ns.substring(with: match.range(at: 2))) ?? 0
        let sha = ns.substring(with: match.range(at: 3))
        byRelPath[rel] = (bytes, sha)
    }
    check(byRelPath.count >= 3, "Manifest.pyPackParsed \(byRelPath.count) entries")

    if let model = byRelPath["sense-voice-int8-2024-07-17/model.int8.onnx"] {
        checkEqual(ModelManifest.asrModelBytes, model.bytes, "Manifest.syncModelBytes")
        checkEqual(ModelManifest.asrModelSHA256, model.sha, "Manifest.syncModelSHA")
    } else {
        check(false, "Manifest.pyPackHasModelEntry")
    }
    if let tokens = byRelPath["sense-voice-int8-2024-07-17/tokens.txt"] {
        checkEqual(ModelManifest.asrTokensBytes, tokens.bytes, "Manifest.syncTokensBytes")
        checkEqual(ModelManifest.asrTokensSHA256, tokens.sha, "Manifest.syncTokensSHA")
    } else {
        check(false, "Manifest.pyPackHasTokensEntry")
    }
}

// MARK: - 下载器纯函数

private func testDownloaderHelpers() {
    let dir = FileManager.default.temporaryDirectory
        .appendingPathComponent("ms-tests-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }

    let payload = Data("VoiceStick model download test 2026-10-29".utf8)
    let file = dir.appendingPathComponent("fixture.bin")
    try? payload.write(to: file)

    let sha = (try? ModelDownloader.sha256File(at: file.path)) ?? ""
    check(sha.count == 64, "DL.sha256File.length")
    check(ModelDownloader.fileMatches(
        path: file.path, bytes: Int64(payload.count), sha256: sha),
        "DL.fileMatches.positive")
    check(!ModelDownloader.fileMatches(
        path: file.path, bytes: Int64(payload.count) + 1, sha256: sha),
        "DL.fileMatches.sizeMismatch")
    check(!ModelDownloader.fileMatches(
        path: file.path, bytes: Int64(payload.count), sha256: String(repeating: "0", count: 64)),
        "DL.fileMatches.shaMismatch")
    check(!ModelDownloader.fileMatches(
        path: dir.appendingPathComponent("absent.bin").path,
        bytes: 1, sha256: sha),
        "DL.fileMatches.absent")
}

// MARK: - 下载器真实链路（file:// 夹具）

/// 构造 direct 条目：bad URL（不存在）在前验证多源回退，good file:// 在后。
private func makeDirectEntry(
    relPath: String, payload: Data, installRoot: URL, badFirst: Bool, shaOverride: String? = nil
) throws -> (entry: ModelManifest.EntrySpec, fixture: URL, sha: String) {
    let fixtureName = "fixtures-\(UUID().uuidString)-"
        + (relPath.components(separatedBy: "/").last ?? "file")
    let fixture = installRoot.deletingLastPathComponent()
        .appendingPathComponent(fixtureName)
    try payload.write(to: fixture)
    let computed = try ModelDownloader.sha256File(at: fixture.path)
    let sha = shaOverride ?? computed
    let good = fixture.absoluteURL.absoluteString
    let bad = fixture.deletingLastPathComponent()
        .appendingPathComponent("definitely-absent-\(UUID().uuidString)").absoluteURL.absoluteString
    let urls = badFirst ? [bad, good] : [good]
    let artifact = ModelManifest.ArtifactSpec(
        urls: urls,
        files: [ModelManifest.FileSpec(
            relPath: relPath, bytes: Int64(payload.count), sha256: sha)],
        isArchive: false, approxArchiveBytes: 0)
    let entry = ModelManifest.EntrySpec(
        kind: .llm("test-fixture"), required: true,
        installRoot: ".", artifacts: [artifact])
    return (entry, fixture, sha)
}

/// 同步跑一次下载：泵主 RunLoop 等待（completion 回调在主线程派发，直接
/// semaphore.wait 会死锁主线程）。
private func runDownload(
    _ plan: [ModelDownloader.PlanItem], progress: @escaping (ModelDownloader.Progress) -> Void = { _ in }
) -> Result<Void, Error> {
    let downloader = ModelDownloader()
    let semaphore = DispatchSemaphore(value: 0)
    var result: Result<Void, Error> = .failure(
        ModelDownloader.DownloadError.installFailed("未回调"))
    downloader.download(plan: plan, progress: progress) {
        result = $0
        semaphore.signal()
    }
    while semaphore.wait(timeout: .now()) == .timedOut {
        RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
    }
    return result
}

private func testDownloaderDirectFileMultiSourceFallback() {
    let root = FileManager.default.temporaryDirectory
        .appendingPathComponent("ms-dl-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }

    let payload = Data("sensevoice-fixture-payload-0123456789".utf8)
    let made = try? makeDirectEntry(
        relPath: "sub/model.int8.onnx", payload: payload, installRoot: root, badFirst: true)
    guard let made else {
        check(false, "DL.direct.makeFixture")
        return
    }
    let installRoot = root.appendingPathComponent("install")
    let result = runDownload([ModelDownloader.PlanItem(entry: made.entry, installRoot: installRoot)])

    if case .failure(let error) = result {
        check(false, "DL.direct.fallbackSuccess（实际 \(error.localizedDescription)）")
        return
    }
    let installed = installRoot.appendingPathComponent("sub/model.int8.onnx")
    check(FileManager.default.fileExists(atPath: installed.path), "DL.direct.installed")
    checkEqual((try? Data(contentsOf: installed)) ?? Data(), payload, "DL.direct.content")
    check(!FileManager.default.fileExists(atPath: installed.path + ".part"), "DL.direct.noPartLeft")

    // 已就位 → 再跑一次秒成功（artifactPresent 跳过路径）。
    let again = runDownload([ModelDownloader.PlanItem(entry: made.entry, installRoot: installRoot)])
    if case .failure(let error) = again {
        check(false, "DL.direct.skipWhenPresent（实际 \(error.localizedDescription)）")
    } else {
        check(true, "DL.direct.skipWhenPresent")
    }
}

private func testDownloaderChecksumMismatchCleansPart() {
    let root = FileManager.default.temporaryDirectory
        .appendingPathComponent("ms-dl-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }

    let payload = Data("payload-that-will-not-match-pinned-sha".utf8)
    let made = try? makeDirectEntry(
        relPath: "model.int8.onnx", payload: payload, installRoot: root,
        badFirst: false, shaOverride: String(repeating: "a", count: 64))
    guard let made else {
        check(false, "DL.mismatch.makeFixture")
        return
    }
    let installRoot = root.appendingPathComponent("install")
    let result = runDownload([ModelDownloader.PlanItem(entry: made.entry, installRoot: installRoot)])

    guard case .failure(let error) = result else {
        check(false, "DL.mismatch.fails")
        return
    }
    check(true, "DL.mismatch.fails")
    let installed = installRoot.appendingPathComponent("model.int8.onnx")
    check(!FileManager.default.fileExists(atPath: installed.path), "DL.mismatch.noCorruptInstall")
    // 尺寸匹配但 SHA 不匹配：part 必须清理（防污染续传）。
    check(!FileManager.default.fileExists(atPath: installed.path + ".part"), "DL.mismatch.partCleaned")
    NSLog("DL.mismatch error: %@", error.localizedDescription)
}

/// 归档安装：真 tar.bz2（系统 tar 创建）+ 两个成员按后缀定位安装到 lib/。
private func testDownloaderArchiveInstall() {
    let root = FileManager.default.temporaryDirectory
        .appendingPathComponent("ms-dl-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }

    // 归档布局 mimic sherpa：顶层目录/lib/两个 dylib。
    let staging = root.appendingPathComponent("staging/sherpa-onnx-fixture-osx/lib")
    try? FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
    let capi = Data("fake-c-api-dylib-bytes".utf8)
    let ort = Data("fake-onnxruntime-dylib-bytes-longer".utf8)
    try? capi.write(to: staging.appendingPathComponent("libsherpa-onnx-c-api.dylib"))
    try? ort.write(to: staging.appendingPathComponent("libonnxruntime.dylib"))
    let archive = root.appendingPathComponent("fixture.tar.bz2")
    let tar = Process()
    tar.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
    tar.arguments = ["-cjf", archive.path, "-C", root.appendingPathComponent("staging").path,
                     "sherpa-onnx-fixture-osx"]
    do { try tar.run(); tar.waitUntilExit() } catch {
        check(false, "DL.archive.makeTar")
        return
    }
    guard tar.terminationStatus == 0 else {
        check(false, "DL.archive.tarStatus")
        return
    }
    guard let capiSHA = try? ModelDownloader.sha256File(
              at: staging.appendingPathComponent("libsherpa-onnx-c-api.dylib").path),
          let ortSHA = try? ModelDownloader.sha256File(
              at: staging.appendingPathComponent("libonnxruntime.dylib").path)
    else {
        check(false, "DL.archive.memberHash")
        return
    }

    let artifact = ModelManifest.ArtifactSpec(
        urls: [archive.absoluteURL.absoluteString],
        files: [
            ModelManifest.FileSpec(
                relPath: "lib/libsherpa-onnx-c-api.dylib",
                bytes: Int64(capi.count), sha256: capiSHA,
                archiveMember: "lib/libsherpa-onnx-c-api.dylib"),
            ModelManifest.FileSpec(
                relPath: "lib/libonnxruntime.dylib",
                bytes: Int64(ort.count), sha256: ortSHA,
                archiveMember: "lib/libonnxruntime.dylib"),
        ],
        isArchive: true, approxArchiveBytes: Int64(capi.count + ort.count))
    let entry = ModelManifest.EntrySpec(
        kind: .runtime, required: true, installRoot: ".", artifacts: [artifact])
    let installRoot = root.appendingPathComponent("install")

    let result = runDownload([ModelDownloader.PlanItem(entry: entry, installRoot: installRoot)])
    if case .failure(let error) = result {
        check(false, "DL.archive.installs（实际 \(error.localizedDescription)）")
        return
    }
    checkEqual(
        (try? Data(contentsOf: installRoot.appendingPathComponent("lib/libsherpa-onnx-c-api.dylib"))) ?? Data(),
        capi, "DL.archive.capiContent")
    checkEqual(
        (try? Data(contentsOf: installRoot.appendingPathComponent("lib/libonnxruntime.dylib"))) ?? Data(),
        ort, "DL.archive.ortContent")
}
