#include "win32_app.h"

#include <Windows.h>
#include <winrt/base.h>

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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    ScopedHandle single_instance_mutex(CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName));
    if (!single_instance_mutex.get()) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    voicestick::Win32App app(instance);
    return app.Run();
}
