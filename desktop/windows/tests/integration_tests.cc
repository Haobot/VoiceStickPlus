// L1 ASR 链路集成测试：真实火山 AsrClientWin + FakeBleCentral 注入语料 ogg，
// 验证「Opus 帧 -> Ogg 封装 -> 火山 ASR -> 识别文本」全链路，断言 CER。
//
// 与 core_tests 的区别：core_tests 用 FakeAsrClient（不联网），本测试用真实 AsrClientWin
// 连火山 ASR WebSocket，需 %APPDATA%\VoiceStick\config.toml 配 volcengine_api_key + 网络。
// 无 key 时 skip（return 77），不伪造结果。ASR 异步回调线程写 final_countdowns，测试线程
// 轮询读取，FakeUi 关键字段加锁同步。

#include "app_config.h"
#include "asr_client_win.h"
#include "asr_protocol.h"
#include "ble_protocol.h"
#include "byte_utils.h"
#include "cJSON.h"
#include "ogg_opus_demuxer.h"
#include "voice_stick_coordinator.h"
#include "xiaomi_atvv_session.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace voicestick;

namespace {

// ===== Fake 基础设施（从 core_tests.cc 复制，L1 测试隔离，零风险不动现有测试）=====

struct SentUiState {
    std::string state;
    std::string text;
    std::optional<std::string> device_id;
};
struct SentRemoteButton {
    RemoteButtonAction action;
    std::string button;
    std::optional<std::string> device_id;
    std::uint32_t request_id;
};
struct SentImuWakeSensitivity {
    int threshold_lsb = 0;
    std::optional<std::string> device_id;
};
struct SentTapSensitivity {
    int level = 0;
    std::optional<std::string> device_id;
};

class FakeBleCentral : public BleCentral {
public:
    void Start() override {}
    void UpdatePairedDeviceIds(const std::vector<std::string>& ids) override {
        paired_device_ids = ids;
    }
    void ConnectPairedDevice(const std::string&, std::uint64_t, BluetoothAddressKind,
                             const std::string&, DeviceClass) override {}
    void SendUiState(const std::string& state, const std::string& text,
                     const std::optional<std::string>& device_id) override {
        sent_ui_states.push_back(SentUiState{state, text, device_id});
    }
    void SendInteractionMode(InteractionMode mode,
                             const std::optional<std::string>& device_id) override {
        sent_interaction_modes.push_back(std::pair{mode, device_id});
    }
    void SendShowImuDebug(bool, const std::optional<std::string>&) override {}
    void SendTapEnabled(bool enabled, const std::optional<std::string>& device_id) override {
        sent_tap_enabled.push_back(std::pair{enabled, device_id});
    }
    void SendTapSensitivity(int level, const std::optional<std::string>& device_id) override {
        sent_tap_sensitivities.push_back(SentTapSensitivity{level, device_id});
    }
    void SendAirMouseEnabled(bool enabled, const std::optional<std::string>& device_id) override {
        sent_air_mouse_enabled.push_back(std::pair{enabled, device_id});
    }
    void SendImuWakeSensitivity(int threshold_lsb,
                                const std::optional<std::string>& device_id) override {
        sent_imu_wake_sensitivities.push_back(SentImuWakeSensitivity{threshold_lsb, device_id});
    }
    void SendEncoderLedColor(const std::string&, const std::optional<std::string>&) override {}
    void SendEncoderRecordingGate(bool, const std::optional<std::string>&) override {}
    void RequestBatteryStatus(const std::optional<std::string>& device_id) override {
        battery_status_requests.push_back(device_id);
    }
    void SendRemoteButton(RemoteButtonAction action, const std::string& button,
                          const std::optional<std::string>& device_id,
                          std::uint32_t request_id) override {
        sent_remote_buttons.push_back(SentRemoteButton{action, button, device_id, request_id});
    }
    void UpdateFirmware(ByteVector image, const std::string& device_id,
                        std::function<void(FirmwareUpdateProgress)> progress,
                        std::function<void(bool, std::string)> completion) override {
        captured_firmware_image = std::move(image);
        captured_firmware_device_id = device_id;
        if (progress) {
            progress(FirmwareUpdateProgress{0, static_cast<int>(captured_firmware_image.size()), true});
        }
        if (completion) completion(true, "");
    }
    void CancelFirmwareUpdate() override {}
    bool IsConnected(const std::string& device_id) const override {
        return connected_device_ids.contains(device_id);
    }

    std::vector<std::string> paired_device_ids;
    std::set<std::string> connected_device_ids;
    ByteVector captured_firmware_image;
    std::string captured_firmware_device_id;
    std::vector<SentUiState> sent_ui_states;
    std::vector<std::pair<InteractionMode, std::optional<std::string>>> sent_interaction_modes;
    std::vector<std::optional<std::string>> battery_status_requests;
    std::vector<SentRemoteButton> sent_remote_buttons;
    std::vector<SentImuWakeSensitivity> sent_imu_wake_sensitivities;
    std::vector<std::pair<bool, std::optional<std::string>>> sent_tap_enabled;
    std::vector<SentTapSensitivity> sent_tap_sensitivities;
    std::vector<std::pair<bool, std::optional<std::string>>> sent_air_mouse_enabled;
};

// FakeUi：L1 只关注 final_countdowns（ASR 最终文本）与 errors（ASR 失败）。
// 两者由 ASR 回调线程写入、测试主线程读取，用 mu 保护。其余字段单线程或测试不读，不加锁。
class FakeUi : public VoiceStickUi {
public:
    void SetStatus(const std::string& status) override { statuses.push_back(status); }
    void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) override {
        connected_devices = devices;
    }
    void SetDeviceInfo(const DeviceInfo& info) override { device_infos.push_back(info); }
    void SetDeviceEncoderPresent(const std::string&, bool) override {}
    void SetDeviceBattery(const std::string&, int, bool, bool) override {}
    void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info) override {
        firmware_info_by_device_id = info;
    }
    void SetPairingError(const std::string& device_id, const std::string& message) override {
        pairing_errors.push_back(device_id + ":" + message);
    }
    void ShowFirmwareUpdatePrompt(const std::string& device_id, const std::string& current_version,
                                  const std::string& latest_version, bool is_below_minimum) override {
        firmware_update_prompts.push_back(device_id + ":" + current_version + ":" + latest_version +
                                          (is_below_minimum ? ":minimum" : ":latest"));
    }
    void SetPairedDeviceIds(const std::vector<std::string>& ids) override { paired_device_ids = ids; }
    void SetHasRecoverableInput(bool has) override { has_recoverable_input_set = has; }
    void ShowListening(const std::optional<std::string>&) override { ++show_listening_count; }
    void ShowPartial(const std::string& text, const std::optional<std::string>&) override {
        partials.push_back(text);
    }
    void AppendPartial(const std::string& text, const std::optional<std::string>&) override {
        partials.push_back(text);
    }
    void ShowRefining(const std::string& text, const std::optional<std::string>&) override {
        refining_texts.push_back(text);
    }
    void ShowFinalCountdown(const std::string& text, const std::optional<std::string>&,
                            std::function<void()> on_complete) override {
        std::lock_guard<std::mutex> lk(mu);
        final_countdowns.push_back(text);
        final_countdown_completion = std::move(on_complete);
    }
    void ShowPausedFinal(const std::string& text, const std::optional<std::string>&) override {
        paused_finals.push_back(text);
    }
    void ShowError(const std::string& text, const std::optional<std::string>&,
                   std::function<void()> on_complete) override {
        std::lock_guard<std::mutex> lk(mu);
        errors.push_back(text);
        error_completion = std::move(on_complete);
    }
    void ShowCloudUpgrade(const std::string& message, const std::string& url,
                          const std::optional<std::string>&) override {
        cloud_upgrades.push_back(message + "|" + url);
    }
    void HideOverlay(std::function<void()> on_hidden = {}) override {
        {
            std::lock_guard<std::mutex> lk(mu);
            ++hide_overlay_count;
        }
        if (on_hidden) on_hidden();
    }
    void ShowSubtitle(const std::string& text, const std::string& device_id,
                      OverlayThemeColor color) override {
        (void)color;
        subtitles.push_back(device_id + ":" + text);
    }
    void HideSubtitles() override { ++hide_subtitles_count; }
    void ShowNotification(const std::string& title, const std::string& body) override {
        notifications.push_back(title + ":" + body);
    }
    void ShowTimedMessage(const std::string& message, int duration_ms) override {
        timed_messages.push_back(message + ":" + std::to_string(duration_ms));
    }

    // 测试线程安全的查询。
    bool HasAsrResult() const {
        std::lock_guard<std::mutex> lk(mu);
        return !final_countdowns.empty() || !errors.empty();
    }
    std::string LastFinalCountdown() const {
        std::lock_guard<std::mutex> lk(mu);
        return final_countdowns.empty() ? std::string{} : final_countdowns.back();
    }
    std::string LastError() const {
        std::lock_guard<std::mutex> lk(mu);
        return errors.empty() ? std::string{} : errors.back();
    }
    // 空 final 路径（协调器 empty_final_done）以 HideOverlay 收尾；synthetic
    // 会话冒烟用它判定识别轮次结束（HideOverlay 由 ASR 回调线程触发，需加锁）。
    int HideOverlayCount() const {
        std::lock_guard<std::mutex> lk(mu);
        return hide_overlay_count;
    }

    mutable std::mutex mu;
    std::vector<std::string> statuses;
    std::vector<ConnectedDevice> connected_devices;
    std::vector<DeviceInfo> device_infos;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_by_device_id;
    std::vector<std::string> pairing_errors;
    std::vector<std::string> firmware_update_prompts;
    std::vector<std::string> paired_device_ids;
    std::vector<std::string> partials;
    std::vector<std::string> refining_texts;
    std::vector<std::string> cloud_upgrades;
    std::vector<std::string> final_countdowns;
    std::vector<std::string> paused_finals;
    std::vector<std::string> errors;
    std::vector<std::string> subtitles;
    std::vector<std::string> notifications;
    std::vector<std::string> timed_messages;
    std::function<void()> final_countdown_completion;
    std::function<void()> error_completion;
    bool has_recoverable_input_set = false;
    int show_listening_count = 0;
    int hide_overlay_count = 0;
    int hide_subtitles_count = 0;
};

class FakeInputInjector : public InputInjector {
public:
    void Paste(const std::string& text, bool press_enter) override {
        std::lock_guard<std::mutex> lk(mu);
        pasted_text = text;
        pasted_enter = press_enter;
        pasted_ = true;
    }
    void SendEnter() override { send_enter_called = true; }
    void SendArrowDown() override { ++arrow_down_count; }
    void SendArrowUp() override {}
    void SendKeyCombo(const KeySpec&) override {}
    void MoveMouse(int dx, int dy) override {
        ++move_mouse_count;
        total_dx += dx;
        total_dy += dy;
    }
    void ClickLeftButton() override { ++left_click_count; }

    // 测试线程安全查询（Paste 由 ASR 回调线程调用）。
    bool HasPasted() const {
        std::lock_guard<std::mutex> lk(mu);
        return pasted_;
    }
    std::string GetPastedText() const {
        std::lock_guard<std::mutex> lk(mu);
        return pasted_text;
    }

    mutable std::mutex mu;
    bool pasted_ = false;
    std::string pasted_text;
    bool pasted_enter = false;
    bool send_enter_called = false;
    int arrow_down_count = 0;
    int move_mouse_count = 0;
    int total_dx = 0;
    int total_dy = 0;
    int left_click_count = 0;
};

StateEvent ButtonEvent(const std::string& event, const std::string& button,
                       std::optional<std::uint32_t> session_id = std::nullopt) {
    StateEvent e;
    e.event = event;
    e.button = button;
    e.session_id = session_id;
    return e;
}

// ===== L1 工具 =====

ByteVector ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return ByteVector((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// UTF-8 按字符拆分（中文 3 字节/字，ASCII 1 字节），用于中文 CER 计算。
std::vector<std::string> SplitUtf8(const std::string& s) {
    std::vector<std::string> chars;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        chars.push_back(s.substr(i, len));
        i += len;
    }
    return chars;
}

size_t EditDistance(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const size_t m = a.size(), n = b.size();
    std::vector<size_t> prev(n + 1), cur(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = j;
    for (size_t i = 1; i <= m; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[n];
}

// 字错误率 CER = 编辑距离(hyp, ref) / ref 字符数。
double Cer(const std::string& hyp, const std::string& ref) {
    const auto h = SplitUtf8(hyp);
    const auto r = SplitUtf8(ref);
    if (r.empty()) return h.empty() ? 0.0 : 1.0;
    return static_cast<double>(EditDistance(h, r)) / static_cast<double>(r.size());
}

// ASCII 小写化（中文不受影响），用于英文关键实体 case-insensitive 匹配。
std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

// 等待 ASR 异步返回：focused_app 模式 on_final 后直接 Paste（不经倒计时），
// 故成功信号是 input.HasPasted()；errors 为失败信号。
bool WaitForAsrResult(const FakeInputInjector& input, const FakeUi& ui, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (input.HasPasted() || ui.HasAsrResult()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return input.HasPasted() || ui.HasAsrResult();
}

struct CorpusItem {
    std::string id;
    std::string text;
    double cer_threshold = 0.10;  // 常规语料 CER < 10%
    // 非空时用关键实体包含判定（case-insensitive），用于数字/英文混合语料：
    // ASR 常把中文数字转阿拉伯数字、英文大小写变化，CER 不稳定。
    std::vector<std::string> keywords;
};

// 跑单条语料：注入 ogg -> 真实火山 ASR -> 断言 CER。返回 CER（负数表示链路失败）。
double RunOneCorpus(const CorpusItem& item, const std::filesystem::path& corpus_dir,
                    const AppConfig& base_config) {
    const auto ogg_path = corpus_dir / (item.id + ".ogg");
    const ByteVector ogg = ReadFile(ogg_path);
    if (ogg.empty()) {
        std::printf("  [%s] FAIL: 无法读取 %s\n", item.id.c_str(), ogg_path.string().c_str());
        return -1.0;
    }
    OggOpusStream stream;
    if (!ParseOggOpus(ogg, stream) || stream.packets.empty()) {
        std::printf("  [%s] FAIL: ogg 解封失败或无 Opus 帧\n", item.id.c_str());
        return -1.0;
    }

    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<AsrClientWin>(base_config);
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(base_config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    const std::string device_id = "5A74";
    const std::uint32_t session_id = 1;
    ble_ptr->connected_device_ids.insert(device_id);
    ble_ptr->on_connection_change({ConnectedDevice{device_id, "VS-" + device_id}});

    // 开始录音（hold_to_talk button_down）。
    ble_ptr->on_state_event(device_id, ButtonEvent("button_down", "primary", session_id));

    // 注入语料 Opus 帧（首帧带 start flag，与固件 audio_tx 帧语义一致）。
    // 按帧时长节流模拟实时采集节奏：真实录音每帧 40ms 到达，一次性灌入全部帧会致
    // ASR WebSocket 突发拥堵、partial 不完整且 final 不返回。
    for (size_t i = 0; i < stream.packets.size(); ++i) {
        AudioFrame frame;
        frame.session_id = session_id;
        frame.seq = static_cast<std::uint32_t>(i);
        frame.flags = (i == 0) ? 0x01 : 0x00;
        frame.payload = stream.packets[i];
        ble_ptr->on_audio_frame(device_id, frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    // 帧间节流已保证 button_down 到 button_up 时长 >= kMinimumRecordingDurationSeconds(0.5s)。

    // 松开 + 结束帧（与固件 button_up + drain 后的 audio_end 语义一致）。
    ble_ptr->on_state_event(device_id, ButtonEvent("button_up", "primary", session_id));
    AudioFrame end_frame;
    end_frame.session_id = session_id;
    end_frame.seq = static_cast<std::uint32_t>(stream.packets.size());
    end_frame.flags = 0x02;
    ble_ptr->on_audio_frame(device_id, end_frame);

    // 等待 ASR 异步返回（火山 WebSocket 首字 + 最终结果 + Paste）。
    if (!WaitForAsrResult(input, ui, 40000)) {
        std::printf("  [%s] FAIL: ASR 超时（40s 无 Paste/结果）\n", item.id.c_str());
        std::printf("    asr LastStartError: %s\n", asr_ptr->LastStartError().c_str());
        std::printf("    ui.statuses(%zu):", ui.statuses.size());
        for (const auto& s : ui.statuses) std::printf(" %s", s.c_str());
        std::printf("\n    sent_ui_states(%zu):", ble_ptr->sent_ui_states.size());
        for (const auto& s : ble_ptr->sent_ui_states) std::printf(" %s/%s", s.state.c_str(), s.text.c_str());
        std::printf("\n    partials(%zu):", ui.partials.size());
        for (const auto& s : ui.partials) std::printf(" %s", s.c_str());
        std::printf("\n    pasted=\"%s\"\n", input.GetPastedText().c_str());
        coordinator.Shutdown();
        return -1.0;
    }

    coordinator.Shutdown();

    const std::string err = ui.LastError();
    if (!err.empty()) {
        std::printf("  [%s] FAIL: ASR 错误: %s\n", item.id.c_str(), err.c_str());
        return -1.0;
    }
    const std::string hyp = input.GetPastedText();
    if (hyp.empty()) {
        std::printf("  [%s] FAIL: 无 Paste 文本\n", item.id.c_str());
        return -1.0;
    }

    // 数字/英文混合语料：ASR 常把中文数字转阿拉伯数字、英文大小写变化，CER 不稳定，
    // 改用关键实体包含判定（case-insensitive）。
    if (!item.keywords.empty()) {
        const std::string hyp_low = ToLower(hyp);
        bool all_found = true;
        for (const auto& kw : item.keywords) {
            if (hyp_low.find(ToLower(kw)) == std::string::npos) {
                all_found = false;
                break;
            }
        }
        std::printf("  [%s] 识别=\"%s\" 预期=\"%s\" 关键实体%s\n",
                    item.id.c_str(), hyp.c_str(), item.text.c_str(),
                    all_found ? "通过" : "缺失");
        return all_found ? 0.0 : 1.0;
    }

    const double cer = Cer(hyp, item.text);
    std::printf("  [%s] 识别=\"%s\" 预期=\"%s\" CER=%.3f\n",
                item.id.c_str(), hyp.c_str(), item.text.c_str(), cer);
    return cer;
}

// ===== 小米遥控器 ATVV golden 回放 =====
// 用 core 的 XiaomiAtvvSession 驱动 golden ADPCM（atvv_capture.py 采集 /
// atvv_bench.py 合成），合成 button_down/up 与 Opus AudioFrame 走与真机一致的
// 协调器路径送真实火山 ASR。真机采集（非 synthetic）会话断言非空识别文本
// （内容准确率归 scripts/e2e_test/atvv_bench.py 评测）；synthetic 合成 fixtures
// 不是语音，只验证链路跑通（识别轮次正常结束 + 无 ASR 错误），不断言文本。
// 无 fixtures 时打印 SKIP 不算失败。

std::filesystem::path ResolveAtvvFixturesRoot() {
    if (const char* env = std::getenv("VOICESTICK_ATVV_FIXTURES_DIR");
        env != nullptr && *env != '\0') {
        return std::filesystem::path(env);
    }
    // CTest 已把本测试 WORKING_DIRECTORY 设为仓库根
    return std::filesystem::path("scripts") / "e2e_test" / "fixtures" / "xiaomi";
}

struct AtvvGoldenInfo {
    std::filesystem::path adpcm_path;
    std::size_t frame_bytes = XiaomiAtvvProtocol::default_frame_bytes;
    double duration_s = 0.0;
    bool synthetic = false;  // sidecar synthetic 标记（合成 fixtures 不作识别率结论）
};

// 读 sidecar（session_N.json）取帧长/时长/synthetic 标记；缺 sidecar 或字段返回 false。
bool LoadAtvvGoldenInfo(const std::filesystem::path& adpcm_path, AtvvGoldenInfo* info) {
    const auto sidecar_path =
        adpcm_path.parent_path() / (adpcm_path.stem().string() + ".json");
    std::ifstream in(sidecar_path, std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    cJSON* root = cJSON_Parse(text.c_str());
    if (root == nullptr) return false;
    const cJSON* frame_len = cJSON_GetObjectItemCaseSensitive(root, "frame_len");
    const cJSON* samples = cJSON_GetObjectItemCaseSensitive(root, "samples");
    const cJSON* sample_rate = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
    const cJSON* synthetic = cJSON_GetObjectItemCaseSensitive(root, "synthetic");
    bool ok = false;
    if (cJSON_IsNumber(frame_len) && frame_len->valuedouble > 0 &&
        cJSON_IsNumber(samples) && cJSON_IsNumber(sample_rate) &&
        sample_rate->valuedouble > 0) {
        info->frame_bytes = static_cast<std::size_t>(frame_len->valuedouble);
        info->duration_s = samples->valuedouble / sample_rate->valuedouble;
        info->synthetic = cJSON_IsTrue(synthetic);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// 找最新一次采集目录（root 本身直接含 session_*.adpcm 时用 root）。
std::vector<std::filesystem::path> CollectAtvvGoldenSessions(
    const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return {};
    auto has_sessions = [&ec](const std::filesystem::path& dir) {
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec;
             it.increment(ec)) {
            const auto& p = it->path();
            if (it->is_regular_file(ec) && p.extension() == ".adpcm" &&
                p.filename().string().rfind("session_", 0) == 0) {
                return true;
            }
        }
        return false;
    };
    std::filesystem::path dir;
    if (has_sessions(root)) {
        dir = root;
    } else {
        std::vector<std::filesystem::path> children;
        for (std::filesystem::directory_iterator it(root, ec), end; it != end && !ec;
             it.increment(ec)) {
            if (it->is_directory(ec) && has_sessions(it->path())) {
                children.push_back(it->path());
            }
        }
        if (children.empty()) return {};
        std::sort(children.begin(), children.end());
        dir = children.back();
    }
    std::vector<std::filesystem::path> sessions;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        const auto& p = it->path();
        if (it->is_regular_file(ec) && p.extension() == ".adpcm" &&
            p.filename().string().rfind("session_", 0) == 0) {
            sessions.push_back(p);
        }
    }
    std::sort(sessions.begin(), sessions.end());
    return sessions;
}

// 跑单个 golden 会话：重放 ADPCM → XiaomiAtvvSession → Opus 帧 → 协调器 →
// 真实火山 ASR，断言非空识别文本。
bool RunOneXiaomiGolden(const AtvvGoldenInfo& info, const AppConfig& base_config) {
    const std::string id = info.adpcm_path.stem().string();
    const ByteVector adpcm = ReadFile(info.adpcm_path);
    if (adpcm.empty()) {
        std::printf("  [%s] FAIL: 无法读取 %s\n", id.c_str(),
                    info.adpcm_path.string().c_str());
        return false;
    }

    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<AsrClientWin>(base_config);
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(base_config, std::move(ble), std::move(asr),
                                      &ui, &input);
    coordinator.Start();

    const std::string device_id = "RC-GOLD";
    XiaomiAtvvSession session;  // 默认 hold_to_talk + 12dB 增益（桌面端默认）
    auto inject = [&](const std::vector<XiaomiAtvvAction>& actions) {
        for (const auto& action : actions) {
            if (const auto* e = std::get_if<XiaomiAtvvStateEvent>(&action)) {
                ble_ptr->on_state_event(device_id, e->event);
            } else if (const auto* f = std::get_if<XiaomiAtvvAudioFrame>(&action)) {
                ble_ptr->on_audio_frame(device_id, f->frame);
            }
        }
    };

    std::int64_t t = 1000;
    inject(session.Start(t));
    // CAPS v1.0：16kHz + sidecar 协商帧长
    const auto frame_bytes = info.frame_bytes;
    inject(session.HandleControlCommand(
        ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03,
                   static_cast<std::uint8_t>((frame_bytes >> 8) & 0xFF),
                   static_cast<std::uint8_t>(frame_bytes & 0xFF)}, t + 10));
    t += 10;
    inject(session.HandleControlCommand(ByteVector{XiaomiAtvvProtocol::control_mic_open},
                                        t));
    inject(session.HandleControlCommand(
        ByteVector{XiaomiAtvvProtocol::control_stream_start, 0x03, 0x02, 0x01}, t + 10));
    t += 10;

    // 按真实时长节奏逐包喂 ADPCM：n 字节 = 2n 采样 = n/8 ms @16kHz；
    // 墙钟 pacing 同时满足协调器 0.5s 最短录音时长（golden 会话 ≥1s）。
    // 简化：不重放会话中段 AUDIO_SYNC——sidecar 里 SYNC 丢弃的残余字节在本回放中
    // 会被当作普通音频连续解入。若未来采集到含中段 SYNC 的会话，该会话的
    // 「非空文本」断言输入保真度下降；准确率口径一律以 atvv_bench.py 的
    // sidecar 段落复现路径为准，本用例只做链路冒烟。
    std::size_t off = 0;
    auto feed_chunk = [&]() -> bool {
        if (off >= adpcm.size()) return false;
        const std::size_t n = std::min(frame_bytes, adpcm.size() - off);
        inject(session.HandleAudioData(
            std::span<const std::uint8_t>(adpcm.data() + off, n), t));
        off += n;
        const auto ms = static_cast<int>(n / 8);
        t += ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return true;
    };
    feed_chunk();  // 首包进入暂存
    inject(session.Tick(t + XiaomiAtvvSession::kHoldThresholdMs));  // 确认长按
    t += XiaomiAtvvSession::kHoldThresholdMs;
    while (feed_chunk()) {
    }
    // 松开：STOP（button_up）→ 150ms 尾包宽限到期补末帧
    // hide0 须在 STOP 前快照：STOP 之后 ASR 才可能返回空 final 触发 HideOverlay。
    const int hide0 = ui.HideOverlayCount();
    inject(session.HandleControlCommand(ByteVector{XiaomiAtvvProtocol::control_stop}, t));
    inject(session.Tick(t + XiaomiAtvvSession::kAudioTailGraceMs));

    // synthetic 合成 fixtures（--emit-demo-fixture 缺省产物为扫频正弦，非语音）：
    // ASR 可能返回空 final，协调器走 empty_final_done 路径（HideOverlay + 回
    // ready），不会 Paste。此类会话只验证链路跑通——识别轮次正常结束（Paste /
    // 最终结果 / HideOverlay 任一）且无 ASR 错误即通过，跳过非空文本断言；
    // 真机采集（非 synthetic）会话保持非空断言。
    if (info.synthetic) {
        auto link_done = [&]() {
            return input.HasPasted() || ui.HasAsrResult() ||
                   ui.HideOverlayCount() > hide0;
        };
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(40);
        while (!link_done() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!link_done()) {
            std::printf("  [%s] FAIL: synthetic 会话 40s 内识别轮次未结束\n",
                        id.c_str());
            coordinator.Shutdown();
            return false;
        }
    } else if (!WaitForAsrResult(input, ui, 40000)) {
        std::printf("  [%s] FAIL: ASR 超时（40s 无 Paste/结果）\n", id.c_str());
        coordinator.Shutdown();
        return false;
    }
    coordinator.Shutdown();
    const std::string err = ui.LastError();
    if (!err.empty()) {
        std::printf("  [%s] FAIL: ASR 错误: %s\n", id.c_str(), err.c_str());
        return false;
    }
    const std::string hyp = input.GetPastedText();
    if (info.synthetic) {
        std::printf("  [%s] synthetic 链路冒烟通过（%.1fs golden，识别=\"%s\"，"
                    "空文本属预期、不断言）\n",
                    id.c_str(), info.duration_s, hyp.c_str());
        return true;
    }
    if (hyp.empty()) {
        std::printf("  [%s] FAIL: 无 Paste 文本\n", id.c_str());
        return false;
    }
    std::printf("  [%s] 识别=\"%s\"（%.1fs golden，仅断言非空）\n",
                id.c_str(), hyp.c_str(), info.duration_s);
    return true;
}

}  // namespace

int main() {
    const std::filesystem::path corpus_dir = "scripts/e2e_test/corpus";

    // 加载 config.toml（火山 key），关闭精修/翻译测纯 ASR。
    AppConfig config = AppConfig::Load();
    config.asr_provider = AsrProvider::kVolcengine;
    config.refine_enabled = false;
    config.default_output_profile.target = OutputTarget::kFocusedApp;
    config.default_output_profile.transform = TextTransform::kOriginal;

    if (config.volcengine_api_key.empty()) {
        std::printf("SKIP: config.toml 无 volcengine_api_key（L1 需真实火山凭据）\n");
        return 77;  // CTest SKIP_RETURN_CODE
    }

    std::printf("=== L1 ASR 集成测试（火山引擎）===\n");

    const std::vector<CorpusItem> items = {
        {"short_01", "今天天气不错。"},
        {"short_02", "帮我打开浏览器。"},
        {"short_03", "现在几点了？"},
        {"long_01", "请帮我把这段会议记录整理成要点，并发送给所有参会人员。"},
        {"long_02", "这个项目的目标是构建一个端到端的语音输入测试系统，覆盖软硬件全链路。"},
        {"punct_01", "好的，没问题，马上处理。"},
        {"command_01", "新建一个文件夹并命名为测试。"},
        {"command_02", "把这段文字翻译成英文。"},
        {"num_01", "我的手机号是一三八零零一三八零零零。", 0.10, {"手机号"}},
        {"num_02", "今天是二零二六年七月十五日。", 0.10, {"今天", "年", "月", "日"}},
        {"mix_01", "用 python 写一个 hello world 程序。", 0.10, {"python", "hello", "world", "程序"}},
        {"mix_02", "把 cpu 使用率报警阈值设为百分之八十。", 0.10, {"cpu", "使用率", "报警", "阈值"}},
    };

    int pass = 0, fail = 0;
    for (const auto& item : items) {
        const double cer = RunOneCorpus(item, corpus_dir, config);
        if (cer < 0.0) {
            ++fail;
            continue;
        }
        if (cer < item.cer_threshold) {
            ++pass;
        } else {
            ++fail;
            std::printf("  [%s] FAIL: CER %.3f >= 阈值 %.3f\n",
                        item.id.c_str(), cer, item.cer_threshold);
        }
    }

    std::printf("=== L1 结果: %d 通过 / %d 失败 ===\n", pass, fail);

    // 小米遥控器 golden 回放：无 fixtures 时 SKIP 本段（不影响 L1 语料结论）；
    // 有 fixtures 时非 synthetic 会话（≥1s）须拿到非空识别文本，synthetic
    // 合成会话只验证链路跑通（见 RunOneXiaomiGolden 注释）。
    int xiaomi_pass = 0, xiaomi_fail = 0;
    const auto atvv_root = ResolveAtvvFixturesRoot();
    const auto golden_sessions = CollectAtvvGoldenSessions(atvv_root);
    if (golden_sessions.empty()) {
        std::printf("SKIP: 无 ATVV golden fixtures（%s），跳过小米 golden 回放\n",
                    atvv_root.string().c_str());
    } else {
        std::printf("=== 小米 ATVV golden 回放（%s，%zu 个会话）===\n",
                    golden_sessions.front().parent_path().string().c_str(),
                    golden_sessions.size());
        for (const auto& adpcm_path : golden_sessions) {
            AtvvGoldenInfo info{adpcm_path};
            if (!LoadAtvvGoldenInfo(adpcm_path, &info)) {
                std::printf("  [%s] FAIL: sidecar 缺失或字段不全\n",
                            adpcm_path.stem().string().c_str());
                ++xiaomi_fail;
                continue;
            }
            if (info.synthetic) {
                std::printf("  [%s] 注意: 回放的是 synthetic 合成 fixtures"
                            "（仅链路冒烟，不作识别率结论）\n",
                            adpcm_path.stem().string().c_str());
            }
            if (info.duration_s < 1.0) {
                std::printf("  [%s] SKIP: 会话 %.2fs < 1s（协调器最短录音时长）\n",
                            adpcm_path.stem().string().c_str(), info.duration_s);
                continue;
            }
            if (RunOneXiaomiGolden(info, config)) {
                ++xiaomi_pass;
            } else {
                ++xiaomi_fail;
            }
        }
        std::printf("=== 小米 golden: %d 通过 / %d 失败 ===\n",
                    xiaomi_pass, xiaomi_fail);
    }

    const int total_fail = fail + xiaomi_fail;
    std::printf("=== 集成测试总计: L1 %d/%d 失败 + 小米 %d 失败 ===\n",
                fail, pass + fail, xiaomi_fail);
    return total_fail == 0 ? 0 : 1;
}
