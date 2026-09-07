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

// SenseVoice 推理吃的 float 样本与 16-bit PCM 的换算（sherpa-onnx 惯例 [-1,1]）。
std::vector<float> Pcm16ToFloat(const std::vector<std::int16_t>& pcm) {
    std::vector<float> out(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        out[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    return out;
}

}  // namespace

struct LocalAsrClient::Impl {
    std::string models_dir;
    int num_threads = 2;

    // recognizer 跨会话复用（模型加载 ~1s，只在首次 Start 构建）。
    const SherpaOnnxOfflineRecognizer* recognizer = nullptr;
    std::string last_start_error;

    std::mutex mutex;
    std::condition_variable work_signal;
    std::thread worker;
    std::atomic_bool cancelled{false};
    bool has_job = false;
    bool shutting_down = false;
    ByteVector pending_ogg;

    AsrClient* client = nullptr;   // 回透 on_final/on_error（owner 即 LocalAsrClient）

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            shutting_down = true;
        }
        work_signal.notify_all();
        if (worker.joinable()) worker.join();
        if (recognizer) SherpaOnnxDestroyOfflineRecognizer(recognizer);
    }

    void WorkerMain() {
        for (;;) {
            ByteVector ogg;
            {
                std::unique_lock<std::mutex> lock(mutex);
                work_signal.wait(lock, [&] { return has_job || shutting_down; });
                if (shutting_down) return;
                ogg = std::move(pending_ogg);
                pending_ogg.clear();
                has_job = false;
            }
            if (cancelled.load()) continue;   // Cancel 后丢弃积压任务
            Recognize(std::move(ogg));
        }
    }

    void Recognize(ByteVector ogg) {
        OggOpusStream stream;
        if (!ParseOggOpus(ogg, stream) || stream.packets.empty()) {
            client->on_error("本地识别失败：音频数据无法解析");
            return;
        }
        std::vector<std::int16_t> pcm;
        AudioOpusDecoder decoder;
        for (const auto& packet : stream.packets) {
            std::vector<std::int16_t> frame(5760);   // 40ms 帧 @16k，留足余量
            const auto result = decoder.Decode(packet.data(), packet.size(),
                                               frame.data(), frame.size());
            if (result.decoded_samples <= 0) {
                client->on_error("本地识别失败：Opus 解码出错 (" +
                                 std::to_string(result.opus_error) + ")");
                return;
            }
            frame.resize(static_cast<size_t>(result.decoded_samples));
            pcm.insert(pcm.end(), frame.begin(), frame.end());
        }
        if (pcm.empty()) {
            client->on_error("本地识别失败：无有效音频样本");
            return;
        }

        const SherpaOnnxOfflineStream* s =
            SherpaOnnxCreateOfflineStream(recognizer);
        if (!s) {
            client->on_error("本地识别失败：创建识别流失败");
            return;
        }
        const auto samples = Pcm16ToFloat(pcm);
        SherpaOnnxAcceptWaveformOffline(s, 16000, samples.data(),
                                        static_cast<std::int32_t>(samples.size()));
        SherpaOnnxDecodeOfflineStream(recognizer, s);
        const SherpaOnnxOfflineRecognizerResult* result =
            SherpaOnnxGetOfflineStreamResult(s);
        std::string text = result && result->text ? result->text : "";
        if (result) SherpaOnnxDestroyOfflineRecognizerResult(result);
        SherpaOnnxDestroyOfflineStream(s);
        if (cancelled.load()) return;
        client->on_final(std::move(text));
    }
};

LocalAsrClient::LocalAsrClient(std::string models_dir, int num_threads)
    : impl_(std::make_unique<Impl>()) {
    impl_->models_dir = std::move(models_dir);
    impl_->num_threads = num_threads;
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
    namespace fs = std::filesystem;
    impl_->last_start_error.clear();

    if (auto error = ValidateSenseVoiceModelsDir(impl_->models_dir)) {
        impl_->last_start_error = *error;
        return false;
    }
    const fs::path model = fs::path(impl_->models_dir) / "model.int8.onnx";
    const fs::path tokens = fs::path(impl_->models_dir) / "tokens.txt";
    if (impl_->recognizer) {   // 已构建，跨会话复用
        impl_->cancelled = false;
        return true;
    }

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
    config.model_config.num_threads = impl_->num_threads;
    config.model_config.provider = kProvider;
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.decoding_method = kDecoding;

    impl_->recognizer = SherpaOnnxCreateOfflineRecognizer(&config);
    if (!impl_->recognizer) {
        impl_->last_start_error = "本地识别引擎初始化失败（模型损坏或不受支持）";
        return false;
    }
    impl_->cancelled = false;
    if (!impl_->worker.joinable()) {
        impl_->worker = std::thread([this] { impl_->WorkerMain(); });
    }
    return true;
}

void LocalAsrClient::SendOggOpusChunk(std::span<const std::uint8_t> data,
                                      bool is_last) {
    if (!impl_->recognizer) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pending_ogg.insert(impl_->pending_ogg.end(),
                                  data.begin(), data.end());
        if (is_last) impl_->has_job = true;
    }
    if (is_last) impl_->work_signal.notify_all();
}

void LocalAsrClient::Cancel() {
    impl_->cancelled = true;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pending_ogg.clear();
        impl_->has_job = false;
    }
    // recognizer 复用不销毁；下次 Start 重置取消态即可。
}

std::string LocalAsrClient::LastStartError() const {
    return impl_->last_start_error;
}

}  // namespace voicestick
