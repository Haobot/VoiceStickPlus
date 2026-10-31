// VoiceStickTapInject.exe —— usage tap 探针的提权注入器（方案 A Task 2，
// Doc/Plan/xiaomi-remote-usage-tap.md §3.2.3 + §6.1）。
//
// 由主程序经 ShellExecuteW "runas" 隐藏启动（单次 UAC），职责链：
//   校验管理员 → 注册表定位当前 RC003 WUDFHost PID 并与 --pid 一致 →
//   目标进程名必须是 wudfhost.exe（防误注入）→ 部署 DLL 到
//   %PROGRAMDATA%\VoiceStick\hid-tap\（SHA-256 一致性 + icacls ACL 锁，防
//   普通用户可写替换造成系统进程加载劫持）→ 幂等检查（已载入则退出）→
//   SeDebugPrivilege → VirtualAllocEx/WriteProcessMemory/
//   CreateRemoteThread(LoadLibraryW) 注入。
//
// 退出码：0 成功（含已注入的幂等路径）；非 0 各阶段失败。
// 日志：追加写 %PROGRAMDATA%\VoiceStick\logs\tap_inject.log（主程序诊断）。

#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "xiaomi_usage_tap_host.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr wchar_t kDllName[] = L"VoiceStickHidTap.dll";
constexpr wchar_t kDeployDir[] = L"%PROGRAMDATA%\\VoiceStick\\hid-tap";
constexpr wchar_t kLogFile[] = L"%PROGRAMDATA%\\VoiceStick\\logs\\tap_inject.log";

void ExpandPath(const wchar_t pattern[], std::wstring* out) {
    wchar_t buffer[MAX_PATH] = {};
    ExpandEnvironmentStringsW(pattern, buffer, MAX_PATH);
    *out = buffer;
}

void AppendLog(const std::string& line) {
    std::wstring path;
    ExpandPath(kLogFile, &path);
    // 逐级创建目录（CreateDirectoryW 不递归；首次运行时 VoiceStick 与
    // VoiceStick\logs 两级都不存在。失败则丢弃日志，不影响注入主流程）。
    for (size_t pos = path.find_first_of(L'\\'); pos != std::wstring::npos;
         pos = path.find_first_of(L'\\', pos + 1)) {
        if (pos > 2) CreateDirectoryW(path.substr(0, pos).c_str(), nullptr);
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02u %02u:%02u:%02u] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DWORD written = 0;
    WriteFile(file, prefix, static_cast<DWORD>(strlen(prefix)), &written,
              nullptr);
    WriteFile(file, line.c_str(), static_cast<DWORD>(line.size()), &written,
              nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &size) &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool TargetIsWudfHost(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        // 真机实测（Win11 26200）：非提权下即 err=5（WUDFHost ACL 拒普通
        // 用户查询）；提权后仍失败须细分原因（PPL 等），日志给出错误码。
        AppendLog("target check: OpenProcess err=" +
                  std::to_string(GetLastError()));
        return false;
    }
    wchar_t image_path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(process, 0, image_path, &size);
    CloseHandle(process);
    if (!ok) {
        AppendLog("target check: QueryFullProcessImageNameW err=" +
                  std::to_string(GetLastError()));
        return false;
    }
    const wchar_t* base = wcsrchr(image_path, L'\\');
    base = base != nullptr ? base + 1 : image_path;
    if (_wcsicmp(base, L"wudfhost.exe") != 0) {
        const std::wstring wide(base);
        AppendLog("target check: image name mismatch: " +
                  std::string(wide.begin(), wide.end()));
        return false;
    }
    return true;
}

std::string Sha256File(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string digest;
    std::string buffer(1 << 16, '\0');
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) ==
            0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        DWORD read = 0;
        bool ok = true;
        while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                        &read, nullptr) &&
               read > 0) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                               read, 0) != 0) {
                ok = false;
                break;
            }
        }
        BYTE raw[32];
        if (ok && BCryptFinishHash(hash, raw, sizeof(raw), 0) == 0) {
            static const char kHex[] = "0123456789abcdef";
            for (BYTE byte : raw) {
                digest.push_back(kHex[byte >> 4]);
                digest.push_back(kHex[byte & 0xF]);
            }
        }
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return digest;
}

// 部署 DLL 到 %PROGRAMDATA%（源 = 注入器同目录副本）。返回部署后完整路径；
// 失败返回空。部署副本与源 SHA-256 必须一致（防复制损坏/半写）。
std::wstring DeployDll(const std::wstring& source_dll) {
    std::wstring dir;
    ExpandPath(kDeployDir, &dir);
    // 逐级创建目录（ProgramData\VoiceStick、…\hid-tap；已存在则忽略）。
    for (size_t pos = dir.find_first_of(L'\\'); pos != std::wstring::npos;
         pos = dir.find_first_of(L'\\', pos + 1)) {
        CreateDirectoryW(dir.substr(0, pos).c_str(), nullptr);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring deployed = dir + L"\\" + kDllName;
    const std::string source_hash = Sha256File(source_dll);
    if (source_hash.empty()) {
        AppendLog("deploy: source dll unreadable " +
                  std::string(source_dll.begin(), source_dll.end()));
        return {};
    }
    if (Sha256File(deployed) != source_hash) {
        // 临时名写半再原子替换，防部署中断留下截断 DLL。
        const std::wstring staging = deployed + L".staging";
        if (!CopyFileW(source_dll.c_str(), staging.c_str(), FALSE)) {
            AppendLog("deploy: copy failed err=" +
                      std::to_string(GetLastError()));
            return {};
        }
        if (Sha256File(staging) != source_hash) {
            AppendLog("deploy: staging hash mismatch");
            DeleteFileW(staging.c_str());
            return {};
        }
        if (!MoveFileExW(staging.c_str(), deployed.c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            const DWORD replace_err = GetLastError();
            DeleteFileW(staging.c_str());
            // 替换失败的典型场景：宿主正加载已部署 DLL（文件锁定，如应用
            // 更新后重注入）。部署副本 hash 有效（旧版）即继续注入旧版，
            // 新版等宿主重启（遥控器重连换 WUDFHost）后自然生效。
            const std::string deployed_hash = Sha256File(deployed);
            if (!deployed_hash.empty()) {
                AppendLog("deploy: replace locked err=" +
                          std::to_string(replace_err) +
                          ", keep running version sha256=" + deployed_hash);
                return deployed;
            }
            AppendLog("deploy: replace failed err=" +
                      std::to_string(replace_err));
            return {};
        }
    }
    if (Sha256File(deployed) != source_hash) {
        AppendLog("deploy: deployed hash mismatch");
        return {};
    }
    // ACL 锁：目录与 DLL 仅 SYSTEM/Administrators 可写、Users 只读（SID 形式
    // 免本地化组名）。失败不阻断注入（文件继承 ACL 仍受 ProgramData 默认
    // 保护），仅记日志。
    wchar_t dir_cmd[1024];
    swprintf(dir_cmd, 1024,
             L"/C icacls \"%s\" /inheritance:r /grant:r "
             L"*S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F "
             L"*S-1-5-32-545:(OI)(CI)RX /C /Q",
             dir.c_str());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(L"C:\\Windows\\System32\\icacls.exe", dir_cmd, nullptr,
                       nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                       &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        AppendLog("deploy: icacls launch failed err=" +
                  std::to_string(GetLastError()));
    }
    AppendLog("deploy: ok sha256=" + source_hash);
    return deployed;
}

bool DllAlreadyLoaded(DWORD pid) {
    const HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, kDllName) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr,
                                          nullptr) &&
                    GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    CloseHandle(token);
    return ok != FALSE;
}

bool InjectLibrary(DWORD pid, const std::wstring& dll_path) {
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (process == nullptr) {
        AppendLog("inject: OpenProcess failed err=" +
                  std::to_string(GetLastError()));
        return false;
    }
    bool ok = false;
    void* remote_path = nullptr;
    HANDLE thread = nullptr;
    do {
        std::wstring path_nul(dll_path + L'\0');
        const SIZE_T bytes = path_nul.size() * sizeof(wchar_t);
        remote_path = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
        if (remote_path == nullptr) break;
        if (!WriteProcessMemory(process, remote_path, path_nul.data(), bytes,
                                nullptr)) {
            break;
        }
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        const auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(kernel32, "LoadLibraryW"));
        if (load_library == nullptr) break;
        thread = CreateRemoteThread(process, nullptr, 0, load_library,
                                    remote_path, 0, nullptr);
        if (thread == nullptr) {
            AppendLog("inject: CreateRemoteThread failed err=" +
                      std::to_string(GetLastError()));
            break;
        }
        if (WaitForSingleObject(thread, 20000) != WAIT_OBJECT_0) break;
        DWORD exit_code = 0;
        if (!GetExitCodeThread(thread, &exit_code) || exit_code == 0) break;
        ok = true;
    } while (false);
    if (thread != nullptr) CloseHandle(thread);
    if (remote_path != nullptr) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    }
    CloseHandle(process);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--pid") == 0) {
            pid = static_cast<DWORD>(wcstoul(argv[i + 1], nullptr, 10));
        }
    }
    AppendLog("---- tap inject start pid=" + std::to_string(pid));
    if (pid == 0) {
        AppendLog("exit 2: missing --pid");
        return 2;
    }
    if (!IsElevated()) {
        AppendLog("exit 3: not elevated");
        return 3;
    }
    const auto host_pid = voicestick::FindXiaomiHidHostPid();
    if (!host_pid.has_value() || *host_pid != pid) {
        AppendLog("exit 4: registry host pid mismatch (registry=" +
                  std::to_string(host_pid.value_or(0)) + ")");
        return 4;
    }
    if (!EnableDebugPrivilege()) {
        AppendLog("exit 8: SeDebugPrivilege not assigned");
        return 8;
    }
    // 进程名校验须在 SeDebugPrivilege 启用之后：WUDFHost ACL 拒绝未启用
    // SeDebug 的进程 OpenProcess（提权管理员也一样，真机 Win11 26200 实测
    // err=5——Administrators 只是「有权启用」，未启用时照样被拒）。
    if (!TargetIsWudfHost(pid)) {
        AppendLog("exit 5: target is not wudfhost.exe");
        return 5;
    }
    // 源 DLL = 注入器同目录（MSI 安装目录）。
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring source_dll(exe_path);
    const size_t slash = source_dll.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        AppendLog("exit 6: exe path resolve failed");
        return 6;
    }
    source_dll.resize(slash);
    source_dll += L"\\";
    source_dll += kDllName;
    // 幂等前置：宿主已加载 DLL 时直接成功返回——部署副本可能被运行中的
    // DLL 锁定，先部署后查幂等会在重注入/应用更新场景撞文件锁。
    if (DllAlreadyLoaded(pid)) {
        AppendLog("exit 0: already injected (idempotent)");
        return 0;
    }
    const std::wstring deployed = DeployDll(source_dll);
    if (deployed.empty()) {
        AppendLog("exit 7: deploy failed");
        return 7;
    }
    if (!InjectLibrary(pid, deployed)) {
        AppendLog("exit 9: inject failed");
        return 9;
    }
    AppendLog("exit 0: injected");
    return 0;
}
