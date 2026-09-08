// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// LocalAsrClient 实现：sherpa-onnx C API（SenseVoice 离线识别）。
// 二进制来源见 CMakeLists sherpa-onnx 段（m0 venv wheel，与 Python 端同版本 ABI）。

#include "local_asr_client_win.h"

#include <sherpa-onnx/c-api/c-api.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

#include "audio_opus_decoder.h"
#include "ogg_opus_demuxer.h"

namespace voicestick {
namespace {

// partial 推理节流周期：worker 对当前全量音频滚动重解码，两次推理至少间隔
// 600ms——首块立即解码尽快出首个 partial，其后按周期轮转；单次推理超过周期时
// 完成后立即追赶下一轮（长录音下 partial 频率随推理耗时自然下降，不丢尾）。
constexpr auto kPartialIntervalMs = std::chrono::milliseconds(600);

// SenseVoice 推理吃的 float 样本与 16-bit PCM 的换算（sherpa-onnx 惯例 [-1,1]）。
std::vector<float> Pcm16ToFloat(const std::vector<std::int16_t>& pcm) {
    std::vector<float> out(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        out[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    return out;
}

// 生产引擎：包 sherpa-onnx C API。recognizer 跨会话复用（模型加载 ~1s）。
class SherpaSenseVoiceEngine : public SenseVoiceEngine {
 public:
    ~SherpaSenseVoiceEngine() override {
        if (recognizer_) SherpaOnnxDestroyOfflineRecognizer(recognizer_);
    }

    // 构建 recognizer；失败返回 nullptr（调用方如实报错，不静默降级）。
    static std::unique_ptr<SherpaSenseVoiceEngine> Create(const std::string& models_dir,
                                                          int num_threads) {
        namespace fs = std::filesystem;
        const fs::path model = fs::path(models_dir) / "model.int8.onnx";
        const fs::path tokens = fs::path(models_dir) / "tokens.txt";
        // config 里的 const char* 必须指向稳定内存（CreateRecognizer 读取前临时
        // string 不能析构），先落地局部变量。
        const std::string model_path = model.string();
        const std::string tokens_path = tokens.string();
        const char* kLanguage = "auto";
        const char* kProvider = "cpu";
        const char* kDecoding = "greedy_search";

        SherpaOnnxOfflineRecognizerConfig config = {};
        config.model_config.sense_voice.model = model_path.c_str();
        config.model_config.sense_voice.language = kLanguage;
        config.model_config.sense_voice.use_itn = 1;
        config.model_config.tokens = tokens_path.c_str();
        config.model_config.num_threads = num_threads;
        config.model_config.provider = kProvider;
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        config.decoding_method = kDecoding;

        auto engine = std::unique_ptr<SherpaSenseVoiceEngine>(new SherpaSenseVoiceEngine());
        engine->recognizer_ = SherpaOnnxCreateOfflineRecognizer(&config);
        if (!engine->recognizer_) return nullptr;
        return engine;
    }

    std::string Decode(std::span<const std::int16_t> samples) override {
        const SherpaOnnxOfflineStream* s = SherpaOnnxCreateOfflineStream(recognizer_);
        if (!s) return {};
        const auto samples_f = Pcm16ToFloat(std::vector<std::int16_t>(
            samples.begin(), samples.end()));
        SherpaOnnxAcceptWaveformOffline(s, 16000, samples_f.data(),
                                        static_cast<std::int32_t>(samples_f.size()));
        SherpaOnnxDecodeOfflineStream(recognizer_, s);
        const SherpaOnnxOfflineRecognizerResult* result =
            SherpaOnnxGetOfflineStreamResult(s);
        std::string text = result && result->text ? result->text : "";
        if (result) SherpaOnnxDestroyOfflineRecognizerResult(result);
        SherpaOnnxDestroyOfflineStream(s);
        return text;
    }

 private:
    const SherpaOnnxOfflineRecognizer* recognizer_ = nullptr;
};

}  // namespace

struct LocalAsrClient::Impl {
    std::string models_dir;
    int num_threads = 2;

    std::string last_start_error;
    // 实际使用的推理引擎：生产 = SherpaSenseVoiceEngine（跨会话复用），
    // 测试 = 构造注入的假引擎（跳过模型校验与创建）。
    std::unique_ptr<SenseVoiceEngine> engine;

    std::mutex mutex;
    std::condition_variable work_signal;
    std::thread worker;
    std::atomic_bool cancelled{false};

    // 锁保护会话输入状态（SendOggOpusChunk 追加，worker 快照消费）。
    bool shutting_down = false;
    std::uint64_t session_id = 0;    // Start/Cancel 递增：worker 据此丢弃增量状态
    std::uint64_t ogg_version = 0;   // 每次追加递增：worker 据此判断有新数据
    bool is_last_received = false;
    ByteVector ogg_buffer;

    AsrClient* client = nullptr;   // 回透回调（owner 即 LocalAsrClient）

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            shutting_down = true;
        }
        work_signal.notify_all();
        if (worker.joinable()) worker.join();
    }

    void ResetSessionLocked() {
        ++session_id;
        ++ogg_version;
        ogg_buffer.clear();
        is_last_received = false;
    }

    // 增量解析+解码 ogg 快照中尚未解码的 Opus packet，追加 pcm。
    // 返回 false = 无可解析数据或解码出错（partial 阶段调用方静默跳过——与云端
    // 网络抖动不杀会话同口径；final 阶段由调用方统一报错）。
    bool AppendNewPackets(const ByteVector& ogg, std::size_t& packets_decoded,
                          AudioOpusDecoder& decoder, std::vector<std::int16_t>& pcm,
                          std::string& error) {
        OggOpusStream stream;
        if (!ParseOggOpus(ogg, stream) && stream.packets.empty()) {
            error = "音频数据无法解析";
            return false;
        }
        if (stream.packets.size() <= packets_decoded) return true;   // 无新包
        for (std::size_t i = packets_decoded; i < stream.packets.size(); ++i) {
            const auto& packet = stream.packets[i];
            std::vector<std::int16_t> frame(5760);   // 最大 Opus 帧长（120ms）余量
            const auto result = decoder.Decode(packet.data(), packet.size(),
                                               frame.data(), frame.size());
            if (result.decoded_samples <= 0) {
                error = "Opus 解码出错 (" + std::to_string(result.opus_error) + ")";
                return false;
            }
            frame.resize(static_cast<std::size_t>(result.decoded_samples));
            pcm.insert(pcm.end(), frame.begin(), frame.end());
        }
        packets_decoded = stream.packets.size();
        return true;
    }

    void WorkerMain() {
        // worker 私有增量状态（锁外独占），随 session_id 变化整体重置。
        std::uint64_t my_session = 0;
        std::uint64_t my_version = 0;
        bool session_final_done = false;
        bool throttle_armed = false;
        auto next_decode_time = std::chrono::steady_clock::now();
        AudioOpusDecoder decoder;
        std::vector<std::int16_t> pcm;
        std::size_t packets_decoded = 0;
        std::string last_text;        // 上次推理文本（final 无新样本时直接复用）
        std::size_t last_decoded_samples = 0;

        for (;;) {
            ByteVector ogg;
            std::uint64_t session = 0;
            bool final_job = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                for (;;) {
                    if (shutting_down) return;
                    final_job = is_last_received && !session_final_done;
                    // 空 buffer 的版本变化是 Start/Cancel 的重置产物，不算新数据
                    // （否则首个真音频块会被误武装的节流挡住 600ms）。
                    const bool has_new =
                        ogg_version > my_version && !ogg_buffer.empty();
                    // 会话首块立即解码（尽快出首个 partial），其后按节流周期。
                    if (final_job || (has_new && !throttle_armed)) break;
                    if (has_new && std::chrono::steady_clock::now() >= next_decode_time) {
                        break;
                    }
                    // 无新数据睡到事件；有新数据睡到节流点（虚假唤醒由谓词兜底）。
                    work_signal.wait_until(
                        lock,
                        has_new ? next_decode_time
                                : std::chrono::steady_clock::time_point::max(),
                        [&] {
                            if (shutting_down) return true;
                            if (is_last_received && !session_final_done) return true;
                            if (ogg_version <= my_version || ogg_buffer.empty()) {
                                return false;
                            }
                            return !throttle_armed ||
                                   std::chrono::steady_clock::now() >=
                                       next_decode_time;
                        });
                }
                session = session_id;
                final_job = is_last_received && !session_final_done;
                ogg.assign(ogg_buffer.begin(), ogg_buffer.end());
                my_version = ogg_version;
            }

            if (session != my_session) {   // 新会话：丢弃上轮增量状态
                my_session = session;
                session_final_done = false;
                throttle_armed = false;
                decoder.Reset();
                pcm.clear();
                packets_decoded = 0;
                last_text.clear();
                last_decoded_samples = 0;
            }
            if (cancelled.load()) continue;   // 已取消：丢弃积压，挂起等新会话

            std::string error;
            if (!AppendNewPackets(ogg, packets_decoded, decoder, pcm, error)) {
                if (final_job) {
                    client->on_error("本地识别失败：" + error);
                    session_final_done = true;
                } else if (!throttle_armed) {   // 首块即失败：武装节流防高频重试
                    throttle_armed = true;
                    next_decode_time = std::chrono::steady_clock::now() +
                                       kPartialIntervalMs;
                }
                continue;
            }
            if (pcm.empty()) {
                if (final_job) {
                    client->on_error("本地识别失败：无有效音频样本");
                    session_final_done = true;
                }
                continue;
            }
            if (pcm.size() == last_decoded_samples) {
                // 无新音频样本：final 直接复用上次推理结果（松键即出，省一次推理）。
                if (final_job && !cancelled.load()) {
                    client->on_final(last_text);
                    session_final_done = true;
                }
                continue;
            }
            const std::string text = engine->Decode(pcm);
            if (cancelled.load()) continue;
            last_text = text;
            last_decoded_samples = pcm.size();
            next_decode_time = std::chrono::steady_clock::now() + kPartialIntervalMs;
            throttle_armed = true;
            if (final_job) {
                client->on_final(text);
                session_final_done = true;
            } else if (!text.empty()) {
                // 空文本 partial 不上报：录音起始静音阶段保持 Listening 显示。
                client->on_partial(text);
            }
        }
    }
};

LocalAsrClient::LocalAsrClient(std::string models_dir, int num_threads,
                               std::unique_ptr<SenseVoiceEngine> engine_override)
    : impl_(std::make_unique<Impl>()) {
    impl_->models_dir = std::move(models_dir);
    impl_->num_threads = num_threads;
    impl_->engine = std::move(engine_override);
    impl_->client = this;
}

std::optional<std::string> ValidateSenseVoiceModelsDir(const std::string& models_dir) {
    namespace fs = std::filesystem;
    const fs::path model = fs::path(models_dir) / "model.int8.onnx";
    const fs::path tokens = fs::path(models_dir) / "tokens.txt";
    if (!fs::exists(model) || !fs::exists(tokens)) {
        return "本地识别模型未就位: " + models_dir +
               "（需 SenseVoice model.int8.onnx 与 tokens.txt）";
    }
    return std::nullopt;
}

std::string ResolveLocalMicModelsDir(const std::string& configured,
                                     const std::string& exe_dir) {
    namespace fs = std::filesystem;
    fs::path dir = configured.empty() ? fs::path("models") : fs::path(configured);
    if (dir.is_relative()) dir = fs::path(exe_dir) / dir;
    return dir.string();
}

LocalAsrClient::~LocalAsrClient() = default;

bool LocalAsrClient::Start(AsrSessionOptions /*options*/) {
    impl_->last_start_error.clear();

    if (!impl_->engine) {
        if (auto error = ValidateSenseVoiceModelsDir(impl_->models_dir)) {
            impl_->last_start_error = *error;
            return false;
        }
        impl_->engine = SherpaSenseVoiceEngine::Create(impl_->models_dir,
                                                       impl_->num_threads);
        if (!impl_->engine) {
            impl_->last_start_error = "本地识别引擎初始化失败（模型损坏或不受支持）";
            return false;
        }
    }

    // 引擎跨会话复用；会话输入状态整体重置（丢弃上一会话/取消后的残留数据）。
    impl_->cancelled = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->ResetSessionLocked();
    }
    impl_->work_signal.notify_all();
    if (!impl_->worker.joinable()) {
        impl_->worker = std::thread([this] { impl_->WorkerMain(); });
    }
    return true;
}

void LocalAsrClient::SendOggOpusChunk(std::span<const std::uint8_t> data,
                                      bool is_last) {
    if (!impl_->engine) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->ogg_buffer.insert(impl_->ogg_buffer.end(), data.begin(), data.end());
        ++impl_->ogg_version;
        if (is_last) impl_->is_last_received = true;
    }
    // 逐块唤醒 worker：录音期间即按节流周期滚动推理产出 partial。
    impl_->work_signal.notify_all();
}

void LocalAsrClient::Cancel() {
    impl_->cancelled = true;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->ResetSessionLocked();   // 清积压并递增 session_id，worker 丢弃增量态
    }
    impl_->work_signal.notify_all();
    // 引擎跨会话复用不销毁；下次 Start 重置取消态即可。
}

std::string LocalAsrClient::LastStartError() const {
    return impl_->last_start_error;
}

}  // namespace voicestick
