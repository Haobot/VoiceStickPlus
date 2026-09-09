// 划词纠错对话框（S1，Doc/Plan/selection-hotword-correction-and-asr-hotword-spike.md §1.1/1.2）：
// 错词醒目显示 + 候选按钮（LLM 异步生成、近音过滤后展示）+ 手输框。用户点选
// 候选或手输确定后 on_confirm(正确词) 并自毁——正确词由上层入 asr_hotwords，
// S2 守卫锚点域随即生效（后续口述近音变体自动纠正）。

#ifndef VOICESTICK_SELECTION_CORRECTION_DIALOG_H_
#define VOICESTICK_SELECTION_CORRECTION_DIALOG_H_

#include "app_config.h"
#include "llm_chat_client.h"
#include "selection_correction.h"

#include <Windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace voicestick {

// 云端候选生成客户端（OpenAI 兼容）：复用 LLMChatClient 网络层，仅候选域
// prompt 与解析过滤。本地引擎路径走协调器 GenerateCorrectionCandidates，
// 两条路在 Win32App 的 provider 里按配置二选一（云端优先）。
class LlmCorrectionCandidatesClient : public LLMChatClient {
public:
    explicit LlmCorrectionCandidatesClient(AppConfig config)
        : LLMChatClient(std::move(config)) {}

    // on_done 在后台线程回调：ok=false 网络失败（候选空）。
    void Request(const std::string& wrong_text, const std::string& context,
                 std::function<void(bool, std::vector<std::string>)> on_done) const;
};

class SelectionCorrectionDialog {
public:
    // 候选提供器：立即发起生成，完成后回调（后台线程，对话框内部转投
    // UI 线程）。由上层绑定云/本地引擎；无可用引擎时直接回调 (false, {})。
    using CandidatesProvider = std::function<void(
        std::function<void(bool ok, std::vector<std::string> candidates)>)>;

    SelectionCorrectionDialog(HINSTANCE instance, HWND owner,
                              std::string wrong_text, UiLanguage language,
                              CandidatesProvider provider);
    ~SelectionCorrectionDialog();

    SelectionCorrectionDialog(const SelectionCorrectionDialog&) = delete;
    SelectionCorrectionDialog& operator=(const SelectionCorrectionDialog&) = delete;

    void Show();

    // 用户确认正确词（UI 线程）：点候选或手输+确定。回调后对话框自毁，
    // 上层负责入热词表与反馈；入表结果的延迟清理（对象 reset）由上层
    // 在下次打开或退出时做，不得在本回调内 reset（DialogProc 栈未退）。
    std::function<void(const std::string& correct_word)> on_confirm;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param,
                                       LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildUi();
    void DestroyControls();
    void RebuildCandidateButtons();
    void CenterWindow();
    void HandleCandidates(bool ok, std::vector<std::string> candidates);
    void ConfirmWord(const std::string& word);
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    UiLanguage language_;
    UINT dpi_ = 96;
    std::string wrong_text_;
    CandidatesProvider provider_;

    HWND wrong_label_ = nullptr;
    HWND candidates_label_ = nullptr;
    HWND status_label_ = nullptr;
    std::vector<HWND> candidate_buttons_;
    HWND manual_label_ = nullptr;
    HWND manual_edit_ = nullptr;
    HWND ok_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    std::vector<HWND> all_controls_;
    std::vector<BYTE> dialog_template_;
    std::vector<std::string> candidates_;
    bool candidates_loaded_ = false;  // 区分「加载中」与「已加载」

    static constexpr int kClientWidth = 320;
    static constexpr int kClientHeight = 312;
    static constexpr UINT kEditId = 203;
    static constexpr UINT kOkId = 204;
    static constexpr UINT kCancelId = 205;
    static constexpr UINT kCandidateBaseId = 210;  // + index（最多 5 个）
    static constexpr UINT kMsgCandidates = WM_APP + 0x503;
};

}  // namespace voicestick

#endif  // VOICESTICK_SELECTION_CORRECTION_DIALOG_H_
