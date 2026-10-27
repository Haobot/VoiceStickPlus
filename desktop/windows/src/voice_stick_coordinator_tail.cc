#include "voice_stick_coordinator.h"

#include "cJSON.h"
#include "hotword_selector.h"  // B14b 提炼候选过滤
#include "usage_events.h"
#include "encoder_speed.h"
#include "localization.h"
#include "log.h"
#include "voice_edit_command.h"
#include "xiaomi_buttons.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <tuple>
#include <utility>

#include <tlhelp32.h>

#include "voice_stick_coordinator_util.h"
namespace voicestick {

namespace {

// 语音编辑（Doc/Plan/voice-edit-commands.md §7.1/§8.2）：退格节拍与提示停留时长。
constexpr int kVoiceEditBackspaceIntervalMs = 12;
constexpr int kVoiceEditToastMs = 3000;

std::string ReplaceFirstDecimal(std::string text, int value) {
    const auto marker = text.find("%d");
    if (marker != std::string::npos) {
        text.replace(marker, 2, std::to_string(value));
    }
    return text;
}

StringId VoiceEditRejectMessageId(VoiceEditRejectReason reason) {
    switch (reason) {
        case VoiceEditRejectReason::kFieldUnreadable:
            return StringId::kVoiceEditUnreadable;
        case VoiceEditRejectReason::kUnverifiedNotAllowed:
            return StringId::kVoiceEditUnverifiedExpired;
        case VoiceEditRejectReason::kSelectionActive:
            return StringId::kVoiceEditSelectionActive;
        case VoiceEditRejectReason::kCaretNotAtEnd:
            return StringId::kVoiceEditCursorMoved;
        case VoiceEditRejectReason::kPasswordField:
            return StringId::kVoiceEditPassword;
        case VoiceEditRejectReason::kTextMismatch:
            return StringId::kVoiceEditTextChanged;
        case VoiceEditRejectReason::kEmptyTarget:
        case VoiceEditRejectReason::kNothingToDelete:
        case VoiceEditRejectReason::kNone:
            break;
    }
    return StringId::kVoiceEditNoTarget;
}

std::string VoiceEditActionName(VoiceEditAction action) {
    switch (action) {
        case VoiceEditAction::kDeleteLastChars: return "chars";
        case VoiceEditAction::kDeleteLastSentences: return "sentences";
        case VoiceEditAction::kDeleteAll: return "all";
    }
    return "unknown";
}

std::string VoiceEditResultMessage(const VoiceEditCommand& command,
                                   const VoiceEditPlan& plan, UiLanguage language) {
    switch (command.action) {
        case VoiceEditAction::kDeleteLastChars:
            return ReplaceFirstDecimal(Tr(StringId::kVoiceEditDeletedChars, language),
                                       plan.delete_graphemes);
        case VoiceEditAction::kDeleteLastSentences:
            if (plan.deleted_sentences <= 1) {
                return Tr(StringId::kVoiceEditDeletedSentence, language);
            }
            return ReplaceFirstDecimal(Tr(StringId::kVoiceEditDeletedSentences, language),
                                       plan.deleted_sentences);
        case VoiceEditAction::kDeleteAll:
            return Tr(StringId::kVoiceEditDeletedAll, language);
    }
    return {};
}

}  // namespace

void VoiceStickCoordinator::MineHotwordCandidatesFromRefinement(const std::string& original,
                                                                const std::string& refined) {
    const auto mined = MineRefinementCandidates(original, refined, ConfigSnapshot()->asr_hotwords);
    if (!mined.empty()) RecordAndNotifyHotwordCandidates(mined);
}

std::string VoiceStickCoordinator::SessionHistoryProviderName() const {
    switch (ConfigSnapshot()->asr_provider) {
    case AsrProvider::kVoiceStickCloud: return "voicestick_cloud";
    case AsrProvider::kVolcengine: return "volcengine";
    case AsrProvider::kTencent: return "tencent";
    }
    return {};
}

void VoiceStickCoordinator::BeginSessionHistory(const std::string& raw_text) {
    std::lock_guard lock(session_history_mutex_);
    pending_session_history_.reset();
    if (raw_text.empty()) return;
    // 懒打开：首次落库时初始化；打开/迁移失败降级停用（Open 返回 nullptr），
    // 绝不阻塞识别主链路（对齐 macOS makeSessionHistoryStore 语义）。
    if (session_history_ == nullptr) {
        session_history_ =
            SessionHistoryStore::Open(ConfigSnapshot()->ConfigPath().parent_path() /
                                      "session_history.db");
    }
    if (session_history_ == nullptr) return;
    auto now = std::chrono::system_clock::now();
    PendingSessionHistory pending;
    pending.id = [] {
        // 会话 UUID（无 boost/uuid：GetTickCount64 + 计数足以保证本进程唯一，
        // 落库主键只要求进程内不重复）。
        static std::atomic<std::uint64_t> seq{0};
        return "s" + std::to_string(GetTickCount64()) + "-" + std::to_string(seq.fetch_add(1));
    }();
    pending.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                             now.time_since_epoch()).count();
    pending.device = active_device_id_.value_or("");
    pending.provider = SessionHistoryProviderName();
    {
        // 翻译/精修/直通统一经 TransformText 收口；mode 按输出 transform 判定。
        const auto profile = OutputProfileForDevice(active_device_id_);
        pending.mode = profile.transform == TextTransform::kTranslate ? "translate" : "original";
    }
    pending.raw = raw_text;
    if (active_session_id_.has_value()) {
        pending.duration = CurrentRecordingDurationSeconds();
    }
    // 热词 JSON 数组（cJSON 序列化，与 macOS JSONSerialization 数组形态一致）。
    {
        auto* array = cJSON_CreateArray();
        if (array != nullptr) {
            for (const auto& word : ConfigSnapshot()->asr_hotwords) {
                cJSON_AddItemToArray(array, cJSON_CreateString(word.c_str()));
            }
            char* json = cJSON_PrintUnformatted(array);
            if (json != nullptr) {
                pending.hotwords = json;
                cJSON_free(json);
            }
            cJSON_Delete(array);
        }
    }
    pending_session_history_ = std::move(pending);
}

void VoiceStickCoordinator::CompleteSessionHistory(const std::string& final_text) {
    std::lock_guard lock(session_history_mutex_);
    if (session_history_ == nullptr || !pending_session_history_.has_value()) {
        pending_session_history_.reset();
        return;
    }
    const PendingSessionHistory pending = *pending_session_history_;
    pending_session_history_.reset();
    SessionHistoryRecord record;
    record.id = pending.id;
    record.created_at = pending.created_at;
    record.device_id = pending.device;
    record.asr_provider = pending.provider;
    record.mode = pending.mode;
    record.raw_asr = pending.raw;
    record.refined_text = final_text;
    record.injected_text = final_text;
    record.duration = pending.duration;
    record.hotwords_sent = pending.hotwords;
    session_history_->Insert(record);
}

std::filesystem::path VoiceStickCoordinator::UsageEventsPath() {
    return ConfigSnapshot()->ConfigPath().parent_path() / "usage_events.jsonl";
}

void VoiceStickCoordinator::RecordUsageEvent(const std::string& kind,
                                             const std::string& text,
                                             std::optional<double> dwell_seconds) {
    // 负信号采集（用户数据飞轮 P1-1，对齐 macOS RecordUsageEvent）：只记文本长度
    // 与热词命中名单（子串匹配的成员关系），绝不记文本本身。
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    std::vector<std::string> matched;
    for (const auto& word : ConfigSnapshot()->asr_hotwords) {
        if (word.empty() || lowered.size() < word.size()) continue;
        std::string word_lowered;
        word_lowered.reserve(word.size());
        for (const char c : word) {
            word_lowered += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        if (lowered.find(word_lowered) != std::string::npos) {
            matched.push_back(word);
        }
    }
    UsageEvent event;
    event.kind = kind;
    event.ts = std::time(nullptr);
    event.device = active_device_id_.value_or("");
    event.text_length = static_cast<int>(text.size());
    event.hotwords = std::move(matched);
    event.dwell_seconds = dwell_seconds;
    AppendUsageEvent(UsageEventsPath(), event);
}

std::string VoiceStickCoordinator::ForegroundAppBucket() {
    // 前台应用分桶（doubao §3.4）：取前台窗口进程 exe 名（UTF-8 窄名；macOS 侧为
    // bundle id——分桶是各机本地数据，两端标识格式不同无碍）。取不到返回空=全局桶。
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (pid == 0) return {};
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    std::wstring exe;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                exe = entry.szExeFile;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    if (exe.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, exe.c_str(),
                                         static_cast<int>(exe.size()), nullptr, 0, nullptr, nullptr);
    std::string narrowed(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, exe.c_str(), static_cast<int>(exe.size()),
                            narrowed.data(), size, nullptr, nullptr);
    }
    return narrowed;
}

void VoiceStickCoordinator::MineCorrectionPairsFromRefinement(const std::string& original,
                                                              const std::string& refined) {
    // 精修没改动（原文回退路径）无对可提取，跳过磁盘事务。
    if (original == refined) return;
    RecordCorrectionPairsToDisk(CorrectionPairsPath(ConfigSnapshot()->ConfigPath()),
                                ExtractReplacementPairs(original, refined),
                                std::time(nullptr), ForegroundAppBucket());
}

std::optional<std::string> VoiceStickCoordinator::ExactMatchCorrectionPairFromDisk(
    const std::string& text) {
    const auto path = CorrectionPairsPath(ConfigSnapshot()->ConfigPath());
    const auto pair = ExactMatchCorrectionPair(text, LoadCorrectionPairs(path),
                                               ForegroundAppBucket());
    if (!pair) return std::nullopt;
    MarkCorrectionPairUsedOnDisk(path, *pair, std::time(nullptr));
    LogCoordinatorLine("correction pair applied exact len=" + std::to_string(pair->after.size()));
    applied_correction_pair_ = *pair;  // P3 反馈环锚点：确认保留 → recall+1
    return pair->after;
}

std::vector<CorrectionPair> VoiceStickCoordinator::CorrectionPairsForLlmPrompts() {
    return TrimCorrectionPairsForPrompt(
        LoadCorrectionPairs(CorrectionPairsPath(ConfigSnapshot()->ConfigPath())),
        kCorrectionPromptMaxPairs, ForegroundAppBucket());
}

void VoiceStickCoordinator::MaybeExtractHotwordCandidates(const std::string& final_text) {
    if (!ConfigSnapshot()->hotword_mining_enabled || ConfigSnapshot()->llm_api_key.empty() || final_text.empty()) {
        LogCoordinatorLine(std::string("hotword extraction skipped: ") +
                           (!ConfigSnapshot()->hotword_mining_enabled
                                ? "mining_disabled"
                                : (ConfigSnapshot()->llm_api_key.empty() ? "no_llm_key" : "empty_text")));
        return;
    }
    LogCoordinatorLine("hotword extraction started: text_len=" +
                       std::to_string(final_text.size()) +
                       " model=" + ConfigSnapshot()->llm_model +
                       " hotwords=" + std::to_string(ConfigSnapshot()->asr_hotwords.size()));
    auto alive = alive_;
    refiner_.ExtractHotwordCandidates(
        final_text, ConfigSnapshot()->asr_hotwords,
        [this, alive](bool ok, std::vector<std::string> words) {
            if (!alive->load()) return;
            LogCoordinatorLine("hotword extraction finished ok=" + std::string(ok ? "1" : "0") +
                               " candidates=" + std::to_string(words.size()));
            if (!ok || words.empty()) return;
            RecordAndNotifyHotwordCandidates(words);
        },
        [](const std::string& line) { LogCoordinatorLine(line); });
}

void VoiceStickCoordinator::RecordAndNotifyHotwordCandidates(const std::vector<std::string>& words) {
    // B14b（r141 拍板并入③）：LLM 提炼候选进池前过统一权威口径——非法词不入池、不通知。
    const std::vector<std::string> filtered = FilterValidHotwordCandidates(words);
    // B17：本函数由 MineHotwordCandidatesFromRefinement 链触发（P0-3 注释明示
    // on_complete 在精修 worker 线程）——整函数体收口回 UI 线程。
    RunOnUiThread([this, filtered] {
            const auto path = HotwordCandidatesPath(ConfigSnapshot()->ConfigPath());
            // B13：走 miner 的唯一写入口（进程级互斥 + reload-merge-save）——原 load-once
            // 缓存整存会用陈旧快照覆盖设置页刚写入的 dismissed/加入，用户「忽略」的词反复弹回。
            const std::vector<std::string> suggestions = RecordHotwordCandidatesToDisk(path, filtered);

            if (!suggestions.empty()) {
                const auto language = EffectiveUiLanguage(ConfigSnapshot()->ui_language);
                std::string joined;
                for (std::size_t i = 0; i < suggestions.size(); ++i) {
                    if (i != 0) joined += ", ";
                    joined += suggestions[i];
                }
                LogCoordinatorLine("hotword candidates suggested: " + joined);
                const auto message = joined + Tr(StringId::kHotwordCandidateNotifyBodySuffix, language);
                ui_->ShowNotification(Tr(StringId::kHotwordCandidateNotifyTitle, language), message);
                // 托盘气球可能被系统勿扰/通知设置静默拦截（实测 Win+N 通知中心无记录），
                // 悬浮窗临时消息保证用户必现；会话活跃时实现侧自动回退托盘。
                ui_->ShowTimedMessage(message, 3000);
            }
    });
}

std::vector<std::string> VoiceStickCoordinator::RankedHotwordsForAsr() {
    std::vector<std::string> ranked;
    {
        std::lock_guard lock(hotword_usage_mutex_);
        if (!hotword_usage_loaded_) {
            hotword_usage_ = LoadHotwordUsage(
                ConfigSnapshot()->ConfigPath().parent_path() / "hotword_usage.json");
            hotword_usage_loaded_ = true;
        }
        ranked = RankHotwords(hotword_usage_, ConfigSnapshot()->asr_hotwords, std::time(nullptr));
    }
    const auto fitted = AsrProtocol::FitHotwordsToCorpusBudget(ranked);
    if (fitted.size() < ranked.size() && !hotword_trim_notified_.exchange(true)) {
        // 每次运行只提示一次：热词库超出直传预算，按使用频率优先保留，其余本次不参与。
        const auto language = EffectiveUiLanguage(ConfigSnapshot()->ui_language);
        const std::string body =
            Tr(StringId::kHotwordTrimBodyPrefix, language) +
            std::to_string(fitted.size()) + "/" + std::to_string(ranked.size()) +
            Tr(StringId::kHotwordTrimBodySuffix, language);
        std::multiset<std::string> kept(fitted.begin(), fitted.end());
        std::ostringstream dropped;
        for (const auto& word : ranked) {
            auto it = kept.find(word);
            if (it != kept.end()) {
                kept.erase(it);
                continue;
            }
            if (dropped.tellp() > 0) dropped << ", ";
            dropped << word;
        }
        LogCoordinatorLine("hotword trimmed to corpus budget " +
                           std::to_string(AsrProtocol::kHotwordCorpusTokenBudget) +
                           ": kept " + std::to_string(fitted.size()) + "/" +
                           std::to_string(ranked.size()) + ", dropped: " + dropped.str());
        ui_->ShowNotification(Tr(StringId::kHotwordTrimTitle, language), body);
        // 托盘气球可能被系统勿扰/通知设置静默拦截，悬浮窗临时消息保证必现；
        // 会话活跃时实现侧自动回退托盘（同 RecordAndNotifyHotwordCandidates 模式）。
        ui_->ShowTimedMessage(body, 3000);
    }
    return fitted;
}

std::vector<std::string> VoiceStickCoordinator::HotwordsForLlmPrompts() {
    std::lock_guard lock(hotword_usage_mutex_);
    if (!hotword_usage_loaded_) {
        hotword_usage_ = LoadHotwordUsage(
            ConfigSnapshot()->ConfigPath().parent_path() / "hotword_usage.json");
        hotword_usage_loaded_ = true;
    }
    return TrimHotwordsForPrompt(hotword_usage_, ConfigSnapshot()->asr_hotwords,
                                 kHotwordPromptMaxWords, std::time(nullptr));
}

void VoiceStickCoordinator::RecordHotwordNegativeFromText(const std::string& text) {
    if (text.empty() || ConfigSnapshot()->asr_hotwords.empty()) return;
    std::lock_guard lock(hotword_usage_mutex_);
    if (!hotword_usage_loaded_) {
        hotword_usage_ = LoadHotwordUsage(
            ConfigSnapshot()->ConfigPath().parent_path() / "hotword_usage.json");
        hotword_usage_loaded_ = true;
    }
    RecordHotwordNegativeInText(hotword_usage_, text, ConfigSnapshot()->asr_hotwords);
    SaveHotwordUsage(ConfigSnapshot()->ConfigPath().parent_path() / "hotword_usage.json",
                     hotword_usage_);
}

void VoiceStickCoordinator::RecordHotwordUsageFromText(const std::string& text) {
    if (text.empty() || ConfigSnapshot()->asr_hotwords.empty()) return;
    std::lock_guard lock(hotword_usage_mutex_);
    if (!hotword_usage_loaded_) {
        hotword_usage_ = LoadHotwordUsage(
            ConfigSnapshot()->ConfigPath().parent_path() / "hotword_usage.json");
        hotword_usage_loaded_ = true;
    }
    RecordHotwordUsageInText(hotword_usage_, text, ConfigSnapshot()->asr_hotwords, std::time(nullptr));
    SaveHotwordUsage(ConfigSnapshot()->ConfigPath().parent_path() / "hotword_usage.json", hotword_usage_);
}

void VoiceStickCoordinator::BeginWaitingForAudioEnd(std::string_view reason) {
    if (waiting_for_audio_end_.load()) return;
    waiting_for_audio_end_.store(true);
    LogCoordinatorLine(std::string("waiting for audio END") +
                       (reason.empty() ? std::string() : " reason=" + std::string(reason)));
    EnterFinalizing("waiting_audio_end");
    ScheduleAudioEndTimeout(active_session_id_, active_device_id_);
}

void VoiceStickCoordinator::ScheduleAudioEndTimeout(std::optional<std::uint32_t> session_id,
                                                    std::optional<std::string> device_id) {
    const auto generation = audio_end_wait_generation_.fetch_add(1) + 1;
    std::thread([this, alive = alive_, generation, session_id, device_id = std::move(device_id)] {
        std::this_thread::sleep_for(kAudioEndTimeout);
        if (!alive->load()) return;
        std::lock_guard lock(audio_mutex_);
        if (audio_end_wait_generation_.load() != generation ||
            !waiting_for_audio_end_.load() ||
            active_session_id_ != session_id ||
            active_device_id_ != device_id) {
            return;
        }
        LogCoordinatorLine("audio END timeout; finalizing buffered audio");
        // 时序探针：audio_end 超时（固件 drain 尾帧丢失/迟到），用 ==0 守卫避免覆盖 recv 路径。
        if (probe_audio_end_done_ms_.load() == 0) {
            probe_audio_end_done_ms_.store(SteadyNowMs());
            LogCoordinatorLine("tseq audio_end_timeout ts=" + std::to_string(probe_audio_end_done_ms_.load()));
        }
        SendFinalOggChunkIfNeeded(CurrentRecordingDurationSeconds());
    }).detach();
}

void VoiceStickCoordinator::CancelAudioEndTimeout() {
    waiting_for_audio_end_.store(false);
    audio_end_wait_generation_.fetch_add(1);
}

// recording 硬超时兜底：button_down 进 recording 时调度，button_up/audio_end/取消/断连
// 经 EnterReady/EnterFinalizing 取消。超时未收到结束信号则按当前输出模式走停止路径，
// 覆盖 button_up 与 audio_end 同时丢失致永久卡 listening。锁内仅校验 generation 并读取
// 停止路径所需状态，释放锁后调 Stop（StopWechatInputMethodSession 内部获取 audio_mutex_，
// 持锁调用会死锁）。
void VoiceStickCoordinator::ScheduleRecordingHardTimeout() {
    const auto generation = recording_hard_timeout_generation_.fetch_add(1) + 1;
    std::thread([this, alive = alive_, generation]() {
        std::this_thread::sleep_for(recording_hard_timeout_);
        if (!alive->load()) return;
        bool stale = false;
        bool wechat_active = false;
        {
            std::lock_guard lock(audio_mutex_);
            if (recording_hard_timeout_generation_.load() != generation) {
                return;
            }
            // 仍在录音且 generation 未变 = button_up/audio_end 都没到 = 卡死。
            stale = active_session_id_.has_value();
            if (stale) {
                wechat_active = wechat_input_method_active_;
            }
        }
        if (!stale) return;
        LogCoordinatorLine("recording hard timeout; canceling stuck session");
        if (wechat_active) {
            // wechat 模式：走专用停止路径（停 renderer/松热键/切回设备/清 wechat_active）+ EnterReady。
            StopWechatInputMethodSession();
            EnterReady("wechat_hard_timeout");
        } else {
            CancelShortRecording();
        }
    }).detach();
}

void VoiceStickCoordinator::CancelRecordingHardTimeout() {
    recording_hard_timeout_generation_.fetch_add(1);
}

// 点按折叠（click/hold）按住流自动松开：click 启动的 SendDown+repeat 只为满足
// WeType 长按检测弹框，面板弹出后松开按键——会话靠 WeType 自身存活（keyup 不
// 终止），文字提交改由停止击的「新按住提交」保证（不再依赖松开时机）。若按住流
// 持续，用户任何关闭面板的尝试（鼠标 detach/VAD 收尾/超时）都会被下一个 repeat
// keydown 立即重新弹开面板（「浮窗点关又弹出」真机复验）。停止路径与新会话启动
// bump generation 取消未触发的释放。
void VoiceStickCoordinator::ScheduleWechatClickHoldRelease() {
    const auto generation = wechat_click_hold_generation_.fetch_add(1) + 1;
    std::thread([this, alive = alive_, generation]() {
        std::this_thread::sleep_for(wechat_click_hold_release_);
        if (!alive->load()) return;
        {
            std::lock_guard lock(audio_mutex_);
            if (wechat_click_hold_generation_.load() != generation ||
                !wechat_input_method_active_) {
                return;
            }
        }
        LogCoordinatorLine("wechat click hold: auto release");
        if (wechat_hotkey_) {
            wechat_hotkey_->SendUp();  // StopRepeat 加锁，与停止路径并发安全
        }
    }).detach();
}

// 音频流停滞兜底：focused_app 录音中固件 25fps 持续发帧，超过 audio_stall_timeout_ 一帧未收
// 说明 button_up 与 audio_end 双丢或链路卡死。此时走 audio_end 等待路径收尾（给迟到的 END 帧
// kAudioEndTimeout 机会后按已有缓冲 finalize），不再干等 120s 硬超时。每 500ms 检查一次；
// 状态离开 kRecording 或 generation 变化即退出。
void VoiceStickCoordinator::ScheduleRecordingStallWatchdog() {
    last_audio_frame_ms_.store(SteadyNowMs());
    const auto generation = recording_stall_generation_.fetch_add(1) + 1;
    // 捕获调度时的会话 id：wechat 会话同样置 kRecording（"wechat_primary_down"）但不走
    // 该 watchdog，若上一个 focused 会话的残留线程在 wechat 录音期间醒来，凭会话 id
    // 不匹配 + wechat 激活态双重校验退出，避免误触发 BeginWaitingForAudioEnd 污染 wechat 会话。
    const auto session_id = active_session_id_;
    std::thread([this, alive = alive_, generation, session_id]() {
        while (alive->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!alive->load()) return;
            {
                std::lock_guard lock(audio_mutex_);
                if (recording_stall_generation_.load() != generation ||
                    session_state_ != SessionState::kRecording ||
                    active_session_id_ != session_id ||
                    wechat_input_method_active_) {
                    return;
                }
                if (SteadyNowMs() - last_audio_frame_ms_.load() < audio_stall_timeout_.count()) {
                    continue;
                }
            }
            LogCoordinatorLine("audio stall timeout; finalizing buffered audio");
            BeginWaitingForAudioEnd("audio_stall");
            return;
        }
    }).detach();
}

// finalizing 闲置兜底：等 ASR final / LLM 翻译或精修期间，链路层对 receive 超时静默重等
// （asr_client_win ReceiveOneReusable），服务端不回 SessionFinished 时会永久卡 Processing。
// 这里按「无进展时长」判活：ASR partial/segment 与精修 token 都会刷新活动时间，连续
// finalizing_timeout_ 无任何进展才兜底——有 ASR 原文回退粘贴原文，否则报错进 error 态。
// 每 200ms 检查一次；离开 kFinalizing 或 generation 变化即退出，无需显式 cancel。
void VoiceStickCoordinator::ScheduleFinalizingWatchdog() {
    finalizing_last_activity_ms_.store(SteadyNowMs());
    const auto generation = finalizing_watchdog_generation_.fetch_add(1) + 1;
    std::thread([this, alive = alive_, generation]() {
        while (alive->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!alive->load()) return;
            std::string fallback_text;
            {
                std::lock_guard lock(audio_mutex_);
                if (finalizing_watchdog_generation_.load() != generation ||
                    session_state_ != SessionState::kFinalizing) {
                    return;
                }
                if (SteadyNowMs() - finalizing_last_activity_ms_.load() <
                    finalizing_timeout_.count()) {
                    continue;
                }
                fallback_text = finalizing_fallback_text_;
            }
            if (!fallback_text.empty()) {
                // LLM 翻译/精修无响应：回退粘贴 ASR 原文，不丢本次输入。
                // 语音编辑兜底（Doc §5.6/§8.1）：命令 final 不得因精修挂起被当正文粘贴。
                // watchdog 在 detached 线程，先 RunOnUiThread 再进入状态机（避免并发写）。
                LogCoordinatorLine("finalizing watchdog timeout; pasting unrefined final text");
                RunOnUiThread([this, fallback_text = std::move(fallback_text)]() mutable {
                    if (is_shutdown_) return;
                    if (HandleVoiceEditIfMatched(fallback_text)) return;
                    EnterPendingConfirmation(fallback_text, "finalizing_watchdog");
                });
            } else {
                // ASR 服务端始终未回 final：报错退出（用户确认后回 ready），不永久卡住。
                LogCoordinatorLine("finalizing watchdog timeout; no final text, aborting");
                FinishWithAsrError("ASR response timeout");
            }
            return;
        }
    }).detach();
}

void VoiceStickCoordinator::TouchFinalizingWatchdog() {
    finalizing_last_activity_ms_.store(SteadyNowMs());
}

void VoiceStickCoordinator::LogLatencyProbeBreakdown(std::string_view tag) {
    const auto bu = probe_button_up_ms_.load();
    // 非 button_up 起始的会话（如全局热键触发）不汇总，避免噪声。
    if (bu == 0) return;
    const auto ae = probe_audio_end_done_ms_.load();
    const auto fc = probe_final_chunk_ms_.load();
    const auto fp = probe_first_partial_ms_.load();
    const auto af = probe_asr_final_ms_.load();
    const auto now = SteadyNowMs();
    auto delta = [](std::int64_t a, std::int64_t b) -> std::int64_t {
        return (a != 0 && b >= a) ? (b - a) : 0;
    };
    // wait_ae: button_up -> audio_end 到达/超时（等固件 drain 尾帧，超时即 2000ms）；
    // chunk_to_first_partial: final chunk -> 首个 partial（ASR 建连 + 首字）；
    // asr_proc: final chunk -> final（ASR 总处理）；total: button_up -> ready（Thinking 总延迟）。
    LogCoordinatorLine("tseq ready tag=" + std::string(tag) +
                       " ts=" + std::to_string(now) +
                       " button_up=" + std::to_string(bu) +
                       " audio_end=" + std::to_string(ae) +
                       " final_chunk=" + std::to_string(fc) +
                       " first_partial=" + std::to_string(fp) +
                       " asr_final=" + std::to_string(af) +
                       " wait_ae_ms=" + std::to_string(delta(bu, ae)) +
                       " chunk_to_first_partial_ms=" + std::to_string(delta(fc, fp)) +
                       " asr_proc_ms=" + std::to_string(delta(fc, af)) +
                       " total_ms=" + std::to_string(now - bu));
}

void VoiceStickCoordinator::LogWechatLatency(std::string_view stage) {
    if (!wechat_latency_anchor_.has_value()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - *wechat_latency_anchor_).count();
    LogCoordinatorLine("wechat latency: " + std::string(stage) + " +" +
                       std::to_string(elapsed) + "ms");
}

void VoiceStickCoordinator::FinishWithAsrError(const std::string& message) {
    CancelAudioEndTimeout();
    CancelAsrClients();
    pending_paste_state_ = {};
    active_session_id_.reset();
    debug_audio_recorder_.Discard();
    FinishRecognitionCycle();
    EnterError(message, "asr_error");
}

void VoiceStickCoordinator::RecoverFromAsrError(bool hide_overlay) {
    if (!is_showing_asr_error_) return;
    is_showing_asr_error_ = false;
    EnterReady("error_recovered", hide_overlay);
}

void VoiceStickCoordinator::CommitPendingPaste(const std::string& text, bool manual) {
    if (pending_paste_state_.text == text) CompletePendingPaste(text, manual);
}

void VoiceStickCoordinator::CompletePendingPaste(const std::string& text, bool manual) {
    const bool should_press_enter = ConfigSnapshot()->auto_enter;
    // 负信号采集（P1-1）：确认动作 + 暂停态停留时长（自动路径无停留 = nullopt）。
    std::optional<double> dwell;
    if (pending_paste_entered_at_ms_ != 0) {
        dwell = static_cast<double>(GetTickCount64() - pending_paste_entered_at_ms_) / 1000.0;
    }
    RecordUsageEvent(manual ? usage_event_kind::kPasteManuallyConfirmed
                            : usage_event_kind::kPasteAutoConfirmed,
                     text, dwell);
    // P3 反馈环保留信号（用户数据飞轮）：精确应用的对被用户确认保留 → recall+1。
    if (applied_correction_pair_.has_value()) {
        MarkCorrectionPairRecalledOnDisk(
            CorrectionPairsPath(ConfigSnapshot()->ConfigPath()), *applied_correction_pair_);
        applied_correction_pair_.reset();
    }
    pending_paste_entered_at_ms_ = 0;
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    // 时序探针汇总：button_up -> ready 各阶段耗时，定位 Thinking 延迟根因。
    LogLatencyProbeBreakdown("paste_complete");
    EnterReady("paste_complete");
    input_injector_->Paste(text, should_press_enter);
    // 语音编辑（Doc §4 原则 2）：唯一粘贴 chokepoint 记录本次注入；Paste 无回执仍记录
    //（UIA 不可读时靠 L1/短时间窗兜底，可读时 L1 校验兜底）。
    RecordLastInjection(text, should_press_enter);
}

void VoiceStickCoordinator::RecordLastInjection(const std::string& text, bool pressed_enter) {
    if (text.empty()) return;  // Paste 对空文本是 no-op，不记录（否则 L0 得到空目标）
    LastInjection record;
    record.text = text;
    record.remaining_text = text;
    record.injected_at = std::chrono::steady_clock::now();
    record.pressed_enter = pressed_enter;
    if (foreground_probe_) {
        record.foreground_window = foreground_probe_->ForegroundWindow();
        if (record.foreground_window != 0) {
            record.foreground_process = foreground_probe_->ForegroundProcessName();
        }
    }
    last_injection_ = std::move(record);
    // 新注入覆盖旧记录（方案 §6.1：始终只保留最近一次）；代际 +1 使在途异步删除回调
    // 只更新同一代记录。
    ++voice_edit_generation_;
    LogCoordinatorLine(
        "voice edit record: len=" + std::to_string(last_injection_->remaining_text.size()) +
        " enter=" + (pressed_enter ? "1" : "0") +
        " hwnd=" + std::to_string(static_cast<unsigned long long>(
                       last_injection_->foreground_window)) +
        " proc=" + (last_injection_->foreground_process.empty()
                        ? std::string("?")
                        : last_injection_->foreground_process));
}

void VoiceStickCoordinator::StartVoiceEditBackspaces(int count, std::uint64_t generation,
                                                     std::string message,
                                                     std::string new_remaining_text,
                                                     bool new_pressed_enter) {
    if (count <= 0) return;
    const auto parsed = ParseKeySpec("backspace");
    if (!parsed.has_value()) {
        // key_spec 白名单恒支持 backspace；防御性记录后放弃（不注入任何东西）。
        LogCoordinatorLine("voice edit: ParseKeySpec(backspace) failed");
        return;
    }
    const KeySpec backspace = *parsed;
    const auto alive = alive_;
    std::thread([this, alive, count, generation, backspace, message = std::move(message),
                 new_remaining_text = std::move(new_remaining_text), new_pressed_enter]() {
        for (int i = 0; i < count; ++i) {
            if (!alive->load()) return;
            input_injector_->SendKeyCombo(backspace);
            if (i + 1 < count) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kVoiceEditBackspaceIntervalMs));
            }
        }
        // 执行完成：回 UI 线程更新记录（仅同一代）+ 提示结果。SendInput 无回执，
        // toast 只表示"已发出"，不是保证（Doc §7.4）。
        RunOnUiThread([this, alive, generation, message = std::move(message),
                       new_remaining_text = std::move(new_remaining_text),
                       new_pressed_enter]() {
            if (!alive->load()) return;
            if (generation == voice_edit_generation_ && last_injection_.has_value()) {
                last_injection_->remaining_text = new_remaining_text;
                last_injection_->pressed_enter = new_pressed_enter;
            }
            ui_->ShowTimedMessage(message, kVoiceEditToastMs);
        });
    }).detach();
}

bool VoiceStickCoordinator::HandleVoiceEditIfMatched(const std::string& text) {
    if (is_shutdown_) return false;
    // 只在 finalizing 内接受（迟到 final/取消后不误执行；watchdog 回 UI 线程后同样校验）。
    if (session_state_ != SessionState::kFinalizing) return false;
    const auto config = ConfigSnapshot();
    const auto& voice_edit = config->voice_edit;
    if (!voice_edit.enabled) return false;
    // 仅 focused_app（Doc §6.2 #2）：字幕/微信输入法模式按原链路处理，不拦截。
    if (OutputProfileForDevice(active_device_id_).target != OutputTarget::kFocusedApp) {
        return false;
    }
    const auto command =
        ParseVoiceEditCommand(text, voice_edit.default_chars, voice_edit.max_chars);
    if (!command.has_value()) return false;  // 非指令：走原链路（普通文本上屏）

    // === 命中指令：本轮由语音编辑接管，绝不进入精修/翻译/粘贴（Doc §8.1） ===
    pasted_final_text_ = true;
    const UiLanguage language = EffectiveUiLanguage(config->ui_language);
    const auto reject = [this, language](StringId message_id, std::string_view reason) {
        LogCoordinatorLine("voice edit rejected reason=" + std::string(reason));
        FinishRecognitionCycle();
        EnterReady("voice_edit_rejected");
        ui_->ShowTimedMessage(Tr(message_id, language), kVoiceEditToastMs);
    };

    // ---- L0 作用域闸（Doc §6.2）----
    const auto now = std::chrono::steady_clock::now();
    if (!last_injection_.has_value() || last_injection_->remaining_text.empty()) {
        reject(StringId::kVoiceEditNoTarget, "no_injection");
        return true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_injection_->injected_at);
    if (elapsed.count() > static_cast<long long>(voice_edit.scope_seconds)) {
        reject(StringId::kVoiceEditNoTarget, "scope_expired");
        return true;
    }
    // 前台窗口 + 进程一致（切应用/文本已提交到别处 → 拒绝；探针缺失时 fail-closed）。
    const std::uintptr_t foreground_window =
        foreground_probe_ ? foreground_probe_->ForegroundWindow() : 0;
    const std::string foreground_process =
        (foreground_probe_ && foreground_window != 0)
            ? foreground_probe_->ForegroundProcessName()
            : std::string();
    if (!foreground_probe_ || foreground_window == 0 ||
        last_injection_->foreground_window == 0 ||
        foreground_window != last_injection_->foreground_window ||
        foreground_process != last_injection_->foreground_process) {
        reject(StringId::kVoiceEditWrongWindow, "app_changed");
        return true;
    }
    // 前台进程完整性不得高于本进程（UIPI 会让 SendInput 静默失效，Doc §6.2 #6）。
    {
        std::wstring elevated_process;
        if (foreground_probe_->IsForegroundHigherIntegrity(elevated_process)) {
            reject(StringId::kVoiceEditElevated, "foreground_elevated");
            return true;
        }
    }

    // ---- L1 目标校验（Doc §6.3）：UIA 读取在 UI 线程（COM STA）----
    VoiceEditFieldState field;
    if (voice_edit.verify_mode != VoiceEditVerifyMode::kOff && focus_text_reader_) {
        if (!focus_text_reader_->ReadFocusedField(&field)) {
            field = VoiceEditFieldState{};  // 不可验证（auto 短窗/严格拒绝由 planner 决定）
        }
    }
    const auto unverified_allowed =
        voice_edit.verify_mode == VoiceEditVerifyMode::kAuto &&
        elapsed.count() <= static_cast<long long>(voice_edit.unverified_seconds);
    const auto plan = PlanVoiceEdit(last_injection_->remaining_text,
                                    last_injection_->pressed_enter, *command, field,
                                    voice_edit.verify_mode, unverified_allowed);
    if (!plan.ok) {
        reject(VoiceEditRejectMessageId(plan.reject_reason),
               VoiceEditRejectReasonName(plan.reject_reason));
        return true;
    }

    const auto generation = voice_edit_generation_;
    const std::string message = VoiceEditResultMessage(*command, plan, language);
    LogCoordinatorLine(
        "voice edit action=" + VoiceEditActionName(command->action) +
        " requested=" + std::to_string(command->count) +
        " delete_chars=" + std::to_string(plan.delete_graphemes) +
        " delete_sentences=" + std::to_string(plan.deleted_sentences) +
        " newline=" + (plan.delete_newline ? "1" : "0") +
        " backspace=" + std::to_string(plan.backspace_count) +
        " verified=" + (plan.verified ? "1" : "0") +
        " age_ms=" +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                            last_injection_->injected_at)
                           .count()));
    // 先收尾回 ready，再异步注入退格（不阻塞 UI；count 上限=剩余注入文本 grapheme 数）。
    FinishRecognitionCycle();
    EnterReady("voice_edit");
    StartVoiceEditBackspaces(plan.backspace_count, generation, message,
                             plan.new_remaining_text, plan.new_pressed_enter);
    return true;
}

bool VoiceStickCoordinator::RestoreLastInputConfirmation(std::optional<std::string> device_id) {
    if (!pending_paste_state_.IsIdle() || session_state_ != SessionState::kReady ||
        active_session_id_.has_value() || !last_recoverable_text_.has_value()) {
        return false;
    }
    active_device_id_ = std::move(device_id);
    EnterPausedConfirmation(*last_recoverable_text_, "restore_last_input");
    return true;
}

bool VoiceStickCoordinator::HandleFrontButtonDuringPendingPaste(const std::string& device_id) {
    if (pending_paste_state_.IsIdle()) return false;
    if (active_device_id_ != device_id) return true;
    if (pending_paste_state_.kind == PendingPasteKind::kWaitingToPaste) {
        EnterPausedConfirmation(pending_paste_state_.text, "pause_pending_paste");
        return true;
    }
    ui_->HideOverlay([this, text = pending_paste_state_.text] { CommitPendingPaste(text, true); });
    return true;
}

void VoiceStickCoordinator::CancelPendingPaste(const std::string& device_id) {
    if (active_session_id_.has_value()) {
        if (active_device_id_ == device_id) {
            CancelRecognitionInProgress();
        }
        return;
    }
    if (IsWaitingForFinalText()) {
        if (active_device_id_ == device_id) CancelRecognitionInProgress();
        else RefreshDeviceUiState(device_id);
        return;
    }
    if (pending_paste_state_.IsIdle()) {
        RestoreLastInputConfirmation(device_id);
        return;
    }
    if (active_device_id_ != device_id) return;
    // 负信号采集（P1-1）：待确认文本被取消 + 暂停态停留时长。
    std::optional<double> cancel_dwell;
    if (pending_paste_entered_at_ms_ != 0) {
        cancel_dwell = static_cast<double>(GetTickCount64() - pending_paste_entered_at_ms_) / 1000.0;
    }
    RecordUsageEvent(usage_event_kind::kPasteCanceled, pending_paste_state_.text, cancel_dwell);
    // 热词负样本记分（评分负项）。
    RecordHotwordNegativeFromText(pending_paste_state_.text);
    // P3 反馈环：应用后被取消 = 未保留，清锚不增 recall。
    applied_correction_pair_.reset();
    pending_paste_entered_at_ms_ = 0;
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("cancel_pending_paste");
}

void VoiceStickCoordinator::CancelRecognitionInProgress() {
    // 负信号采集（P1-1）：识别中途取消/重录（无文本）。
    RecordUsageEvent(usage_event_kind::kRecognitionCanceled, {});
    pending_paste_entered_at_ms_ = 0;
    if (IsWechatInputMethodActive()) {
        StopWechatInputMethodSession();
        EnterReady("cancel_recognition_wechat");
        return;
    }
    active_session_id_.reset();
    active_session_started_at_ = {};
    CancelAudioEndTimeout();
    CancelStreamingRefinement();
    CancelAsrClients();
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("cancel_recognition");
}

void VoiceStickCoordinator::CancelActiveCycleIfDeviceDisconnected() {
    if (is_shutdown_) return;
    // 断连设备的体感态必须清理，否则残留激活会拦截重连后的主键录音。
    for (auto it = air_mouse_active_devices_.begin(); it != air_mouse_active_devices_.end();) {
        if (!ble_->IsConnected(*it)) {
            const std::string disconnected_id = *it;
            it = air_mouse_active_devices_.erase(it);
            air_mouse_states_.erase(disconnected_id);
            ble_->SendAirMouseEnabled(false, disconnected_id);
            LogCoordinatorLine("air mouse disabled on VS-" + disconnected_id + " (disconnected)");
            if (on_air_mouse_active_changed) on_air_mouse_active_changed(!air_mouse_states_.empty());
        } else {
            ++it;
        }
    }
    for (auto it = subtitle_cycles_.begin(); it != subtitle_cycles_.end();) {
        const auto& device_id = it->first.first;
        if (!ble_->IsConnected(device_id)) {
            if (it->second->asr) it->second->asr->Cancel();
            it->second->debug_audio_recorder.Discard();
            ui_->HideOverlay();
            active_subtitle_sessions_.erase(device_id);
            it = subtitle_cycles_.erase(it);
        } else {
            ++it;
        }
    }
    if (active_device_id_.has_value() && !ble_->IsConnected(*active_device_id_)) {
        if (IsWechatInputMethodActive()) {
            // wechat 模式断连必须走专用停止路径，否则 renderer/热键/wechat_active 残留，
            // 重连后 button_up 条件不匹配、button_down 被残留 active 忽略，卡在 Recording。
            StopWechatInputMethodSession();
            EnterReady("wechat_device_disconnected");
            return;
        }
        if (waiting_for_audio_end_.load()) {
            SendFinalOggChunkIfNeeded(CurrentRecordingDurationSeconds());
            return;
        }
        // final 音频块已发出后，剩余链路（ASR nostream final、翻译/精修）全在网络侧，
        // 与 BLE 链路无关：断连不取消 ASR，让 final 到达后正常粘贴；若 final 始终不到，
        // finalizing watchdog 按既有逻辑回退粘贴原文或报错。修复「流式已上屏文字，
        // 但 BLE 僵尸链路断连把在途 ASR final 取消掉导致不粘贴」的数据丢失。
        if (sent_final_audio_chunk_ && session_state_ == SessionState::kFinalizing) {
            LogCoordinatorLine("device disconnected while awaiting ASR final; "
                               "keeping network-side finalization alive");
            return;
        }
        CancelAsrClients();
        pending_paste_state_ = {};
        active_session_id_.reset();
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
        EnterReady("device_disconnected");
    }
}

void VoiceStickCoordinator::CancelStreamingRefinement() {
    if (refinement_cancel_token_) {
        refinement_cancel_token_->store(true);
        refinement_cancel_token_.reset();
    }
}

void VoiceStickCoordinator::FinishRecognitionCycle() {
    CancelAudioEndTimeout();
    CancelStreamingRefinement();
    asr_started_ = false;
    sent_final_audio_chunk_ = false;
    pasted_final_text_ = false;
    finalizing_fallback_text_.clear();
    buffered_ogg_chunks_.clear();
}

bool VoiceStickCoordinator::IsXiaomiRemoteDevice(const std::string& device_id) {
    {
        std::lock_guard lock(firmware_mutex_);
        const auto it = firmware_info_by_device_id_.find(device_id);
        if (it != firmware_info_by_device_id_.end() &&
            it->second.hardware == kHardwareXiaomiRemote2Pro) {
            return true;
        }
    }
    // 配对配置种子：HandlePairingCompleted 先存 config 再调 CheckFirmwareAfterPairing，
    // 早于 UpdateDeviceFirmwareInfo 时这里兜底。
    for (const auto& entry : ConfigSnapshot()->paired_devices) {
        if (entry.device_id == device_id && entry.hardware == kHardwareXiaomiRemote2Pro) {
            return true;
        }
    }
    return false;
}

void VoiceStickCoordinator::UpdateDeviceFirmwareInfo(const StateEvent& event, const std::string& device_id) {
    std::string hardware_to_save;
    std::string version_to_save;
    {
        std::lock_guard lock(firmware_mutex_);
        auto& info = firmware_info_by_device_id_[device_id];
        if (!event.hardware.empty()) {
            info.hardware = event.hardware;
            hardware_to_save = event.hardware;
        }
        if (!event.firmware_version.empty()) {
            info.current_version = event.firmware_version;
            version_to_save = event.firmware_version;
        }
        info.error_message.clear();
    }
    if (!hardware_to_save.empty() || !version_to_save.empty()) {
        std::lock_guard<std::mutex> write_lock(config_write_mutex_);
        AppConfig next = *ConfigSnapshot();
        next.SavePairedDeviceInfo(device_id, hardware_to_save, version_to_save);
        config_.store(std::make_shared<const AppConfig>(std::move(next)));
    }
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::CheckFirmwareUpdatesIfNeeded(bool force, bool show_errors) {
    if (paired_device_ids_.empty() && !force) return;
    {
        std::lock_guard lock(firmware_mutex_);
        if (firmware_manifest_check_in_flight_) return;
        if (!force && has_last_firmware_manifest_check_at_ &&
            std::chrono::steady_clock::now() - last_firmware_manifest_check_at_ < kFirmwareManifestCacheDuration) {
            // Use the cached manifest to refresh any newly connected device info.
        } else {
            firmware_manifest_check_in_flight_ = true;
            SetFirmwareChecking(true);
            if (firmware_manifest_thread_.joinable()) {
                firmware_manifest_thread_.join();
            }
            auto alive = alive_;
            firmware_manifest_thread_ = std::thread([this, alive, show_errors] {
                std::string error;
                auto manifest = firmware_manifest_client_.FetchManifestSync(error);
                if (!alive->load()) return;
                {
                    std::lock_guard callback_lock(firmware_mutex_);
                    firmware_manifest_check_in_flight_ = false;
                    for (auto& [_, info] : firmware_info_by_device_id_) {
                        info.is_checking = false;
                    }
                    if (manifest.has_value()) {
                        LogCoordinatorLine("firmware manifest version=" + manifest->version +
                                           " hardware=" + manifest->hardware);
                        latest_firmware_manifest_ = std::move(manifest);
                        last_firmware_manifest_check_at_ = std::chrono::steady_clock::now();
                        has_last_firmware_manifest_check_at_ = true;
                        for (auto& [_, info] : firmware_info_by_device_id_) {
                            info.error_message.clear();
                        }
                    } else {
                        LogCoordinatorLine("firmware manifest check failed: " + error);
                        for (const auto& device_id : paired_device_ids_) {
                            if (show_errors || firmware_info_by_device_id_.contains(device_id)) {
                                firmware_info_by_device_id_[device_id].error_message = error;
                            }
                        }
                    }
                }
                RefreshFirmwareAvailability();
            });
            return;
        }
    }
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::RefreshFirmwareAvailability() {
    std::map<std::string, DeviceFirmwareInfo> snapshot;
    std::vector<std::tuple<std::string, std::string, std::string, bool>> update_prompts;
    // 气泡候选（锁内收集全部可升级设备，锁外再按连接态过滤+去重）：connected_device_ids_
    // 由 BLE 回调线程写、无锁，参照 ResolveActiveDevice 等现有读点在锁外弱一致读。
    std::vector<std::tuple<std::string, std::string, std::string, bool>> balloon_candidates;
    {
        std::lock_guard lock(firmware_mutex_);
        for (auto& [device_id, info] : firmware_info_by_device_id_) {
            info.latest_version.clear();
            info.update_available = false;
            // 小米遥控器无 OTA 渠道：不参与固件更新检查与提示。
            if (info.hardware == kHardwareXiaomiRemote2Pro) {
                snapshot[device_id] = info;
                continue;
            }
            if (!latest_firmware_manifest_.has_value() ||
                info.hardware.empty() ||
                info.current_version.empty()) {
                snapshot[device_id] = info;
                continue;
            }
            if (!IsFirmwareManifestCompatible(info, *latest_firmware_manifest_)) {
                LogCoordinatorLine("firmware availability VS-" + device_id +
                                   " hardware=" + info.hardware +
                                   " current=" + info.current_version +
                                   " latest=" + latest_firmware_manifest_->version +
                                   " update=false reason=hardware_mismatch manifest_hardware=" +
                                   latest_firmware_manifest_->hardware);
            } else {
                info.latest_version = latest_firmware_manifest_->version;
                const auto urgency = ClassifyFirmwareUpdateUrgency(
                    info.current_version, *latest_firmware_manifest_,
                    AppConfig::minimum_compatible_firmware_version);
                info.update_available = urgency != FirmwareUpdateUrgency::kUpToDate;
                const bool is_below_minimum = urgency == FirmwareUpdateUrgency::kRequired;
                if (ShouldShowFirmwareUpdatePromptAfterPairing(device_id, info)) {
                    update_prompts.emplace_back(
                        device_id,
                        info.current_version,
                        info.latest_version,
                        is_below_minimum);
                }
                if (info.update_available) {
                    balloon_candidates.emplace_back(
                        device_id, info.current_version, info.latest_version, is_below_minimum);
                }
                LogCoordinatorLine("firmware availability VS-" + device_id +
                                   " hardware=" + info.hardware +
                                   " current=" + info.current_version +
                                   " latest=" + info.latest_version +
                                   " update=" + (info.update_available ? "true" : "false"));
            }
            snapshot[device_id] = info;
        }
    }
    ui_->SetFirmwareInfo(snapshot);
    for (const auto& [device_id, current_version, latest_version, is_below_minimum] : update_prompts) {
        ui_->ShowFirmwareUpdatePrompt(device_id, current_version, latest_version, is_below_minimum);
    }
    // 主动提醒：已连接且版本落后时发一次托盘气泡（同设备同目标版本会话内不重复）；
    // below_minimum 的模态 prompt 语义保持不变。未连接不消耗去重名额。
    for (const auto& [device_id, current_version, latest_version, is_below_minimum] : balloon_candidates) {
        const bool is_connected =
            std::find(connected_device_ids_.begin(), connected_device_ids_.end(),
                      device_id) != connected_device_ids_.end();
        if (!is_connected) continue;
        const std::string balloon_key = device_id + "@" + latest_version;
        {
            std::lock_guard lock(firmware_mutex_);
            if (firmware_update_balloon_sent_keys_.contains(balloon_key)) continue;
            firmware_update_balloon_sent_keys_.insert(balloon_key);
        }
        ui_->ShowFirmwareUpdateBalloon(device_id, current_version, latest_version, is_below_minimum);
    }
}

bool VoiceStickCoordinator::ShouldShowFirmwareUpdatePromptAfterPairing(const std::string& device_id,
                                                                       const DeviceFirmwareInfo& info) {
    if (!pending_firmware_update_prompt_device_ids_.contains(device_id) ||
        info.current_version.empty() || info.latest_version.empty()) {
        return false;
    }
    if (!info.update_available) {
        pending_firmware_update_prompt_device_ids_.erase(device_id);
        return false;
    }
    pending_firmware_update_prompt_device_ids_.erase(device_id);
    return true;
}

void VoiceStickCoordinator::SetFirmwareChecking(bool is_checking) {
    std::map<std::string, DeviceFirmwareInfo> snapshot;
    {
        for (const auto& device_id : paired_device_ids_) {
            auto& info = firmware_info_by_device_id_[device_id];
            info.is_checking = is_checking;
            if (is_checking) info.error_message.clear();
            snapshot[device_id] = info;
        }
    }
    ui_->SetFirmwareInfo(snapshot);
}

bool VoiceStickCoordinator::IsWaitingForFinalText() const {
    return session_state_ == SessionState::kFinalizing;
}

void VoiceStickCoordinator::SetSessionState(SessionState state, std::string_view reason) {
    if (session_state_ == state) return;
    auto state_name = [](SessionState value) {
        switch (value) {
        case SessionState::kReady: return "ready";
        case SessionState::kRecording: return "recording";
        case SessionState::kFinalizing: return "finalizing";
        case SessionState::kPendingConfirmation: return "pending_confirmation";
        case SessionState::kPausedConfirmation: return "paused_confirmation";
        case SessionState::kError: return "error";
        }
        return "unknown";
    };
    LogCoordinatorLine("state " + std::string(state_name(session_state_)) +
                       " -> " + state_name(state) +
                       (reason.empty() ? std::string() : " reason=" + std::string(reason)));
    session_state_ = state;
}

void VoiceStickCoordinator::EnterReady(std::string_view reason, bool hide_overlay) {
    CancelRecordingHardTimeout();
    SetSessionState(SessionState::kReady, reason);
    ui_->SetStatus("Ready");
    SendUiStateForActiveDevice("ready");
    if (hide_overlay) ui_->HideOverlay();
    active_device_id_.reset();
    // local-mic 会话任意收尾（含 watchdog/取消异常路径）：停喂采集线程。
    // 采集器本身不在此停（EnterReady 调用方多持有 audio_mutex_，join 采集线程
    // 会与喂帧路径的死锁）；采集由热键释放/Shutdown 停止，期间帧被会话校验丢弃。
    local_mic_active_session_id_.store(0);
    // 会话级 ASR 路由随会话结束解除（后续无音频可发，防御性复位）。
    session_asr_ = nullptr;
    // 本地精修钉住同源解除：final 文本路径可能在收尾后仍有迟到回调，
    // 复位后走原文直通，不再依赖可能已被外壳替换的 refiner。
    session_uses_local_refine_ = false;
}

void VoiceStickCoordinator::EnterFinalizing(std::string_view reason) {
    CancelRecordingHardTimeout();
    SetSessionState(SessionState::kFinalizing, reason);
    ScheduleFinalizingWatchdog();
    ui_->SetStatus("Processing");
    SendUiStateForActiveDevice("thinking");
}

void VoiceStickCoordinator::EnterPendingConfirmation(const std::string& text, std::string_view reason) {
    // 只在 kFinalizing 下接受粘贴完成：watchdog 兜底或用户取消已离开 finalizing 后，
    // 迟到的 translate/refine 完成回调不得二次粘贴。
    if (session_state_ != SessionState::kFinalizing) {
        LogCoordinatorLine("ignore stale pending confirmation reason=" + std::string(reason));
        return;
    }
    CompletePendingPaste(text, /*manual=*/false);
}

void VoiceStickCoordinator::EnterPausedConfirmation(const std::string& text, std::string_view reason) {
    pending_paste_entered_at_ms_ = GetTickCount64();
    pending_paste_state_ = {PendingPasteKind::kPaused, text};
    SetSessionState(SessionState::kPausedConfirmation, reason);
    ui_->ShowPausedFinal(text, active_device_id_);
    SendUiStateForActiveDevice("pending_confirmation", text);
}

void VoiceStickCoordinator::EnterError(const std::string& message, std::string_view reason) {
    is_showing_asr_error_ = true;
    SetSessionState(SessionState::kError, reason);
    SendUiStateForActiveDevice("error", message);
    ui_->ShowError(message, active_device_id_, [this] { RecoverFromAsrError(false); });
}

void VoiceStickCoordinator::RefreshDeviceUiState(const std::string& device_id) {
    switch (session_state_) {
    case SessionState::kRecording:
        ble_->SendUiState(active_device_id_ == device_id ? "recording" : "ready", "", device_id);
        break;
    case SessionState::kFinalizing:
        ble_->SendUiState(active_device_id_ == device_id ? "thinking" : "ready", "", device_id);
        break;
    case SessionState::kPendingConfirmation:
    case SessionState::kPausedConfirmation:
        if (active_device_id_ == device_id) {
            ble_->SendUiState("pending_confirmation", pending_paste_state_.text, device_id);
        } else {
            ble_->SendUiState("ready", "", device_id);
        }
        break;
    case SessionState::kError:
        ble_->SendUiState(active_device_id_ == device_id ? "error" : "ready", "", device_id);
        break;
    case SessionState::kReady:
        ble_->SendUiState("ready", "", device_id);
        break;
    }
}

void VoiceStickCoordinator::SendUiStateForActiveDevice(const std::string& state, const std::string& text) {
    ble_->SendUiState(state, text, active_device_id_);
}

OutputProfile VoiceStickCoordinator::OutputProfileForDevice(const std::optional<std::string>& device_id) const {
    return ConfigSnapshot()->OutputProfileForDevice(device_id);
}

InteractionMode VoiceStickCoordinator::InteractionModeToSend() const {
    // wechat 模式按其专属触发模式（trigger_mode，与全局 interaction_mode 解耦）决定下发：
    //   hold -> hold_to_talk_instant（按下即录音跳过 300ms 阈值，降低弹框延迟）
    //   click -> click_to_talk
    // 非 wechat 模式（focused_app/字幕）仍下发全局 interaction_mode（托盘菜单控制），
    // 不被 wechat 的点按式选择污染。
    if (ConfigSnapshot()->default_output_profile.target == OutputTarget::kWechatInputMethod) {
        return ConfigSnapshot()->wechat_input_method.trigger_mode == InteractionMode::kHoldToTalk
                   ? InteractionMode::kHoldToTalkInstant
                   : InteractionMode::kClickToTalk;
    }
    return ConfigSnapshot()->interaction_mode;
}

OverlayThemeColor VoiceStickCoordinator::ThemeColorForDevice(const std::string& device_id) const {
    return ThemeColorForConfig(*ConfigSnapshot(), device_id);
}

OverlayThemeColor VoiceStickCoordinator::ThemeColorForConfig(const AppConfig& config,
                                                             const std::string& device_id) {
    auto it = config.device_theme_colors.find(device_id);
    return it == config.device_theme_colors.end() ? DefaultOverlayThemeColor() : it->second;
}

bool VoiceStickCoordinator::ShouldUseDefiniteSegments(const OutputProfile& profile) const {
    return profile.target == OutputTarget::kSubtitle &&
           ConfigSnapshot()->interaction_mode == InteractionMode::kClickToTalk;
}

double VoiceStickCoordinator::CurrentRecordingDurationSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - active_session_started_at_).count();
}

bool VoiceStickCoordinator::ShouldDiscardWechatRecording() const {
    // 与 focused_app/subtitle 路径对齐：零帧或短于最小录音时长的会话不落盘调试音频，
    // 避免 hold_to_talk_instant 模式下无意点按 / button_up 抢跑产生仅含 ogg 头的极小文件。
    if (received_audio_frames_ == 0) return true;
    return CurrentRecordingDurationSeconds() < kMinimumRecordingDurationSeconds;
}

std::optional<std::string> VoiceStickCoordinator::ResolveHotkeyTargetDevice() const {
    if (active_device_id_.has_value() &&
        std::find(connected_device_ids_.begin(), connected_device_ids_.end(), *active_device_id_) !=
            connected_device_ids_.end()) {
        return active_device_id_;
    }
    for (const auto& device_id : paired_device_ids_) {
        if (std::find(connected_device_ids_.begin(), connected_device_ids_.end(), device_id) !=
            connected_device_ids_.end()) {
            return device_id;
        }
    }
    if (!connected_device_ids_.empty()) {
        return connected_device_ids_.front();
    }
    return std::nullopt;
}

void VoiceStickCoordinator::HandleGlobalHotkeyPressed() {
    if (hotkey_is_down_) {
        LogApp("hotkey pressed but already down, skipping");
        return;
    }

    LogApp("hotkey pressed, resolving target device...");
    LogApp("  connected_device_ids: " + std::to_string(connected_device_ids_.size()));
    for (const auto& id : connected_device_ids_) {
        LogApp("    - VS-" + id);
    }
    LogApp("  active_device_id: " + (active_device_id_.has_value() ? "VS-" + *active_device_id_ : "none"));

    auto target_device = ResolveHotkeyTargetDevice();
    if (!target_device) {
        if (paired_device_ids_.empty()) {
            ui_->SetStatus("Hotkey: pair a VoiceStick first");
            if (ConfigSnapshot()->debug_audio_cache) {
                ui_->ShowNotification("热键触发失败", "请先配对 VoiceStick 设备");
            }
        } else {
            ui_->SetStatus("Hotkey: VoiceStick not connected; press the main button to wake it");
            if (ConfigSnapshot()->debug_audio_cache) {
                ui_->ShowNotification("热键触发失败", "设备可能已休眠，请按主键唤醒后重试。");
            }
        }
        LogApp("hotkey pressed but no connected device");
        return;
    }

    LogApp("  resolved target device: VS-" + *target_device);

    const auto request_id = next_hotkey_request_id_++;
    if (ConfigSnapshot()->interaction_mode == InteractionMode::kHoldToTalk) {
        hotkey_is_down_ = true;
        hotkey_active_device_id_ = target_device;
    }
    LogApp("  sending remote_button_down to VS-" + *target_device + ", request_id=" + std::to_string(request_id));
    ble_->SendRemoteButton(RemoteButtonAction::kDown, "primary", target_device, request_id);
    ui_->SetStatus("Recording (hotkey) on VS-" + *target_device);
    if (ConfigSnapshot()->debug_audio_cache) {
        ui_->ShowNotification("热键已触发", "正在 VS-" + *target_device + " 上启动录音，松开热键结束识别");
    }
    LogApp("hotkey pressed, starting recording on VS-" + *target_device);
}

void VoiceStickCoordinator::HandleGlobalHotkeyReleased() {
    if (ConfigSnapshot()->interaction_mode == InteractionMode::kClickToTalk) {
        return;
    }

    if (!hotkey_is_down_) return;

    auto target_device = hotkey_active_device_id_;
    hotkey_is_down_ = false;
    hotkey_active_device_id_.reset();

    if (target_device && ble_->IsConnected(*target_device)) {
        const auto request_id = next_hotkey_request_id_++;
        ble_->SendRemoteButton(RemoteButtonAction::kUp, "primary", target_device, request_id);
        LogApp("hotkey released, stopping recording on VS-" + *target_device);
    }
}


// ===== 本机麦克风模式（local-mic）=====

bool VoiceStickCoordinator::LocalMicSessionActiveLocked() const {
    return active_session_id_.has_value() && active_device_id_.has_value() &&
           *active_device_id_ == kLocalMicDeviceId;
}

AsrClient* VoiceStickCoordinator::SessionAsrClient() {
    // 会话建立时钉住（HandlePrimaryButtonDown）：final 块发送前 active_session_id_
    // 已被 SendFinalOggChunkIfNeeded 重置，不能按会话身份现算路由。
    return session_asr_ ? session_asr_ : asr_.get();
}

void VoiceStickCoordinator::CancelAsrClients() {
    if (asr_) asr_->Cancel();
    if (local_asr_) local_asr_->Cancel();
}

void VoiceStickCoordinator::HandleLocalMicHotkeyPressed() {
    // 门控：运行件齐备 + 配置开启。focused_app 之外的目标（wechat/字幕）是设备流
    // 设计，本机麦克风首期不接（迭代三后再评估）。
    if (!local_mic_capture_ || !local_asr_ || !ConfigSnapshot()->local_asr.enabled) return;
    if (ConfigSnapshot()->default_output_profile.target != OutputTarget::kFocusedApp) return;
    if (local_mic_hotkey_down_) return;  // 按住期间自动重复去抖
    // 授权闸：本地引擎被拒（试用到期/无有效授权）时提示并放弃本次会话。
    if (allow_local_asr_ && !allow_local_asr_()) {
        const auto language = EffectiveUiLanguage(ConfigSnapshot()->ui_language);
        ui_->ShowTimedMessage(Tr(StringId::kLicenseLocalBlocked, language), 3000);
        return;
    }

    local_mic_hotkey_down_ = true;
    StateEvent event;
    event.event = "button_down";
    event.button = "primary";
    event.session_id = next_local_mic_session_id_++;
    HandleStateEvent(event, std::string(kLocalMicDeviceId));

    std::lock_guard lock(audio_mutex_);
    if (!LocalMicSessionActiveLocked()) {
        // 会话被既有守卫拒绝（其他设备会话活跃/确认中等）：HandlePrimaryButtonDown
        // 已按设备语义处理，本机麦克风不启动。
        return;
    }
    local_mic_slicer_.Reset();
    local_mic_encoder_.Reset();
    local_mic_next_seq_ = 1;
    local_mic_active_session_id_.store(*active_session_id_);
    if (local_mic_capture_->Start()) {
        LogCoordinatorLine("local mic session started session=" +
                           std::to_string(*active_session_id_));
        return;
    }
    // 采集启动失败（无麦克风/被占用）：按短按取消收尾并给用户可见提示。
    const auto error = local_mic_capture_->LastStartError();
    LogCoordinatorLine("local mic capture start failed: " + error);
    local_mic_active_session_id_.store(0);
    CancelShortRecording();
    ui_->ShowTimedMessage("麦克风启动失败：" + error, 3000);
}

void VoiceStickCoordinator::HandleLocalMicHotkeyReleased() {
    if (!local_mic_hotkey_down_) return;
    local_mic_hotkey_down_ = false;
    if (!local_mic_capture_) return;
    // 先停采（join 采集线程）：此后 on_pcm 不再触发，slicer/encoder 余量可安全访问。
    // 若会话已被 watchdog/取消路径收尾，这里只负责停采即返回。
    local_mic_capture_->Stop();

    std::optional<std::uint32_t> session_id;
    {
        std::lock_guard lock(audio_mutex_);
        if (LocalMicSessionActiveLocked()) {
            session_id = active_session_id_;
        }
        local_mic_active_session_id_.store(0);
    }
    if (!session_id) return;

    // 尾帧补零凑满 40ms 帧编码（P1 同策略，避免尾字丢失），再发空 END 帧走主会话
    // audio_end 收尾路径：短按丢弃 / 最终块发送 / EnterFinalizing 全部复用既有逻辑。
    auto remainder = local_mic_slicer_.TakeRemainder();
    if (!remainder.empty()) {
        remainder.resize(AudioOpusEncoder::kFrameSamples, 0);
        std::uint8_t packet[512];
        const auto result = local_mic_encoder_.Encode(remainder.data(), remainder.size(),
                                                      packet, sizeof(packet));
        if (result.encoded_bytes > 0) {
            AudioFrame frame;
            frame.session_id = *session_id;
            frame.seq = local_mic_next_seq_++;
            frame.payload.assign(packet, packet + result.encoded_bytes);
            HandleAudioFrame(frame, std::string(kLocalMicDeviceId));
        }
    }
    AudioFrame end_frame;
    end_frame.session_id = *session_id;
    end_frame.seq = local_mic_next_seq_++;
    end_frame.flags = 0x02;  // AudioFrame::IsEnd()；空 payload 触发收尾
    HandleAudioFrame(end_frame, std::string(kLocalMicDeviceId));
    LogCoordinatorLine("local mic session stopped session=" + std::to_string(*session_id));
}

void VoiceStickCoordinator::FeedLocalMicPcm(std::span<const std::int16_t> pcm) {
    const auto session_id = local_mic_active_session_id_.load();
    if (session_id == 0) return;  // 会话未建立或已收尾：无锁早退
    for (const auto& frame : local_mic_slicer_.Append(pcm)) {
        std::uint8_t packet[512];
        const auto result = local_mic_encoder_.Encode(frame.data(), frame.size(),
                                                      packet, sizeof(packet));
        if (result.encoded_bytes <= 0) continue;
        AudioFrame audio;
        audio.session_id = session_id;
        audio.seq = local_mic_next_seq_++;
        audio.payload.assign(packet, packet + result.encoded_bytes);
        // 复用主会话帧处理（内部自锁并校验会话/seq，watchdog 刷新/ogg 组包/ASR
        // 发送全部同设备路径）；会话被取消时帧被校验丢弃。
        HandleAudioFrame(audio, std::string(kLocalMicDeviceId));
    }
}

} // namespace voicestick
