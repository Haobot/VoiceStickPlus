#include "clipboard_vault.h"

#include <map>
#include <utility>

namespace voicestick {

const ClipboardSnapshot::Entry* ClipboardSnapshot::Find(UINT format) const {
    for (const auto& entry : entries) {
        if (entry.format == format) return &entry;
    }
    return nullptr;
}

namespace {

// 句柄类格式：数据不是 HGLOBAL，无法字节快照（见类注释）。
bool IsHandleFormat(UINT format) {
    return format == CF_BITMAP || format == CF_METAFILEPICT ||
           format == CF_PALETTE || format == CF_ENHMETAFILE;
}

std::string FormatName(UINT format) {
    static const std::map<UINT, const char*> kStandardNames = {
        {CF_TEXT, "CF_TEXT"},         {CF_SYLK, "CF_SYLK"},
        {CF_DIF, "CF_DIF"},           {CF_TIFF, "CF_TIFF"},
        {CF_OEMTEXT, "CF_OEMTEXT"},   {CF_DIB, "CF_DIB"},
        {CF_UNICODETEXT, "CF_UNICODETEXT"}, {CF_LOCALE, "CF_LOCALE"},
        {CF_HDROP, "CF_HDROP"},       {CF_DIBV5, "CF_DIBV5"},
    };
    const auto it = kStandardNames.find(format);
    if (it != kStandardNames.end()) return it->second;
    wchar_t buffer[128]{};
    if (GetClipboardFormatNameW(format, buffer, 128) > 0) {
        std::string out(WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0,
                                            nullptr, nullptr) - 1,
                        '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out.data(),
                            static_cast<int>(out.size()) + 1, nullptr, nullptr);
        return out;
    }
    return "CF_" + std::to_string(format);
}

// message-only 窗口做剪贴板 owner（进程级单例，随进程回收）：P1 本机实证
// OpenClipboard(NULL) 写入的数据存在释放异常，带 owner 窗口则稳定。
HWND OwnerWindow() {
    static const HWND owner = []() -> HWND {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VoiceStickClipboardVault";
        RegisterClassW(&wc);  // 已存在（1410）= 复用
        return CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }();
    return owner;
}

} // namespace

ClipboardVault::ClipboardVault(int open_retries, int retry_delay_ms)
    : open_retries_(open_retries), retry_delay_ms_(retry_delay_ms) {}

bool ClipboardVault::OpenWithRetry() {
    const HWND owner = OwnerWindow();
    for (int i = 0; i < open_retries_; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(retry_delay_ms_);
    }
    return false;
}

ClipboardSnapshot ClipboardVault::Save() {
    ClipboardSnapshot snapshot;
    if (!OpenWithRetry()) {
        throw std::runtime_error("剪贴板持续被占用，无法快照");
    }
    for (UINT format = EnumClipboardFormats(0); format != 0;
         format = EnumClipboardFormats(format)) {
        if (IsHandleFormat(format)) continue;
        HANDLE handle = GetClipboardData(format);
        if (handle == nullptr) continue;  // 含延迟渲染未触发/触发失败
        const SIZE_T size = GlobalSize(handle);
        if (size == 0) continue;
        void* ptr = GlobalLock(handle);
        if (ptr == nullptr) continue;
        ClipboardSnapshot::Entry entry;
        entry.format = format;
        entry.name = FormatName(format);
        entry.data.assign(static_cast<const std::uint8_t*>(ptr),
                          static_cast<const std::uint8_t*>(ptr) + size);
        GlobalUnlock(handle);
        snapshot.entries.push_back(std::move(entry));
    }
    CloseClipboard();
    return snapshot;
}

bool ClipboardVault::Restore(const ClipboardSnapshot& snapshot) {
    if (!OpenWithRetry()) return false;
    EmptyClipboard();
    for (const auto& entry : snapshot.entries) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, entry.data.size());
        if (memory == nullptr) continue;
        void* ptr = GlobalLock(memory);
        if (ptr == nullptr) {
            GlobalFree(memory);
            continue;
        }
        memcpy(ptr, entry.data.data(), entry.data.size());
        GlobalUnlock(memory);
        if (SetClipboardData(entry.format, memory) == nullptr) {
            GlobalFree(memory);  // 写回失败自己释放，尽力恢复下一格式
        }
    }
    CloseClipboard();
    return true;
}

} // namespace voicestick
