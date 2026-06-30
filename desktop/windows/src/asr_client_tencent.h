#pragma once

#include "app_config.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <Winhttp.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace voicestick {

/// 腾讯云实时语音识别（WebSocket）客户端。
///
/// 协议要点（参考 https://cloud.tencent.com/document/product/1093/48982）：
/// - 鉴权：URL 查询参数 HMAC-SHA1 签名
/// - 握手成功后服务端先返回 {"code":0,"message":"success","voice_id":"..."}
/// - 音频数据：二进制帧（Opus, voice_format=10），每帧需封装为
///   "Opus"(4B) + 大端长度(2B) + Opus 一帧压缩数据
/// - 结束识别：文本帧 JSON（{"type":"end"}）
/// - 识别结果：文本帧 JSON（slice_type 0=开始/1=中间/2=稳态结束）
///
/// 状态机：Disconnected → Connecting → Ready → Streaming → Finishing → Idle
class AsrClientTencent : public AsrClient {
public:
    explicit AsrClientTencent(AppConfig config);
    ~AsrClientTencent() override;

    // ---- AsrClient 接口 ----
    bool Start(AsrSessionOptions options = {}) override;
    void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) override;
    void Cancel() override;
    std::string LastStartError() const override;

    // ---- 静态工具方法（公开以便单元测试）----
    static std::string BuildSignedUrl(const AppConfig& config, std::string_view voice_id);
    static std::string HmacSha1(std::string_view key, std::string_view message);
    static std::string Base64Encode(std::span<const std::uint8_t> data);
    static std::string UrlEncode(std::string_view value);
    static std::string GenerateVoiceId();
    static std::string MakeEndMessage();
    static ByteVector ExtractTencentOpusFrame(std::span<const std::uint8_t> data);
    static std::string ExtractVoiceText(std::string_view json);
    static int ExtractSliceType(std::string_view json);
    static int ExtractFinalFlag(std::string_view json);
    static int ExtractErrorCode(std::string_view json);
    static std::string ExtractErrorMessage(std::string_view json);
    static std::vector<AsrSegment> ExtractWordListSegments(std::string_view json,
                                                           std::set<std::string>* emitted_keys);
    static std::string SegmentKey(int start_ms, int end_ms, std::string_view text);
    /// 把单句稳态结果累积到已确定文本上。空 sentence 不改变累积。
    static std::string AccumulateSentence(std::string_view current, std::string_view sentence);

private:
    enum class ConnectionState {
        kDisconnected,
        kConnecting,
        kReady,
    };

    enum class SessionState {
        kIdle,
        kStarting,   // WebSocket 已连接，等待服务端握手确认
        kStreaming,  // 可发送音频数据
        kFinishing,  // 已发送 end，等待最终结果
    };

    struct QueuedAudioChunk {
        ByteVector data;
        bool is_last = false;
    };

    // ---- WebSocket 生命周期 ----
    void RunWebSocket();
    void ShutdownConnection();
    void FailSession(const std::string& message);

    // ---- 帧发送 ----
    static bool SendTextFrame(HINTERNET websocket, std::string_view text);
    static bool SendBinaryFrame(HINTERNET websocket, std::span<const std::uint8_t> data);

    // ---- 结果分发 ----
    void HandleTextResponse(std::string_view json, HINTERNET websocket);
    /// 整段识别结束（收到 final=1 或连接关闭兜底）时输出累积全文并重置会话。
    /// 受 final_emitted_ 守卫保护，本会话只触发一次 on_final；cancelled_ 时不触发。
    void EmitFinalText();

    // ---- 音频发送 ----
    void SendAudio(std::span<const std::uint8_t> data, bool is_last);
    void FlushQueuedAudioChunks();
    void FinishSessionIfNeeded();

    // ---- 工具 ----
    bool SendFrameOrFail(HINTERNET websocket, const ByteVector& frame,
                         WINHTTP_WEB_SOCKET_BUFFER_TYPE type,
                         const std::string& context);
    void SetLastStartError(std::string message);
    static std::string LastErrorText();

    AppConfig config_;
    std::atomic_bool cancelled_ = false;
    std::thread worker_;
    mutable std::recursive_mutex mutex_;
    std::vector<QueuedAudioChunk> queued_audio_chunks_;
    ConnectionState connection_state_ = ConnectionState::kDisconnected;
    SessionState session_state_ = SessionState::kIdle;
    std::string current_voice_id_;
    std::string latest_transcript_;
    std::string accumulated_final_text_;  // 累积各句 slice_type=2 稳态文本，整段结束(on_final)时输出
    bool final_emitted_ = false;          // 本会话是否已触发 on_final（防 final=1 与 close 兜底重复）
    std::set<std::string> emitted_definite_segment_keys_;
    AsrSessionOptions session_options_;
    HINTERNET websocket_ = nullptr;
    std::string last_start_error_;
    std::string cached_vocab_id_;      // 本次会话自动创建的热词表 ID
    std::string pending_error_message_; // 延迟触发的错误消息（避免跨线程死锁）
};

} // namespace voicestick
