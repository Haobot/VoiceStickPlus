#include "app_config.h"
#include "ota_command.h"

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kAppWindowClass[] = L"VoiceStickWindow";
constexpr wchar_t kAppWindowTitle[] = L"VoiceStick";

std::string Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring CurrentExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    if (length == 0) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path().wstring();
}

BOOL CALLBACK FindAppWindowProc(HWND hwnd, LPARAM lparam) {
    wchar_t class_name[128]{};
    GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
    if (std::wstring_view(class_name) != kAppWindowClass) return TRUE;
    *reinterpret_cast<HWND*>(lparam) = hwnd;
    return FALSE;
}

HWND FindAppWindow() {
    if (HWND hwnd = FindWindowW(kAppWindowClass, nullptr)) return hwnd;
    HWND found = nullptr;
    EnumWindows(FindAppWindowProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

HWND WaitForAppWindow(std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (HWND hwnd = FindAppWindow()) return hwnd;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return nullptr;
}

bool StartApp() {
    const auto app_path = std::filesystem::path(CurrentExecutableDirectory()) / L"VoiceStick.exe";
    std::wstring command = L"\"" + app_path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(app_path.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, app_path.parent_path().c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

std::wstring MakePipeName(const std::string& request_id) {
    return L"\\\\.\\pipe\\VoiceStick.Ota." + std::to_wstring(GetCurrentProcessId()) +
           L"." + std::to_wstring(GetTickCount64());
}

int ExitCodeForErrorLine(const std::string& line) {
    if (line.find("code=timeout") != std::string::npos) return 124;
    if (line.find("code=wifi_") != std::string::npos ||
        line.find("code=park_required") != std::string::npos) {
        return 4;
    }
    return 5;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(Utf8FromUtf16(argv[i]));
    }

    std::string error;
    auto command = voicestick::ParseOtaCommandLine(args, &error);
    if (!command) {
        std::cerr << "参数错误: " << error << "\n";
        return 2;
    }

    voicestick::AppConfig config = voicestick::AppConfig::Load();
    if (!voicestick::ResolveOtaPullCommandFromConfig(config, &*command, &error)) {
        std::cerr << "配置错误: " << error << "\n";
        return 2;
    }

    if (command->request_id.empty()) {
        command->request_id = "ctl-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    }
    const std::wstring pipe_name_w = MakePipeName(command->request_id);
    command->reply_pipe = Utf8FromUtf16(pipe_name_w);

    HANDLE pipe = CreateNamedPipeW(pipe_name_w.c_str(), PIPE_ACCESS_INBOUND,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "IPC 错误: 创建 reply pipe 失败\n";
        return 3;
    }

    HWND hwnd = FindAppWindow();
    if (!hwnd) {
        if (!StartApp()) {
            CloseHandle(pipe);
            std::cerr << "IPC 错误: 无法启动 VoiceStick.exe\n";
            return 3;
        }
        hwnd = WaitForAppWindow(std::chrono::seconds(15));
    }
    if (!hwnd) {
        CloseHandle(pipe);
        std::cerr << "IPC 错误: 找不到 VoiceStick 主窗口\n";
        return 3;
    }

    const std::string request = voicestick::SerializeOtaIpcRequest(*command);
    COPYDATASTRUCT copy{};
    copy.dwData = 1;
    copy.cbData = static_cast<DWORD>(request.size() + 1);
    copy.lpData = const_cast<char*>(request.c_str());
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy),
                             SMTO_ABORTIFHUNG, 5000, &result)) {
        CloseHandle(pipe);
        std::cerr << "IPC 错误: 发送 WM_COPYDATA 失败\n";
        return 3;
    }

    BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(pipe);
        std::cerr << "IPC 错误: App 未连接 reply pipe\n";
        return 3;
    }

    std::string pending;
    char buffer[512];
    int exit_code = 3;
    while (true) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        pending.append(buffer, buffer + read);
        std::size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::cout << line << "\n";
            if (line.rfind("done ok=true", 0) == 0) {
                exit_code = 0;
                CloseHandle(pipe);
                return exit_code;
            }
            if (line.rfind("error ", 0) == 0) {
                exit_code = ExitCodeForErrorLine(line);
                CloseHandle(pipe);
                return exit_code;
            }
        }
    }

    CloseHandle(pipe);
    return exit_code;
}
