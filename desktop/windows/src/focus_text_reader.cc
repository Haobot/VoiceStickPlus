#include "focus_text_reader.h"

#include "log.h"

#include <Windows.h>
// WIN32_LEAN_AND_MEAN（voicestick_core PUBLIC 透传）会从 Windows.h 排除 ole2.h，
// 而 <uiautomation.h> 依赖 COM 接口定义，必须显式包含 ole2.h 在前
//（与 selection_hotword_manager.cc 同范式）。
#include <ole2.h>
#include <uiautomation.h>

#include <climits>
#include <cstring>
#include <string>
#include <string_view>

// oleaut32：SysStringLen/SysFreeString；uiautomationcore：UIA Client 组件。
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace voicestick {

namespace {

// 字段尾部读取上限（UTF-16 code unit）：只取最后约 512 个字符做后缀匹配，避免
// 大文档 GetText 全量读取阻塞 UI 线程（Doc §6.3）。
constexpr int kTailUtf16Limit = 512;

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** Put() {
        Reset();
        return &ptr_;
    }
    T* Get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void Reset() {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

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

class FocusTextReaderWin : public IFocusTextReader {
public:
    bool ReadFocusedField(VoiceEditFieldState* out) override {
        if (out == nullptr) return false;
        *out = VoiceEditFieldState{};

        // UI Automation 通过 COM 调用焦点控件；Win32App 主线程已
        // init_apartment（STA），本类只在 UI 线程调用（协调器收尾路径）。
        ComPtr<IUIAutomation> automation;
        HRESULT hr = CoCreateInstance(
            __uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(automation.Put()));
        if (FAILED(hr) || !automation) {
            LogApp("voice edit: CoCreateInstance(CUIAutomation8) failed hr=" +
                   std::to_string(static_cast<long long>(hr)));
            return false;
        }

        ComPtr<IUIAutomationElement> focused;
        hr = automation->GetFocusedElement(focused.Put());
        if (FAILED(hr) || !focused) {
            LogApp("voice edit: GetFocusedElement failed hr=" +
                   std::to_string(static_cast<long long>(hr)));
            return false;
        }

        // 密码框：不读取内容，直接标记（planner 任何模式都拒绝）。
        BOOL is_password = FALSE;
        if (SUCCEEDED(focused->get_CurrentIsPassword(&is_password)) && is_password) {
            out->is_password = true;
            return false;
        }

        ComPtr<IUIAutomationTextPattern> text_pattern;
        hr = focused->GetCurrentPatternAs(UIA_TextPatternId,
                                          __uuidof(IUIAutomationTextPattern),
                                          reinterpret_cast<void**>(text_pattern.Put()));
        if (FAILED(hr) || !text_pattern) {
            // 不支持 TextPattern 的控件（自绘编辑器/终端等）→ 不可验证。
            return false;
        }

        ComPtr<IUIAutomationTextRange> document;
        hr = text_pattern->GetDocumentRange(document.Put());
        if (FAILED(hr) || !document) {
            LogApp("voice edit: GetDocumentRange failed hr=" +
                   std::to_string(static_cast<long long>(hr)));
            return false;
        }

        // ---- 尾部文本：Clone 文档 range → Start 端点回退约 512 字符 → GetText ----
        ComPtr<IUIAutomationTextRange> tail;
        hr = document->Clone(tail.Put());
        if (FAILED(hr) || !tail) return false;
        int moved = 0;
        hr = tail->MoveEndpointByUnit(UIA_TextPatternRangeEndpoint_Start,
                                      UIA_TextUnit_Character, -kTailUtf16Limit, &moved);
        if (FAILED(hr)) {
            LogApp("voice edit: MoveEndpointByUnit failed hr=" +
                   std::to_string(static_cast<long long>(hr)));
            return false;
        }
        BSTR text = nullptr;
        hr = tail->GetText(-1, &text);
        if (FAILED(hr)) return false;
        if (text != nullptr) {
            const UINT length = SysStringLen(text);
            out->tail_text = Utf8FromUtf16(std::wstring_view(text, length));
            out->tail_utf16_length = length;
            SysFreeString(text);
        }
        // 尾部被截断（字段比 tail 长）：仅用于日志/排障，planner 只做尾部后缀匹配。
        {
            int compare_start = 0;
            if (SUCCEEDED(tail->CompareEndpoints(UIA_TextPatternRangeEndpoint_Start,
                                                 document.Get(),
                                                 UIA_TextPatternRangeEndpoint_Start,
                                                 &compare_start))) {
                out->truncated = compare_start != 0;
            }
        }

        // ---- 选区/光标：恰 1 个折叠选区且末端 == 文档末端 ----
        ComPtr<IUIAutomationTextRangeArray> selections;
        hr = text_pattern->GetSelection(selections.Put());
        if (FAILED(hr) || !selections) {
            LogApp("voice edit: GetSelection failed hr=" +
                   std::to_string(static_cast<long long>(hr)));
            return false;
        }
        int selection_count = 0;
        selections->get_Length(&selection_count);
        if (selection_count == 0) {
            // 无选区信息（部分控件不支持）→ 不可验证。
            return false;
        }
        if (selection_count != 1) {
            // 多选区：视为用户存在选区（planner 拒绝并提示先取消选中）。
            out->readable = true;
            out->selection_collapsed = false;
            out->caret_at_end = false;
            return true;
        }
        ComPtr<IUIAutomationTextRange> selection;
        hr = selections->GetElement(0, selection.Put());
        if (FAILED(hr) || !selection) return false;
        int collapsed = 0;
        hr = selection->CompareEndpoints(UIA_TextPatternRangeEndpoint_Start,
                                         selection.Get(),
                                         UIA_TextPatternRangeEndpoint_End, &collapsed);
        if (FAILED(hr)) return false;
        out->readable = true;
        out->selection_collapsed = collapsed == 0;
        if (out->selection_collapsed) {
            // 光标是否在文档末尾：用 CompareEndpoints 比较（不依赖字符偏移）。
            int at_end = 0;
            hr = selection->CompareEndpoints(UIA_TextPatternRangeEndpoint_End,
                                             document.Get(),
                                             UIA_TextPatternRangeEndpoint_End, &at_end);
            if (FAILED(hr)) return false;
            out->caret_at_end = at_end == 0;
        }
        return true;
    }
};

class TargetedFieldEditorWin : public ITargetedFieldEditor {
public:
    bool SelectAndReadBack(std::size_t tail_relative_start_utf16,
                           std::size_t length_utf16,
                           std::string* selected_text) override {
        if (selected_text == nullptr) return false;
        selected_text->clear();
        ComPtr<IUIAutomation> automation;
        HRESULT hr = CoCreateInstance(
            __uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(automation.Put()));
        if (FAILED(hr) || !automation) return false;

        ComPtr<IUIAutomationElement> focused;
        hr = automation->GetFocusedElement(focused.Put());
        if (FAILED(hr) || !focused) return false;
        ComPtr<IUIAutomationTextPattern> text_pattern;
        hr = focused->GetCurrentPatternAs(UIA_TextPatternId,
                                          __uuidof(IUIAutomationTextPattern),
                                          reinterpret_cast<void**>(text_pattern.Put()));
        if (FAILED(hr) || !text_pattern) return false;
        ComPtr<IUIAutomationTextRange> document;
        hr = text_pattern->GetDocumentRange(document.Put());
        if (FAILED(hr) || !document) return false;

        // 文档长度：Start 端点前进 INT_MAX，实际移动数即字符数。
        ComPtr<IUIAutomationTextRange> measure;
        hr = document->Clone(measure.Put());
        if (FAILED(hr) || !measure) return false;
        int doc_length = 0;
        hr = measure->MoveEndpointByUnit(UIA_TextPatternRangeEndpoint_Start,
                                         UIA_TextUnit_Character, INT_MAX, &doc_length);
        if (FAILED(hr)) return false;

        // 尾部窗口长度（与 FocusTextReaderWin 同一窗口定义：Start 回退 512 码元）。
        ComPtr<IUIAutomationTextRange> tail;
        hr = document->Clone(tail.Put());
        if (FAILED(hr) || !tail) return false;
        int tail_back = 0;
        hr = tail->MoveEndpointByUnit(UIA_TextPatternRangeEndpoint_Start,
                                      UIA_TextUnit_Character, -kTailUtf16Limit, &tail_back);
        if (FAILED(hr)) return false;
        BSTR tail_bstr = nullptr;
        hr = tail->GetText(-1, &tail_bstr);
        if (FAILED(hr)) return false;
        const std::size_t tail_length =
            tail_bstr != nullptr ? SysStringLen(tail_bstr) : 0;
        if (tail_bstr != nullptr) SysFreeString(tail_bstr);

        const std::size_t doc_len = static_cast<std::size_t>(doc_length);
        if (tail_length > doc_len) return false;
        const std::size_t tail_window_start = doc_len - tail_length;
        if (tail_relative_start_utf16 + length_utf16 > tail_length) return false;
        const std::size_t abs_start = tail_window_start + tail_relative_start_utf16;
        if (abs_start + length_utf16 > doc_len) return false;

        // 选区：clone 文档 range，Start 前进 abs_start，End 收束到 abs_start+length。
        ComPtr<IUIAutomationTextRange> selection;
        hr = document->Clone(selection.Put());
        if (FAILED(hr) || !selection) return false;
        int moved = 0;
        hr = selection->MoveEndpointByUnit(UIA_TextPatternRangeEndpoint_Start,
                                           UIA_TextUnit_Character,
                                           static_cast<int>(abs_start), &moved);
        if (FAILED(hr) || static_cast<std::size_t>(moved) != abs_start) return false;
        const std::size_t abs_end = abs_start + length_utf16;
        hr = selection->MoveEndpointByUnit(
            UIA_TextPatternRangeEndpoint_End, UIA_TextUnit_Character,
            -(static_cast<int>(doc_len - abs_end)), &moved);
        if (FAILED(hr)) return false;
        hr = selection->Select();
        if (FAILED(hr)) return false;

        // 读回实际选中文本（GetSelection → 恰 1 个 range → GetText）。
        ComPtr<IUIAutomationTextRangeArray> selections;
        hr = text_pattern->GetSelection(selections.Put());
        if (FAILED(hr) || !selections) return false;
        int selection_count = 0;
        selections->get_Length(&selection_count);
        if (selection_count != 1) return false;
        ComPtr<IUIAutomationTextRange> actual;
        hr = selections->GetElement(0, actual.Put());
        if (FAILED(hr) || !actual) return false;
        BSTR selected = nullptr;
        hr = actual->GetText(-1, &selected);
        if (FAILED(hr)) return false;
        if (selected != nullptr) {
            *selected_text = Utf8FromUtf16(std::wstring_view(selected, SysStringLen(selected)));
            SysFreeString(selected);
        }
        return true;
    }

    bool MoveCaretToEnd() override {
        ComPtr<IUIAutomation> automation;
        HRESULT hr = CoCreateInstance(
            __uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(automation.Put()));
        if (FAILED(hr) || !automation) return false;
        ComPtr<IUIAutomationElement> focused;
        hr = automation->GetFocusedElement(focused.Put());
        if (FAILED(hr) || !focused) return false;
        ComPtr<IUIAutomationTextPattern> text_pattern;
        hr = focused->GetCurrentPatternAs(UIA_TextPatternId,
                                          __uuidof(IUIAutomationTextPattern),
                                          reinterpret_cast<void**>(text_pattern.Put()));
        if (FAILED(hr) || !text_pattern) return false;
        ComPtr<IUIAutomationTextRange> document;
        hr = text_pattern->GetDocumentRange(document.Put());
        if (FAILED(hr) || !document) return false;
        // 折叠到文档末尾：Start 前进 INT_MAX（钳制到 End）。
        ComPtr<IUIAutomationTextRange> caret;
        hr = document->Clone(caret.Put());
        if (FAILED(hr) || !caret) return false;
        int moved = 0;
        hr = caret->MoveEndpointByUnit(UIA_TextPatternRangeEndpoint_Start,
                                       UIA_TextUnit_Character, INT_MAX, &moved);
        if (FAILED(hr)) return false;
        return SUCCEEDED(caret->Select());
    }
};

}  // namespace

std::unique_ptr<IFocusTextReader> CreateFocusTextReader() {
    return std::make_unique<FocusTextReaderWin>();
}

std::unique_ptr<ITargetedFieldEditor> CreateTargetedFieldEditor() {
    return std::make_unique<TargetedFieldEditorWin>();
}

}  // namespace voicestick
