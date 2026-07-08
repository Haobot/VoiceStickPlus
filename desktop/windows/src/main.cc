#include "win32_app.h"

#include "log.h"

#include <Windows.h>
#include <winrt/base.h>

#include <cstdint>
#include <cwchar>
#include <exception>
#include <string>

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\TenClass.VoiceStick.SingleInstance";

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_) CloseHandle(handle_);
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    try {
        // 带 --relaunch 启动表示这是提权重启：旧实例正在 ShutdownAndQuit 退出、尚未释放
        // 单例 Mutex。直接 CreateMutexW 会拿到 ALREADY_EXISTS 被当作"已有实例"退出，导致
        // 新旧实例都消失（提权重启失败）。重试等待旧实例释放 Mutex（最多约 5 秒）后再接管。
        const bool is_relaunch = command_line && std::wcsstr(command_line, L"--relaunch") != nullptr;
        HANDLE mutex_handle = nullptr;
        if (is_relaunch) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                mutex_handle = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
                if (mutex_handle && GetLastError() != ERROR_ALREADY_EXISTS) {
                    break;
                }
                if (mutex_handle) {
                    CloseHandle(mutex_handle);
                    mutex_handle = nullptr;
                }
                Sleep(100);
            }
            if (!mutex_handle || GetLastError() == ERROR_ALREADY_EXISTS) {
                if (mutex_handle) CloseHandle(mutex_handle);
                voicestick::LogApp("Relaunch: timed out waiting for previous instance to exit");
                return 0;
            }
        } else {
            mutex_handle = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
            if (!mutex_handle) {
                voicestick::LogApp("CreateMutexW failed");
                return 1;
            }
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(mutex_handle);
                voicestick::LogApp("Another VoiceStick instance is already running");
                return 0;
            }
        }
        ScopedHandle single_instance_mutex(mutex_handle);

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        voicestick::Win32App app(instance);
        return app.Run();
    } catch (const winrt::hresult_error& error) {
        voicestick::LogApp("Fatal startup WinRT error: hr=" +
                           std::to_string(static_cast<std::int32_t>(error.code())) +
                           " message=" + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        voicestick::LogApp(std::string("Fatal startup exception: ") + error.what());
    } catch (...) {
        voicestick::LogApp("Fatal startup unknown exception");
    }
    return 1;
}
