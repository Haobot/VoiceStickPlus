#pragma once

#include "app_config.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <Winhttp.h>

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace voicestick {

class AsrClientWin : public AsrClient {
public:
    explicit AsrClientWin(AppConfig config);
    ~AsrClientWin() override;

    bool Start() override;
    void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) override;
    void Cancel() override;

private:
    void RunWebSocket();
    void FlushQueuedFrames(HINTERNET websocket);
    void ReceiveOne(HINTERNET websocket);

    static bool SendFrame(HINTERNET websocket, const ByteVector& frame);
    static void AddHeader(HINTERNET request, std::string_view name, std::string_view value);
    static std::string QueryStatusCode(HINTERNET request);
    static std::string LastErrorText();
    static void CloseHandles(HINTERNET session, HINTERNET connect,
                             HINTERNET request, HINTERNET websocket);

    AppConfig config_;
    std::atomic_bool cancelled_ = false;
    std::thread worker_;
    std::mutex mutex_;
    std::vector<ByteVector> queued_frames_;
    HINTERNET websocket_ = nullptr;
};

} // namespace voicestick
