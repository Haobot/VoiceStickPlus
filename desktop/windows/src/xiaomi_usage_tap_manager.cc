#include "xiaomi_usage_tap_manager.h"

#include "log.h"
#include "xiaomi_buttons.h"
#include "xiaomi_usage_tap_decoder.h"
#include "xiaomi_usage_tap_host.h"

#include <objbase.h>
#include <shellapi.h>

#include <vector>

namespace voicestick {

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\VoiceStickHidTap";
constexpr DWORD kMonitorPollMs = 2000;
constexpr DWORD kRecreatePipeDelayMs = 1000;

std::int64_t NowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 按钮名 → kXiaomiMappableButtons 下标（tap 识别的 volume_mute 等未开放
// 映射的键返回 -1，沿丢弃）。
int ButtonIndexOf(std::string_view button) {
    for (size_t i = 0; i < kXiaomiMappableButtons.size(); ++i) {
        if (kXiaomiMappableButtons[i] == button) return static_cast<int>(i);
    }
    return -1;
}

// 主程序同目录下的注入器（MSI/便携布局一致）。
std::wstring DefaultInjectorPath() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, path.data(),
                                         static_cast<DWORD>(path.size()));
    if (len == 0 || len >= path.size()) return L"VoiceStickTapInject.exe";
    path.resize(len);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"VoiceStickTapInject.exe";
    return path.substr(0, slash + 1) + L"VoiceStickTapInject.exe";
}

} // namespace

XiaomiUsageTapManager::~XiaomiUsageTapManager() { Stop(); }

bool XiaomiUsageTapManager::Start(EdgeCallback on_edge, StateCallback on_state,
                                  void* ctx) {
    on_edge_ = on_edge;
    on_state_ = on_state;
    callback_ctx_ = ctx;
    if (stop_event_) return true;  // 幂等：已运行仅刷新回调
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
    if (!stop_event_) {
        LogApp("XiaomiUsageTapManager: create stop event failed err=" +
               std::to_string(GetLastError()));
        return false;
    }
    pipe_connected_.store(false);
    last_frame_ms_.store(0);
    state_.store(LinkState::kNoHost);
    monitor_thread_ = std::thread([this] { MonitorThreadMain(); });
    pipe_thread_ = std::thread([this] { PipeThreadMain(); });
    LogApp("XiaomiUsageTapManager: started (pipe server + host monitor)");
    return true;
}

void XiaomiUsageTapManager::Stop() {
    if (!stop_event_) return;
    SetEvent(stop_event_);
    if (monitor_thread_.joinable()) monitor_thread_.join();
    if (pipe_thread_.joinable()) pipe_thread_.join();
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
    pipe_connected_.store(false);
    state_.store(LinkState::kNoHost);
    LogApp("XiaomiUsageTapManager: stopped");
}

void XiaomiUsageTapManager::SetState(LinkState state) {
    if (state_.exchange(state, std::memory_order_relaxed) == state) return;
    if (on_state_) on_state_(callback_ctx_, state);
}

void XiaomiUsageTapManager::RequestInject(DWORD pid) {
    const std::wstring path =
        injector_path_.empty() ? DefaultInjectorPath() : injector_path_;
    const std::wstring params = L"--pid " + std::to_wstring(pid);
    // ShellExecuteW 在未初始化 COM 的线程不保证可用：本线程按需初始化，
    // 调用后即释放（fire-and-forget，不等待注入器退出，结果由心跳收敛）。
    const HRESULT coinit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                    COINIT_DISABLE_OLE1DDE);
    HINSTANCE instance = ShellExecuteW(nullptr, L"runas", path.c_str(),
                                       params.c_str(), nullptr, SW_SHOWNORMAL);
    if (SUCCEEDED(coinit)) CoUninitialize();
    if (reinterpret_cast<INT_PTR>(instance) <= 32) {
        LogApp("XiaomiUsageTapManager: launch injector failed code=" +
               std::to_string(reinterpret_cast<INT_PTR>(instance)) +
               " path=" + std::string(path.begin(), path.end()));
    } else {
        LogApp("XiaomiUsageTapManager: injector launched pid=" +
               std::to_string(pid));
    }
}

void XiaomiUsageTapManager::MonitorThreadMain() {
    DWORD last_seen_pid = 0;
    std::int64_t last_inject_ms = 0;
    while (WaitForSingleObject(stop_event_, kMonitorPollMs) ==
           WAIT_TIMEOUT) {
        const auto pid = FindXiaomiHidHostPid();
        const std::int64_t now = NowSteadyMs();
        if (!pid.has_value()) {
            // 宿主消失（遥控器断连/OS 卸载 WUDFHost）：重置注入记忆，管道由
            // EOF 自然断开；下次出现按新宿主重新注入。
            if (last_seen_pid != 0) {
                last_seen_pid = 0;
                SetState(LinkState::kNoHost);
            }
            continue;
        }
        if (*pid != last_seen_pid) {
            last_seen_pid = *pid;
            last_inject_ms = now;
            RequestInject(*pid);
            SetState(LinkState::kInjectPending);
            continue;
        }
        if (pipe_connected_.load(std::memory_order_relaxed)) {
            // 同宿主且管道在：按心跳新鲜度收敛（stale 不重注入——DLL 已在
            // 宿主进程内，重注入无法修复心跳线程死亡/管道半开，等宿主重启）。
            const std::int64_t last = last_frame_ms_.load(
                std::memory_order_relaxed);
            SetState(now - last <= kHeartbeatStaleMs ? LinkState::kConnected
                                                     : LinkState::kStale);
        } else if (now - last_inject_ms >= kInjectRetryMs) {
            // 未连接且注入节流已过（UAC 被拒/注入器失败）：重试。
            last_inject_ms = now;
            RequestInject(last_seen_pid);
            SetState(LinkState::kInjectPending);
        }
    }
}

void XiaomiUsageTapManager::PipeThreadMain() {
    XiaomiTapFrameDecoder decoder;
    std::vector<uint8_t> chunk(512);
    while (WaitForSingleObject(stop_event_, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateNamedPipeW(
            kPipeName, PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 512, 512, 0,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            // 端口占用/资源不足：退避后重试（非 stop）。
            if (WaitForSingleObject(stop_event_, kRecreatePipeDelayMs) !=
                WAIT_TIMEOUT) {
                break;
            }
            continue;
        }
        // overlapped connect：等待探针连接或 stop（事件 auto-reset，重叠 IO
        // 完成即消费，无跨轮假唤醒）。
        OVERLAPPED cov{};
        HANDLE connect_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        cov.hEvent = connect_done;
        BOOL connected = ConnectNamedPipe(pipe, &cov);
        bool stop_requested = false;
        if (!connected) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {connect_done, stop_event_};
                const DWORD wait_result =
                    WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait_result != WAIT_OBJECT_0) {
                    stop_requested = true;
                } else {
                    DWORD dummy = 0;
                    connected = GetOverlappedResult(pipe, &cov, &dummy, FALSE);
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                connected = FALSE;
            }
        }
        CloseHandle(connect_done);
        if (stop_requested) {
            CancelIoEx(pipe, nullptr);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }
        if (!connected) {
            CloseHandle(pipe);
            if (WaitForSingleObject(stop_event_, kRecreatePipeDelayMs) !=
                WAIT_TIMEOUT) {
                break;
            }
            continue;
        }
        // 探针已连接：会话从空活跃集起步（EOF 已产释放沿清过状态，这里
        // 防御性再清一次，输出沿丢弃——旧连接的残留不往新连接泄漏）。
        decoder.Reset();
        (void)session_.OnDisconnect();
        pipe_connected_.store(true, std::memory_order_relaxed);
        LogApp("XiaomiUsageTapManager: probe connected");
        HANDLE read_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool peer_gone = false;
        while (!peer_gone) {
            OVERLAPPED rov{};
            rov.hEvent = read_done;
            DWORD read_bytes = 0;
            BOOL ok = ReadFile(pipe, chunk.data(),
                               static_cast<DWORD>(chunk.size()), &read_bytes,
                               &rov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                HANDLE waits[2] = {read_done, stop_event_};
                const DWORD wait_result =
                    WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait_result != WAIT_OBJECT_0) {
                    stop_requested = true;
                    break;
                }
                ok = GetOverlappedResult(pipe, &rov, &read_bytes, FALSE);
            }
            if (!ok || read_bytes == 0) {  // 错误或 EOF：对端断开
                peer_gone = true;
                break;
            }
            last_frame_ms_.store(NowSteadyMs(),
                                 std::memory_order_relaxed);
            for (const auto& frame : decoder.OnBytes(chunk.data(),
                                                     read_bytes)) {
                if (!frame.is_data) continue;  // 心跳：last_frame_ms_ 已刷
                auto edges = session_.OnReport(frame.report, 9);
                if (edges.has_value()) DispatchSessionEdges(*edges);
            }
        }
        CloseHandle(read_done);
        pipe_connected_.store(false, std::memory_order_relaxed);
        CancelIoEx(pipe, nullptr);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (stop_requested) break;
        // 对端断开：活跃集合全释放（防按键卡死），随后回建管道实例等重连。
        DispatchSessionEdges(session_.OnDisconnect());
        LogApp("XiaomiUsageTapManager: probe disconnected");
    }
    LogApp("XiaomiUsageTapManager: pipe server exited");
}

void XiaomiUsageTapManager::DispatchSessionEdges(
    const XiaomiUsageTapSession::Edges& edges) {
    for (const auto button : edges.pressed) {
        const int index = ButtonIndexOf(button);
        if (index < 0) {
            LogApp("XiaomiUsageTapManager: edge for unmapped button dropped");
            continue;
        }
        if (on_edge_) on_edge_(callback_ctx_, index, true);
    }
    for (const auto button : edges.released) {
        const int index = ButtonIndexOf(button);
        if (index < 0) continue;
        if (on_edge_) on_edge_(callback_ctx_, index, false);
    }
}

} // namespace voicestick
