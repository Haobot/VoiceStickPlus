#pragma once

#include "com_port_selector.h"
#include "esptool_flash_command.h"
#include "esptool_progress.h"
#include "localization.h"
#include "voice_stick_flash_tool.h"

#include <Windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace voicestick {

class FlashToolDialog {
public:
    FlashToolDialog(HINSTANCE instance, int show_cmd);
    ~FlashToolDialog();
    int Run();  // 消息循环，返回退出码

private:
    // 对话框过程
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void OnInitDialog(HWND hwnd);
    void OnRefreshPorts();
    void OnBrowseFirmware();
    void OnFlash();
    void OnCancel();
    void OnFlashEvent(const FlashEvent& event);

    void BuildControls();
    void AppendLog(const std::wstring& line);
    void SetFlashing(bool flashing);
    void ResetFlashState();
    int Dp(int px) const;
    LPCDLGTEMPLATE BuildDialogTemplate();
    static DWORD WINAPI FlashThreadProc(void* param);

    HINSTANCE instance_ = nullptr;
    int show_cmd_ = SW_SHOWNORMAL;
    UINT dpi_ = 96;
    HWND hwnd_ = nullptr;
    HFONT ui_font_ = nullptr;

    HWND warning_label_ = nullptr;
    HWND port_combo_ = nullptr;
    HWND refresh_button_ = nullptr;
    HWND firmware_edit_ = nullptr;
    HWND browse_button_ = nullptr;
    HWND mode_combo_ = nullptr;
    HWND baud_combo_ = nullptr;
    HWND flash_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HWND close_button_ = nullptr;
    HWND progress_bar_ = nullptr;
    HWND status_label_ = nullptr;
    HWND log_edit_ = nullptr;

    std::vector<ComPortInfo> ports_;
    std::filesystem::path firmware_path_;
    FlashMode mode_ = FlashMode::kFullMerged;
    int baud_ = 921600;

    // B1 生命周期：worker 线程经 FlashThreadCtx 持有这两者的共享引用（见 .cc），
    // 对话框关窗/析构的 5s 有界等待超时后（esptool 子进程卡死）可安全释放本侧引用，
    // worker 跑完 Run() 对象才真正析构——原独占持有下超时即 use-after-free。
    std::shared_ptr<FlashTool> flash_tool_;
    std::shared_ptr<IFlashProcessRunner> runner_;
    HANDLE flash_thread_ = nullptr;
    bool flashing_ = false;

    std::vector<BYTE> dialog_template_;
};

} // namespace voicestick
