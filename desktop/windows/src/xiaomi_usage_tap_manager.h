#pragma once

#include "xiaomi_usage_tap.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include <windows.h>

namespace voicestick {

// usage tap 管理器（Doc/Plan/xiaomi-remote-usage-tap.md §3.2.1 宿主侧）：
// 命名管道服务端 + WUDFHost HostPid 监视 + 提权注入触发，把探针 DLL 回传的
// HID 报文流还原成按键沿。
//
// 线程模型：两条工作线程。
//   - 管道线程：单实例阻塞命名管道 `\\.\pipe\VoiceStickHidTap`，接受探针
//     （重）连接；帧解码（XiaomiTapFrameDecoder）+ 会话 diff
//     （XiaomiUsageTapSession，本线程独占）→ 沿回调（本线程调用，实现方
//     应仅 PostMessage 转主线程）；EOF/错误 → OnDisconnect 全释放沿。
//   - 监视线程：2s 轮询 FindXiaomiHidHostPid——发现新宿主 PID 触发一次
//     runas 注入（同 PID 重试 30s 节流，注入器自身幂等）；管道连接 + 心跳
//     新鲜（15s 阈值）判 kConnected，否则按情况上报状态。
//
// 回调（函数指针 + ctx，调用线程分别为管道/监视线程，实现须线程安全）：
//   OnEdge(ctx, button_index, pressed)  按键沿（index 同 kXiaomiMappableButtons）
//   OnState(ctx, state)                 链路状态变化
class XiaomiUsageTapManager {
public:
    // 链路状态（设置页状态行语义）。
    enum class LinkState {
        kNoHost,         // 遥控器未连接（无 WUDFHost 宿主）
        kInjectPending,  // 已发现宿主，注入器已触发（UAC 待确认/注入中）
        kConnected,      // 管道已连接且心跳新鲜（探针存活）
        kStale,          // 管道连接但心跳超时（宿主假死/DLL 卸载）
    };

    using EdgeCallback = void (*)(void* ctx, int button_index, bool pressed);
    using StateCallback = void (*)(void* ctx, LinkState state);

    XiaomiUsageTapManager() = default;
    ~XiaomiUsageTapManager();
    XiaomiUsageTapManager(const XiaomiUsageTapManager&) = delete;
    XiaomiUsageTapManager& operator=(const XiaomiUsageTapManager&) = delete;

    // 幂等：已运行时仅刷新回调。Start 后回调即刻生效，沿/状态可能从工作
    // 线程并发到达（实现方负责转主线程）。
    bool Start(EdgeCallback on_edge, StateCallback on_state, void* ctx);
    void Stop();
    bool running() const { return stop_event_ != nullptr; }

    // 注入器 exe 路径覆盖（默认主程序同目录 VoiceStickTapInject.exe）。
    // 须在 Start 前调用。
    void SetInjectorPath(std::wstring path) { injector_path_ = std::move(path); }

    LinkState state() const { return state_.load(std::memory_order_relaxed); }

    // 心跳新鲜度阈值：DLL 心跳 5s 周期，3 个周期无任何帧判 stale。
    static constexpr std::int64_t kHeartbeatStaleMs = 15000;
    // 同一宿主 PID 的注入重试间隔（UAC 被拒/注入失败的节流）。
    static constexpr std::int64_t kInjectRetryMs = 30000;

private:
    void MonitorThreadMain();
    void PipeThreadMain();
    void SetState(LinkState state);
    void RequestInject(DWORD pid);
    // 管道线程：数据帧 → 会话 diff → 沿回调；断连 → 全释放沿。
    void DispatchSessionEdges(const XiaomiUsageTapSession::Edges& edges);

    std::wstring injector_path_;
    EdgeCallback on_edge_ = nullptr;
    StateCallback on_state_ = nullptr;
    void* callback_ctx_ = nullptr;

    HANDLE stop_event_ = nullptr;
    std::thread monitor_thread_;
    std::thread pipe_thread_;
    // 管道线程独占（含会话状态；断连语义 = 进程内全释放）。
    XiaomiUsageTapSession session_;
    // 跨线程观测：管道连通性与最近帧时刻（steady_clock 毫秒）。
    std::atomic<bool> pipe_connected_{false};
    std::atomic<std::int64_t> last_frame_ms_{0};
    std::atomic<LinkState> state_{LinkState::kNoHost};
};

} // namespace voicestick
