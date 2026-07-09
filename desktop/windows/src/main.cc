#include "win32_app.h"
#include "cmd_line.h"
#include "log.h"

#include <Windows.h>
#include <shellapi.h>
#include <winrt/base.h>

#include <cstdint>
#include <cwchar>
#include <exception>
#include <optional>
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
    // 命令行 --ota <path> [--device <id>]：解析请求，优先转发给已运行实例。
    std::optional<voicestick::OtaCliRequest> ota_request;
    if (command_line) {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(command_line, &argc);
        if (argv) {
            ota_request = voicestick::ParseOtaCliArgs(argc, argv);
            LocalFree(argv);
        }
    }
    if (ota_request) {
        // 已有运行实例：WM_COPYDATA 转发后退出，不碰单例 Mutex。
        HWND existing = FindWindowW(L"VoiceStickWindow", nullptr);
        if (existing) {
            std::string payload = ota_request->file_path;
            if (ota_request->device_id.has_value()) {
                payload += "\n" + *ota_request->device_id;
            }
            COPYDATASTRUCT cds{};
            cds.dwData = voicestick::kOtaCopyDataId;
            cds.cbData = payload.size() + 1;  // 含终止符，跨进程安全。
            cds.lpData = payload.data();
            SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
            return 0;
        }
        // 无运行实例：继续正常启动，稍后注入 pending 请求。
    }

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
        // 无运行实例时 --ota 自启动：连上设备后由 SetConnectedDevices 触发。
        if (ota_request) {
            app.SetPendingOtaRequest(std::move(ota_request->file_path),
                                      std::move(ota_request->device_id));
        }
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
