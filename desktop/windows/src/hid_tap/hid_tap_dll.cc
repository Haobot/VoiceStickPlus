// VoiceStickHidTap.dll —— 注入 WUDFHost 的只读 usage 探针
//（Doc/Plan/xiaomi-remote-usage-tap.md §3.2.2，方案 A Task 2）。
//
// 职责：Detours hook ntdll!NtDeviceIoControlFile，截获 IOCTL
// 0x80018483（BTHLE 读 HID 特征）成功返回的 9 字节输出（`01 00 00` 前缀 +
// 3×LE16 usage），经命名管道回传主程序。IOCTL 号与输出格式为 MiVibe-Remote
// 真机验证事实，本项目按事实重新实现（GPL 隔离，hook 引擎用 Detours/MIT）。
//
// 铁律（WUDFHost 是系统进程，崩溃连带蓝牙外设掉线）：
//   - 只读旁路：不修改 IOCTL 参数/返回值/输出缓冲，原样放行；
//   - 零侵入：所有探针侧代码异常一律吞掉（SEH/静默返回），不影响宿主；
//   - 管道写失败静默断开重连，绝不阻塞 IOCTL 路径（锁内仅 WriteFile 快路径，
//     不做连接等待）；
//   - /MT 静态 CRT（CMake 全局已钉），不向系统进程引入运行时 DLL 依赖。
//
// 管道协议（定长 10 字节帧，byte-stream）：
//   0x01 ……………………………… 心跳（5s 周期，主程序判探针存活）
//   0x02 + 9 字节原始报文 …… 按键数据

#include <windows.h>
#include <winternl.h>

#include <detours.h>

namespace {

constexpr ULONG kReadCharacteristicIoctl = 0x80018483;
constexpr ULONG kExpectedOutputLength = 9;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\VoiceStickHidTap";
constexpr DWORD kHeartbeatPeriodMs = 5000;
constexpr DWORD kDataFrameSize = 1 + kExpectedOutputLength;  // type + payload

using NtDeviceIoControlFileFn = NTSTATUS(NTAPI*)(
    HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID,
    ULONG, PVOID, ULONG);
NtDeviceIoControlFileFn g_real_nt_device_io_control_file = nullptr;

// 管道句柄与连接：SRWLOCK 保护（IOCTL 回调多线程并发；连接是 lazy + 心跳
// 线程驱动，写失败即断开，下次写/心跳时重连）。
SRWLOCK g_pipe_guard = SRWLOCK_INIT;
HANDLE g_pipe = INVALID_HANDLE_VALUE;

// 尝试连接（调用方持独占锁）。失败静默（主程序未启动/管道未创建）。
void TryConnectLocked() {
    if (g_pipe != INVALID_HANDLE_VALUE) return;
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) g_pipe = pipe;
}

// 写一帧；失败断开句柄（下次重连）。调用方持独占锁。
void WriteFrameLocked(const void* frame, DWORD size) {
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (!WriteFile(g_pipe, frame, size, &written, nullptr) ||
        written != size) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

void WriteHeartbeat() {
    AcquireSRWLockExclusive(&g_pipe_guard);
    TryConnectLocked();
    const BYTE frame[1] = {0x01};
    WriteFrameLocked(frame, sizeof(frame));
    ReleaseSRWLockExclusive(&g_pipe_guard);
}

// 数据帧（IOCTL 回调路径调用）：SEH 全包裹，读输出缓冲失败/管道断开均静默。
void WriteDataFrame(const void* output_buffer) {
    __try {
        BYTE frame[kDataFrameSize];
        frame[0] = 0x02;
        memcpy(frame + 1, output_buffer, kExpectedOutputLength);
        AcquireSRWLockExclusive(&g_pipe_guard);
        TryConnectLocked();
        WriteFrameLocked(frame, sizeof(frame));
        ReleaseSRWLockExclusive(&g_pipe_guard);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 输出缓冲换页等异常：吞掉，不影响宿主。
    }
}

NTSTATUS NTAPI HookedNtDeviceIoControlFile(
    HANDLE file_handle, HANDLE event, PIO_APC_ROUTINE apc_routine,
    PVOID apc_context, PIO_STATUS_BLOCK io_status_block, ULONG ioctl_code,
    PVOID input_buffer, ULONG input_buffer_length, PVOID output_buffer,
    ULONG output_buffer_length) {
    const NTSTATUS status = g_real_nt_device_io_control_file(
        file_handle, event, apc_routine, apc_context, io_status_block,
        ioctl_code, input_buffer, input_buffer_length, output_buffer,
        output_buffer_length);
    // 三重过滤（方案 §6.2.5）：IOCTL 号 + 成功 + 输出长 9，最小化开销与误报。
    // 只读：不触碰任何参数与返回值。
    if (ioctl_code == kReadCharacteristicIoctl && status == 0 &&
        output_buffer != nullptr &&
        output_buffer_length == kExpectedOutputLength) {
        WriteDataFrame(output_buffer);
    }
    return status;
}

// 心跳线程：周期心跳兼驱动重连。与宿主同生命周期，不退出。
DWORD WINAPI HeartbeatThreadMain(LPVOID) {
    for (;;) {
        Sleep(kHeartbeatPeriodMs);
        __try {
            WriteHeartbeat();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 0;  // 不可达：线程与宿主同生命周期
}

void InstallHook() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return;
    // Detours 官方定位导出的推荐路径：GetProcAddress + DetourAttach。
    g_real_nt_device_io_control_file =
        reinterpret_cast<NtDeviceIoControlFileFn>(GetProcAddress(
            ntdll, "NtDeviceIoControlFile"));
    if (g_real_nt_device_io_control_file == nullptr) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(
        reinterpret_cast<PVOID*>(&g_real_nt_device_io_control_file),
        HookedNtDeviceIoControlFile);
    // 提交失败时 g_real 指针保持原值（未挂钩）：探针静默无功能，宿主不受
    // 影响，主程序侧以无心跳判不可用并回落（§6.2 兼容性门控）。
    DetourTransactionCommit();
}

void RemoveHook() {
    if (g_real_nt_device_io_control_file == nullptr) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(
        reinterpret_cast<PVOID*>(&g_real_nt_device_io_control_file),
        HookedNtDeviceIoControlFile);
    DetourTransactionCommit();
    AcquireSRWLockExclusive(&g_pipe_guard);
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_pipe_guard);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        InstallHook();
        // loader lock 下 CreateThread 本身安全（线程体等锁释放后才开始跑），
        // 连接/IO 全部在该线程内做。
        if (CreateThread(nullptr, 0, HeartbeatThreadMain, nullptr, 0,
                         nullptr) == nullptr) {
            // 无心跳线程则探针只在有数据时尝试连接，仍可用；不视为失败。
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        RemoveHook();
    }
    return TRUE;
}
