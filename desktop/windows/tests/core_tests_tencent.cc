// N8 cut11: tencent ASR batch extracted from core_tests.cc (15 tests).
// Pre-checks: col0 non-test heads = 0, cross-span refs = 0.
#include "test_support.h"

void TestTencentProviderSelection() {
    assert(AsrProviderFromName("voicestick_cloud") == AsrProvider::kVoiceStickCloud);
    assert(AsrProviderFromName("volcengine") == AsrProvider::kVolcengine);
    assert(AsrProviderFromName("tencent") == AsrProvider::kTencent);
    // 未知名称回退到 volcengine
    assert(AsrProviderFromName("unknown") == AsrProvider::kVolcengine);

    assert(AsrProviderName(AsrProvider::kVoiceStickCloud) == "voicestick_cloud");
    assert(AsrProviderName(AsrProvider::kVolcengine) == "volcengine");
    assert(AsrProviderName(AsrProvider::kTencent) == "tencent");
}

void TestTencentConfigRoundTrip() {
    // 测试腾讯云字段的 TOML 读写
    AppConfig config = AppConfig::Defaults();
    config.asr_provider = AsrProvider::kTencent;
    config.tencent_secret_id = "AKID-test-id";
    config.tencent_secret_key = "test-secret-key";
    config.tencent_appid = "1234567890";
    config.tencent_engine_model_type = "16k_zh_en";
    config.tencent_hotword_id = "vocab-abc123";

    assert(config.asr_provider == AsrProvider::kTencent);
    assert(config.tencent_secret_id == "AKID-test-id");
    assert(config.tencent_secret_key == "test-secret-key");
    assert(config.tencent_appid == "1234567890");
    assert(config.tencent_engine_model_type == "16k_zh_en");
    assert(config.tencent_hotword_id == "vocab-abc123");

    // ActiveApiKey 对 Tencent 应返回 SecretId
    assert(config.ActiveApiKey() == "AKID-test-id");

    // ActiveWebsocketUrl 对 Tencent 应包含 appid
    auto url = config.ActiveWebsocketUrl();
    assert(url.find("asr.cloud.tencent.com") != std::string::npos);
    assert(url.find("1234567890") != std::string::npos);

    // 默认引擎模型
    AppConfig defaults = AppConfig::Defaults();
    assert(defaults.tencent_engine_model_type == "16k_zh");
}

void TestTencentCredentialsTrimmedOnLoad() {
    // 凭据前后带空格是 Tencent 返回 4002 "密钥不存在" 的常见原因。
    // 验证 TOML 加载时自动去除首尾空格。
    auto temp = std::filesystem::temp_directory_path() /
                "voicestick_tencent_trim_test.toml";
    std::filesystem::remove(temp);

    {
        std::ofstream out(temp);
        out << "asr_provider = \"tencent\"\n";
        out << "tencent_secret_id = \"  AKID-test-id  \"\n";
        out << "tencent_secret_key = \"  test-secret-key  \"\n";
        out << "tencent_appid = \"  1234567890  \"\n";
        out << "tencent_hotword_id = \"  vocab-abc  \"\n";
    }

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.tencent_secret_id == "AKID-test-id");
    assert(loaded.tencent_secret_key == "test-secret-key");
    assert(loaded.tencent_appid == "1234567890");
    assert(loaded.tencent_hotword_id == "vocab-abc");

    std::filesystem::remove(temp);
}

void TestTencentSecretIdRecoveryFromVolcengineField() {
    // 历史版本设置对话框在 ASR 提供商切换时，可能把 Tencent SecretId
    // 误写入 volcengine_api_key 字段。验证加载配置时自动回迁。
    auto temp = std::filesystem::temp_directory_path() /
                "voicestick_tencent_recovery_test.toml";
    std::filesystem::remove(temp);

    {
        std::ofstream out(temp);
        out << "asr_provider = \"tencent\"\n";
        out << "volcengine_api_key = \"AKID_REDACTED_PLACEHOLDER\"\n";
        out << "tencent_secret_id = \"a31355ab-old-wrong\"\n";
        out << "tencent_secret_key = \"secret-key\"\n";
        out << "tencent_appid = \"1259040144\"\n";
    }

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.tencent_secret_id == "AKID_REDACTED_PLACEHOLDER");
    assert(loaded.volcengine_api_key.empty());

    std::filesystem::remove(temp);
}

void TestTencentSignatureGeneration() {
    // 验证 HMAC-SHA1（使用已知测试向量）
    auto result = AsrClientTencent::HmacSha1("key", "The quick brown fox jumps over the lazy dog");
    // HMAC-SHA1("key", message) 的已知结果
    assert(!result.empty());
    // SHA1 HMAC 输出 20 字节
    assert(result.size() == 20);

    // 空消息
    auto empty_result = AsrClientTencent::HmacSha1("key", "");
    assert(!empty_result.empty());
    assert(empty_result.size() == 20);

    // Base64 编码
    std::vector<std::uint8_t> test_bytes = {'M', 'a', 'n'};
    auto b64 = AsrClientTencent::Base64Encode(test_bytes);
    assert(b64 == "TWFu");

    // URL 编码
    auto url_enc = AsrClientTencent::UrlEncode("hello world");
    assert(url_enc == "hello%20world");
}

void TestTencentUrlConstruction() {
    AppConfig config = AppConfig::Defaults();
    config.tencent_secret_id = "AKIDtest";
    config.tencent_secret_key = "testkey";
    config.tencent_appid = "1234567890";
    config.tencent_engine_model_type = "16k_zh_en";
    config.tencent_hotword_id = "vocab-abc";

    auto url = AsrClientTencent::BuildSignedUrl(config, "test-voice-id-1234");

    // 验证 URL 基本结构
    assert(url.starts_with("wss://asr.cloud.tencent.com/asr/v2/1234567890?"));
    assert(url.find("secretid=AKIDtest") != std::string::npos);
    assert(url.find("engine_model_type=16k_zh_en") != std::string::npos);
    assert(url.find("voice_format=10") != std::string::npos);
    assert(url.find("needvad=1") != std::string::npos);
    assert(url.find("voice_id=test-voice-id-1234") != std::string::npos);
    assert(url.find("hotword_id=vocab-abc") != std::string::npos);
    assert(url.find("&signature=") != std::string::npos);

    // 参数应按字典序排列：secretid 排在 engine_model_type 之前是错的，应该是 e < s
    auto secretid_pos = url.find("secretid=");
    auto engine_pos = url.find("engine_model_type=");
    assert(engine_pos < secretid_pos);  // 'e' < 's'
}

void TestTencentResultParsing() {
    // slice_type=0 — 开始识别
    const char* json_start = R"(
    {
        "code": 0,
        "message": "success",
        "voice_id": "test-uuid",
        "result": {
            "slice_type": 0,
            "voice_text_str": ""
        }
    })";
    assert(AsrClientTencent::ExtractSliceType(json_start) == 0);
    assert(AsrClientTencent::ExtractVoiceText(json_start).empty());

    // slice_type=1 — 中间结果
    const char* json_partial = R"(
    {
        "code": 0,
        "message": "success",
        "result": {
            "slice_type": 1,
            "voice_text_str": "今天天气"
        }
    })";
    assert(AsrClientTencent::ExtractSliceType(json_partial) == 1);
    assert(AsrClientTencent::ExtractVoiceText(json_partial) == "今天天气");

    // slice_type=2 — 最终结果
    const char* json_final = R"(
    {
        "code": 0,
        "message": "success",
        "result": {
            "slice_type": 2,
            "voice_text_str": "今天天气很好"
        }
    })";
    assert(AsrClientTencent::ExtractSliceType(json_final) == 2);
    assert(AsrClientTencent::ExtractVoiceText(json_final) == "今天天气很好");

    // 错误响应
    const char* json_error = R"(
    {
        "code": 4002,
        "message": "鉴权失败"
    })";
    assert(AsrClientTencent::ExtractErrorCode(json_error) == 4002);
    assert(AsrClientTencent::ExtractErrorMessage(json_error) == "鉴权失败");

    // 词列表 segments
    const char* json_with_words = R"(
    {
        "code": 0,
        "message": "success",
        "result": {
            "slice_type": 1,
            "voice_text_str": "今天天气很好",
            "word_list": [
                {"word": "今天", "start_time": 0, "end_time": 400, "stable_flag": 1},
                {"word": "天气", "start_time": 400, "end_time": 700, "stable_flag": 1},
                {"word": "很好", "start_time": 700, "end_time": 1000, "stable_flag": 0}
            ]
        }
    })";
    std::set<std::string> emitted;
    auto segments = AsrClientTencent::ExtractWordListSegments(json_with_words, &emitted);
    // 只有 stable_flag=1 的词被聚合
    assert(segments.size() == 1);
    assert(segments[0].text == "今天天气");
    assert(segments[0].definite);
    assert(segments[0].start_time == 0);
    assert(segments[0].end_time == 700);

    // 再次提取应无新 segment（已去重）
    auto segments2 = AsrClientTencent::ExtractWordListSegments(json_with_words, &emitted);
    assert(segments2.empty());
}

void TestTencentVocabSyncOffMainThread() {
    // B7：热词表同步是纯网络操作（Find+Update/Create，WinHTTP 各阶段 10s 超时）。
    // Start 常被调用方持 audio_mutex_ 调用（HandleAudioFrame → SendOrBufferOggChunk
    // → Start），内联等待会把状态机/按键/音频帧一起冻结到超时（评审记录最长 ~80s）。
    // 注入 1200ms 慢同步，断言构造与 Start 都立即返回、在飞期间不重复起线程。
    std::atomic<int> sync_calls{0};
    AsrClientTencent::SetVocabSyncTestSeam(
        [&sync_calls](const AppConfig&, const std::vector<std::string>&) {
            sync_calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            return std::string("vocab-b7-test");
        });

    AppConfig cfg;
    cfg.tencent_appid = "1259000001";
    cfg.tencent_secret_id = "AKIDtestsecretid";
    cfg.tencent_secret_key = "testsecretkey";
    cfg.tencent_hotword_id.clear();   // 自动同步路径（配置指定 VocabId 时不同步）
    cfg.asr_hotwords = {"Opus", "ESP32-S3"};

    const auto ctor_at = std::chrono::steady_clock::now();
    auto client = std::make_unique<AsrClientTencent>(cfg);  // 构造即预热（锁外）
    const auto ctor_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - ctor_at).count();
    assert(ctor_ms < 600);  // 内联同步需 1200ms —— 构造必须不等它

    // 等预热线程真正进入同步体，确认"在飞"后再测 Start。
    for (int i = 0; i < 200 && sync_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(sync_calls.load() == 1);

    const auto start_at = std::chrono::steady_clock::now();
    AsrSessionOptions opts;  // hotwords 空 → 回落 config_.asr_hotwords
    const bool started = client->Start(opts);
    const auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start_at).count();
    assert(started);
    assert(start_ms < 600);     // 同步仍在飞（1200ms），Start 绝不能等它
    assert(sync_calls.load() == 1);  // 在飞期间重复 kick 幂等，不起第二个线程

    // 后台同步完成后结果通过 CachedVocabId 可见（有界等待）。
    for (int i = 0; i < 400 && client->CachedVocabId().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(client->CachedVocabId() == "vocab-b7-test");

    client->Cancel();
    client.reset();  // 析构 join websocket worker（有界，WinHTTP 超时兜底）
    AsrClientTencent::SetVocabSyncTestSeam(nullptr);
}

void TestTencentFinalFlagParsing() {
    // final=1：整段音频识别结束（顶层字段，无 result）
    const char* json_final_end = R"(
    {
        "code": 0,
        "message": "success",
        "voice_id": "test-uuid",
        "message_id": "test-uuid_241",
        "final": 1
    })";
    assert(AsrClientTencent::ExtractFinalFlag(json_final_end) == 1);

    // 无 final 字段（握手确认 / 普通识别结果）→ 0
    const char* json_handshake = R"(
    {
        "code": 0,
        "message": "success",
        "voice_id": "test-uuid"
    })";
    assert(AsrClientTencent::ExtractFinalFlag(json_handshake) == 0);

    const char* json_result = R"(
    {
        "code": 0,
        "message": "success",
        "result": {
            "slice_type": 2,
            "voice_text_str": "今天天气很好"
        }
    })";
    assert(AsrClientTencent::ExtractFinalFlag(json_result) == 0);
}

void TestTencentSentenceAccumulation() {
    // 空累积 + 首句 → 首句
    assert(AsrClientTencent::AccumulateSentence("", "今天天气很好") == "今天天气很好");
    // 已累积 + 新句 → 拼接
    assert(AsrClientTencent::AccumulateSentence("今天天气很好", "我们去看电影") == "今天天气很好我们去看电影");
    // 空句不改变累积
    assert(AsrClientTencent::AccumulateSentence("今天天气很好", "") == "今天天气很好");
    // 两者皆空 → 空
    assert(AsrClientTencent::AccumulateSentence("", "").empty());
}

void TestTencentEndMessage() {
    auto msg = AsrClientTencent::MakeEndMessage();
    assert(msg == R"({"type":"end"})");
}

void TestTencentOpusEncapsulation() {
    // 构造一个最小 Ogg Opus 音频页（不含有效 CRC，ExtractTencentOpusFrame 不校验 CRC）。
    // Ogg 页结构：OggS(4) + version(1) + type(1) + granule(8) + serial(4) + seq(4) + crc(4) + segments(1) + table(1) + payload
    std::vector<std::uint8_t> ogg_page;
    ogg_page.insert(ogg_page.end(), {'O', 'g', 'g', 'S', 0, 0});
    for (int i = 0; i < 8; ++i) ogg_page.push_back(0);  // granule
    for (int i = 0; i < 4; ++i) ogg_page.push_back(0);  // serial
    for (int i = 0; i < 4; ++i) ogg_page.push_back(0);  // seq
    for (int i = 0; i < 4; ++i) ogg_page.push_back(0);  // crc placeholder
    ogg_page.push_back(1);  // 1 segment
    ogg_page.push_back(4);  // segment length = 4
    ogg_page.insert(ogg_page.end(), {0x01, 0x02, 0x03, 0x04});  // fake Opus payload

    auto frame = AsrClientTencent::ExtractTencentOpusFrame(std::span(ogg_page));
    assert(frame.size() == 4 + 2 + 4);
    assert(frame[0] == 'o' && frame[1] == 'p' && frame[2] == 'u' && frame[3] == 's');
    assert(frame[4] == 0x04 && frame[5] == 0x00);  // little-endian length = 4
    assert(frame[6] == 0x01 && frame[7] == 0x02 && frame[8] == 0x03 && frame[9] == 0x04);

    // OpusHead/OpusTags 头包应返回空
    std::vector<std::uint8_t> head_page;
    head_page.insert(head_page.end(), {'O', 'g', 'g', 'S', 0, 0});
    for (int i = 0; i < 8; ++i) head_page.push_back(0);
    for (int i = 0; i < 4; ++i) head_page.push_back(0);
    for (int i = 0; i < 4; ++i) head_page.push_back(0);
    for (int i = 0; i < 4; ++i) head_page.push_back(0);
    head_page.push_back(1);
    head_page.push_back(8);
    head_page.insert(head_page.end(), {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'});
    auto head_frame = AsrClientTencent::ExtractTencentOpusFrame(std::span(head_page));
    assert(head_frame.empty());
}

void TestTencentVoiceIdGeneration() {
    auto id1 = AsrClientTencent::GenerateVoiceId();
    auto id2 = AsrClientTencent::GenerateVoiceId();
    // UUID 格式: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx (36 字符)
    assert(id1.size() == 36);
    assert(id2.size() == 36);
    assert(id1 != id2);  // 每次生成应不同
    assert(id1[8] == '-');
    assert(id1[13] == '-');
    assert(id1[18] == '-');
    assert(id1[23] == '-');
    assert(id1[14] == '4');  // UUID v4 版本标识
}

// ===== 腾讯 ASR 连接关停死锁回归（2026-09-10 UI 线程卡死 30s+ 事故）=====
// 事故链：服务端 final=1 后未断开 → worker 无限期阻塞在 WinHttpWebSocketReceive
//（WinHttpSetTimeouts 的接收超时对 WebSocket Receive 不生效）→ 用户保存设置时
// UI 线程同步析构旧客户端 → ShutdownConnection 持锁跨线程调用 WinHttpWebSocketClose
//（与 worker 并发操作同一句柄 + 等待对端 close ack）+ join 卡死的 worker → UI 线程
// 停摆 → 小米 ATVV 事件（全部依赖 UI 线程 DispatchToUiThread 分发）静默丢弃。
namespace tencent_seam {
    std::atomic<int> receive_calls = 0;
    std::atomic<int> websocket_close_calls = 0;
    std::atomic<int> handle_close_calls = 0;

    void Reset() {
        receive_calls.store(0);
        websocket_close_calls.store(0);
        handle_close_calls.store(0);
    }

    // 第 1 次返回 final=1 文本帧；其后模拟服务端不断开——Receive 长时间不返回
    //（线上死锁场景，超时只是防测试进程挂死的保险）。
    DWORD WINAPI FakeReceive(HINTERNET, PVOID buffer, DWORD, DWORD* bytes_read,
                             WINHTTP_WEB_SOCKET_BUFFER_TYPE* type) {
        const int n = receive_calls.fetch_add(1) + 1;
        if (n == 1) {
            const char* json =
                R"({"code":0,"message":"success","voice_id":"t","message_id":"t","final":1})";
            const std::size_t len = std::strlen(json);
            std::memcpy(buffer, json, len);
            *bytes_read = static_cast<DWORD>(len);
            *type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            return ERROR_SUCCESS;
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return ERROR_WINHTTP_TIMEOUT;
    }

    DWORD WINAPI FakeWebSocketClose(HINTERNET, USHORT, PVOID, DWORD) {
        websocket_close_calls.fetch_add(1);
        return ERROR_SUCCESS;
    }

    BOOL WINAPI FakeHandleClose(HINTERNET) {
        handle_close_calls.fetch_add(1);
        return TRUE;
    }
} // namespace tencent_seam

// 方向 1：final=1（on_final 已触发）后接收循环必须主动退出，不得继续阻塞等
// 服务端断开。旧实现卡在第二次 Receive → loop_done 永不置位 → assert 失败。
void TestTencentReceiveLoopExitsAfterFinalEmitted() {
    using namespace tencent_seam;
    Reset();
    AsrClientTencent client(AppConfig::Defaults());
    std::atomic<bool> final_seen{false};
    client.on_final = [&](std::string) { final_seen = true; };
    client.SetWinHttpTestSeams(&FakeReceive, &FakeWebSocketClose, &FakeHandleClose);

    std::atomic<bool> loop_done{false};
    std::thread loop([&] {
        client.ReceiveLoop(reinterpret_cast<HINTERNET>(1));
        loop_done = true;
    });
    // 修复后毫秒级退出；未修复阻塞在第二次 Receive，2 秒内 loop_done 仍为 false。
    for (int i = 0; i < 100 && !loop_done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(loop_done.load());
    loop.join();
    assert(final_seen.load());          // on_final 已随 final=1 触发
    assert(receive_calls.load() == 1);  // 第二次 Receive 未发生（循环已退出）
}

// 方向 2：ShutdownConnection 必须用 WinHttpCloseHandle 强制拆除（令阻塞中的
// Receive 立即返回 OPERATION_CANCELLED），不得调用 WinHttpWebSocketClose
//（close 握手要等服务端 ack，且与 worker 的 Receive 并发操作同一句柄）。
void TestTencentShutdownForcesHandleCloseNotWebSocketClose() {
    using namespace tencent_seam;
    Reset();
    AsrClientTencent client(AppConfig::Defaults());
    client.SetWinHttpTestSeams(&FakeReceive, &FakeWebSocketClose, &FakeHandleClose);
    client.SetWebSocketHandleForTest(reinterpret_cast<HINTERNET>(42));

    client.ShutdownConnection();

    assert(websocket_close_calls.load() == 0);  // 旧实现恒调 → 红灯
    assert(handle_close_calls.load() == 1);     // 强制关闭路径执行一次
}

// 方向 3：UpdateConfig 替换云端客户端时，旧客户端析构（可能 join 卡死的
// WebSocket worker）必须移出调用线程——曾在 UI 线程同步析构致使其停摆。

// Suite entry: core_tests.cc main() calls this once.
void RunTencentBatchTests() {
    TestTencentProviderSelection();
    TestTencentConfigRoundTrip();
    TestTencentCredentialsTrimmedOnLoad();
    TestTencentSecretIdRecoveryFromVolcengineField();
    TestTencentSignatureGeneration();
    TestTencentUrlConstruction();
    TestTencentResultParsing();
    TestTencentVocabSyncOffMainThread();
    TestTencentFinalFlagParsing();
    TestTencentSentenceAccumulation();
    TestTencentEndMessage();
    TestTencentOpusEncapsulation();
    TestTencentVoiceIdGeneration();
    TestTencentReceiveLoopExitsAfterFinalEmitted();
    TestTencentShutdownForcesHandleCloseNotWebSocketClose();
}

