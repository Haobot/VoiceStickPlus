import Foundation
import VoiceStickCore

// 诊断回放：把 DebugAudio 的会话 ogg 按 BLE 节奏喂给真实 LocalAsrClient，
// 打印每一步（引擎加载/回调/延迟）。用法：
//   swift run AsrReplayCli <ogg 路径> [models_dir]
let args = CommandLine.arguments
guard args.count >= 2 else {
    print("usage: AsrReplayCli <session.ogg> [models_dir]")
    exit(2)
}
let oggURL = URL(fileURLWithPath: args[1])
let modelsDir = args.count >= 3 ? args[2]
    : AppConfig.configDirectory.appendingPathComponent(
        "models/sense-voice-int8-2024-07-17").path

guard let ogg = try? Data(contentsOf: oggURL) else {
    print("无法读取 \(oggURL.path)"); exit(1)
}
print("ogg: \(oggURL.lastPathComponent) \(ogg.count) bytes")
print("models_dir: \(modelsDir)")
if let err = LocalAsrClient.validateModelsDir(modelsDir) {
    print("模型校验失败: \(err)"); exit(1)
}
let dylibs = SherpaSenseVoiceEngine.librarySearchPaths()
print("dylib 搜索:", dylibs)
print("dylib 命中:", dylibs.first { FileManager.default.fileExists(atPath: $0) } ?? "无!")

// 先直接测 demuxer + 引擎（绕过 client 状态机）
var stream = OggOpusStream()
let parseOK = OggOpusDemuxer.parse(ogg, into: &stream)
print("demux: ok=\(parseOK) packets=\(stream.packets.count) rate=\(stream.sampleRate) ch=\(stream.channels)")
if let engine = try? SherpaSenseVoiceEngine(modelsDir: modelsDir) {
    let dec = try! AudioOpusDecoder()
    var pcm = [Int16]()
    for p in stream.packets {
        if let f = dec.decode(p) { pcm.append(contentsOf: f) }
    }
    print("decoded pcm samples: \(pcm.count) (\(Double(pcm.count)/16000.0)s)")
    let t0 = Date()
    let text = engine.decode(pcm)
    print(String(format: "直接推理(%.0fms): \"%@\"", -t0.timeIntervalSinceNow*1000, text))

    // 再过完整 client 状态机（按页分块喂，模拟 BLE chunk 节奏）
    let client = LocalAsrClient(modelsDir: modelsDir)
    var partialCount = 0
    client.onPartial = { partialCount += 1; print("  onPartial[\(partialCount)]: \($0)") }
    client.onFinal = { print("  onFinal: \($0)") }
    client.onError = { print("  onError: \($0)") }
    let t1 = Date()
    let ok = client.start(options: ASRSessionOptions())
    print(String(format: "client.start = %@ (%.0fms)", "\(ok)", -t1.timeIntervalSinceNow*1000))
    if ok {
        // 切成 ~5 块喂（大小无关紧要，client 是全量缓冲追加）
        let chunkSize = max(1, ogg.count / 5)
        var i = 0
        while i < ogg.count {
            let end = min(i + chunkSize, ogg.count)
            client.sendOggOpusChunk(ogg.subdata(in: i..<end), isLast: end == ogg.count)
            i = end
        }
        // 等回调最多 10s
        let deadline = Date().addingTimeInterval(10)
        while Date() < deadline { Thread.sleep(forTimeInterval: 0.1) }
        print("done (partialCount=\(partialCount))")
    }
} else {
    print("引擎创建失败！")
    exit(1)
}
