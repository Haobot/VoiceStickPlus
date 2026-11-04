#include "clipboard_field_reader.h"

#include "clipboard_vault.h"
#include "log.h"

#include <Windows.h>

#include <exception>
#include <string>
#include <string_view>

namespace voicestick {
namespace {

// Ctrl+C 到 SetClipboardData 是异步的：轮询间隔 20ms（总等待窗口见
// ReadFieldFullText 的 wait_for_update）。
constexpr DWORD kClipboardPollIntervalMs = 20;

std::string Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

void SendKey(WORD virtual_key, bool key_down) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtual_key;
    if (!key_down) input.ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        LogApp("clipboard field reader: SendInput failed err=" +
               std::to_string(GetLastError()));
    }
    // 自绘编辑器对按键合并敏感：每个键留一拍，确保 Ctrl+A/C 被独立处理。
    Sleep(15);
}

void SendCtrlCombo(WORD virtual_key) {
    SendKey(VK_CONTROL, true);
    SendKey(virtual_key, true);
    SendKey(virtual_key, false);
    SendKey(VK_CONTROL, false);
}

// 读剪贴板 CF_UNICODETEXT（打开带重试：目标 Ctrl+C 后可能短暂占用）。无文本
// 格式返回 false（区别于「读到空串」）。
bool ReadClipboardUnicodeText(std::wstring* out) {
    out->clear();
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!OpenClipboard(nullptr)) {
            Sleep(25);
            continue;
        }
        bool ok = false;
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle != nullptr) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
            if (text != nullptr) {
                out->assign(text);
                GlobalUnlock(handle);
                ok = true;
            }
        }
        CloseClipboard();
        return ok;
    }
    return false;
}

// 剪贴板回读通道（Win32）：快照剪贴板 → Ctrl+A → Ctrl+C → 等序列号变化 →
// 读 CF_UNICODETEXT → 恢复快照 → 右方向键取消选区。
class ClipboardFieldReaderWin : public IClipboardFieldReader {
public:
    bool ReadFieldFullText(std::string* out) override {
        if (out == nullptr) return false;
        out->clear();
        // 快照失败（剪贴板被占用）→ 不发任何按键，避免污染用户剪贴板。
        ClipboardVault vault;
        ClipboardSnapshot snapshot;
        try {
            snapshot = vault.Save();
        } catch (const std::exception& e) {
            LogApp(std::string("clipboard field reader: snapshot failed: ") + e.what());
            return false;
        }
        const DWORD sequence_before = GetClipboardSequenceNumber();
        SendCtrlCombo('A');
        Sleep(120);  // 全选需要稳定一拍（自绘编辑器选区建立不是瞬时的）。
        SendCtrlCombo('C');
        auto wait_for_update = [&]() {
            for (int attempt = 0; attempt < 50; ++attempt) {  // 1000ms
                if (GetClipboardSequenceNumber() != sequence_before) return true;
                Sleep(kClipboardPollIntervalMs);
            }
            return false;
        };
        bool updated = wait_for_update();
        if (!updated) {
            // 某些自绘编辑器对第一次 Ctrl+C 不响应：再给一拍重试一次。
            LogApp("clipboard field reader: no clipboard update after Ctrl+C, retry");
            Sleep(100);
            SendCtrlCombo('C');
            updated = wait_for_update();
        }
        std::wstring text;
        const bool read_ok = updated && ReadClipboardUnicodeText(&text);
        LogApp("clipboard field reader: seq_before=" + std::to_string(sequence_before) +
               " seq_after=" + std::to_string(GetClipboardSequenceNumber()) +
               " updated=" + (updated ? "1" : "0") +
               " read_ok=" + (read_ok ? "1" : "0") +
               " text_len=" + std::to_string(text.size()));
        // 复制后目标可能短暂占用剪贴板：恢复前留一拍。
        Sleep(50);
        if (!vault.Restore(snapshot)) {
            LogApp("clipboard field reader: restore clipboard failed");
        }
        // 取消选区：右方向键（Windows 标准语义：折叠到选区末尾）。
        SendKey(VK_RIGHT, true);
        SendKey(VK_RIGHT, false);
        if (!read_ok) {
            LogApp("clipboard field reader: clipboard not updated or no text");
            return false;
        }
        *out = Utf8FromUtf16(text);
        return true;
    }
};

}  // namespace

std::unique_ptr<IClipboardFieldReader> CreateClipboardFieldReader() {
    return std::make_unique<ClipboardFieldReaderWin>();
}

}  // namespace voicestick
