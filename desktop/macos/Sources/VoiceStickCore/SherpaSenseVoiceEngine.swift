import CSherpaOnnx
import Foundation

/// SenseVoice 推理引擎抽象：生产实现包 sherpa-onnx C API；测试注入假引擎，
/// 在无模型环境下驱动 LocalAsrClient 的流式调度逻辑（与 Windows 同构）。
public protocol SenseVoiceEngine: AnyObject {
    /// 对整段 16kHz 单声道 PCM16 做一次离线推理，返回文本。
    /// 仅在 LocalAsrClient 的 worker 线程调用，实现无需自身加锁。
    func decode(_ samples: [Int16]) -> String
}

/// 生产引擎：运行时 dlopen libsherpa-onnx-c-api.dylib 并绑定 C API。
///
/// 不在链接期绑定（仓库无 dylib 也能构建，见 Sources/CSherpaOnnx/README.md）。
/// dylib 搜索顺序（取第一个存在者）：
/// 1. env `VOICESTICK_SHERPA_ONNX_DIR` 指向目录下的 `lib/libsherpa-onnx-c-api.dylib`
///    （真机 smoke/开发机覆盖用）；
/// 2. app bundle `Contents/Frameworks/libsherpa-onnx-c-api.dylib`（打包形态）；
/// 3. `~/Library/Application Support/VoiceStick/models/sherpa-onnx/lib/`
///    （开发形态，scripts/setup-mac-local-asr.sh 的安装位）。
/// dylib 对 libonnxruntime.dylib 的 @rpath 依赖由其自带 LC_RPATH=@loader_path
/// 解析——两个 dylib 同目录放置即可。
///
/// 与 Windows `local_asr_client_win.cc` SherpaSenseVoiceEngine 同参：
/// language=auto、use_itn=1、num_threads=2（短句足够）、greedy_search、
/// feat 16kHz/80 维；模型目录布局 model.int8.onnx + tokens.txt。
public final class SherpaSenseVoiceEngine: SenseVoiceEngine {

    public enum EngineError: Error {
        case dylibNotFound(searched: [String])
        case symbolMissing(String)
        case recognizerCreateFailed
    }

    /// dylib 文件名（sherpa-onnx v1.13.7 官方 release 布局）。
    static let libraryName = "libsherpa-onnx-c-api.dylib"

    private var handle: UnsafeMutableRawPointer?
    private var recognizer: OpaquePointer?

    // dlsym 绑定的 C API（类型来自 vendored 头，@convention(c) 精确对齐）。
    private var createRecognizer: (@convention(c) (
        UnsafePointer<SherpaOnnxOfflineRecognizerConfig>?) -> OpaquePointer?)!
    private var destroyRecognizer: (@convention(c) (OpaquePointer?) -> Void)!
    private var createStream: (@convention(c) (OpaquePointer?) -> OpaquePointer?)!
    private var acceptWaveform: (@convention(c) (
        OpaquePointer?, Int32, UnsafePointer<Float>?, Int32) -> Void)!
    private var decodeStream: (@convention(c) (OpaquePointer?, OpaquePointer?) -> Void)!
    private var getResult: (@convention(c) (
        OpaquePointer?) -> UnsafePointer<SherpaOnnxOfflineRecognizerResult>?)!
    private var destroyResult: (@convention(c) (
        UnsafePointer<SherpaOnnxOfflineRecognizerResult>?) -> Void)!
    private var destroyStream: (@convention(c) (OpaquePointer?) -> Void)!

    /// dylib 搜索路径列表（出错时供诊断输出）。
    public static func librarySearchPaths() -> [String] {
        var paths: [String] = []
        if let env = ProcessInfo.processInfo.environment["VOICESTICK_SHERPA_ONNX_DIR"],
           !env.isEmpty {
            paths.append(env + "/lib/" + libraryName)
        }
        paths.append(Bundle.main.bundleURL.path + "/Contents/Frameworks/" + libraryName)
        if let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask).first {
            paths.append(appSupport.appendingPathComponent(
                "VoiceStick/models/sherpa-onnx/lib/" + libraryName).path)
        }
        return paths
    }

    /// - Parameters:
    ///   - modelsDir: SenseVoice 模型目录（含 model.int8.onnx 与 tokens.txt）。
    ///   - numThreads: 推理线程数（Windows 同参默认 2）。
    ///   - libraryDir: 显式 dylib 所在目录（含 lib/ 子目录的根）；nil = 按搜索序。
    public init(modelsDir: String, numThreads: Int32 = 2, libraryDir: String? = nil) throws {
        // 1) dlopen（RTLD_LOCAL|GLOBAL 均可； sherpa 头无回调注册，LOCAL 即可）。
        let dylibPath: String
        if let libraryDir {
            dylibPath = libraryDir + "/lib/" + Self.libraryName
        } else {
            dylibPath = Self.librarySearchPaths().first { FileManager.default.fileExists(atPath: $0) }
                ?? (Self.librarySearchPaths().first ?? "")
        }
        guard let handle = dlopen(dylibPath, RTLD_NOW | RTLD_LOCAL) else {
            throw EngineError.dylibNotFound(searched: Self.librarySearchPaths())
        }
        self.handle = handle

        // 2) dlsym 绑定（逐个检查，任一缺失即失败——半绑定引擎不可用）。
        var missing: String?
        func bind(_ name: String) -> UnsafeMutableRawPointer? {
            if let p = dlsym(handle, name) { return p }
            if missing == nil { missing = name }
            return nil
        }

        if let p = bind("SherpaOnnxCreateOfflineRecognizer") {
            createRecognizer = unsafeBitCast(p, to: (@convention(c) (
                UnsafePointer<SherpaOnnxOfflineRecognizerConfig>?) -> OpaquePointer?).self)
        }
        if let p = bind("SherpaOnnxDestroyOfflineRecognizer") {
            destroyRecognizer = unsafeBitCast(p, to: (@convention(c) (OpaquePointer?) -> Void).self)
        }
        if let p = bind("SherpaOnnxCreateOfflineStream") {
            createStream = unsafeBitCast(p, to: (@convention(c) (OpaquePointer?) -> OpaquePointer?).self)
        }
        if let p = bind("SherpaOnnxAcceptWaveformOffline") {
            acceptWaveform = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, Int32, UnsafePointer<Float>?, Int32) -> Void).self)
        }
        if let p = bind("SherpaOnnxDecodeOfflineStream") {
            decodeStream = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?, OpaquePointer?) -> Void).self)
        }
        if let p = bind("SherpaOnnxGetOfflineStreamResult") {
            getResult = unsafeBitCast(p, to: (@convention(c) (
                OpaquePointer?) -> UnsafePointer<SherpaOnnxOfflineRecognizerResult>?).self)
        }
        if let p = bind("SherpaOnnxDestroyOfflineRecognizerResult") {
            destroyResult = unsafeBitCast(p, to: (@convention(c) (
                UnsafePointer<SherpaOnnxOfflineRecognizerResult>?) -> Void).self)
        }
        if let p = bind("SherpaOnnxDestroyOfflineStream") {
            destroyStream = unsafeBitCast(p, to: (@convention(c) (OpaquePointer?) -> Void).self)
        }
        if let missing {
            throw EngineError.symbolMissing(missing)
        }

        // 3) 创建 recognizer。config 里的 const char* 必须指向稳定内存且存活到
        //    create 调用结束（C API 在创建期拷贝）——withUnsafeBufferPointer
        //    嵌套保证各 cString 缓冲的生命周期覆盖 createRecognizer。
        let modelPath = (modelsDir as NSString).appendingPathComponent("model.int8.onnx")
        let tokensPath = (modelsDir as NSString).appendingPathComponent("tokens.txt")
        var modelC = Array(modelPath.utf8CString)
        var tokensC = Array(tokensPath.utf8CString)
        var langC = Array("auto".utf8CString)
        var providerC = Array("cpu".utf8CString)
        var decodeC = Array("greedy_search".utf8CString)

        modelC.withUnsafeBufferPointer { modelBuf in
        tokensC.withUnsafeBufferPointer { tokensBuf in
        langC.withUnsafeBufferPointer { langBuf in
        providerC.withUnsafeBufferPointer { providerBuf in
        decodeC.withUnsafeBufferPointer { decodeBuf in
            var config = SherpaOnnxOfflineRecognizerConfig()
            config.model_config.sense_voice.model = modelBuf.baseAddress
            config.model_config.sense_voice.language = langBuf.baseAddress
            config.model_config.sense_voice.use_itn = 1
            config.model_config.tokens = tokensBuf.baseAddress
            config.model_config.num_threads = numThreads
            config.model_config.provider = providerBuf.baseAddress
            config.feat_config.sample_rate = 16000
            config.feat_config.feature_dim = 80
            config.decoding_method = decodeBuf.baseAddress
            recognizer = createRecognizer(&config)
        }}}}}
        guard recognizer != nil else {
            throw EngineError.recognizerCreateFailed
        }
    }

    deinit {
        if let recognizer {
            destroyRecognizer(recognizer)
        }
        if let handle {
            dlclose(handle)
        }
    }

    public func decode(_ samples: [Int16]) -> String {
        guard let recognizer else { return "" }
        guard let stream = createStream(recognizer) else { return "" }
        defer { destroyStream(stream) }
        var floats = [Float](repeating: 0, count: samples.count)
        for (i, s) in samples.enumerated() {
            floats[i] = Float(s) / 32768.0
        }
        floats.withUnsafeBufferPointer { buf in
            acceptWaveform(stream, 16000, buf.baseAddress, Int32(floats.count))
        }
        decodeStream(recognizer, stream)
        guard let result = getResult(stream) else { return "" }
        defer { destroyResult(result) }
        guard let text = result.pointee.text else { return "" }
        return String(cString: text)
    }
}
