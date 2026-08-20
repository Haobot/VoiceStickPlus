#include "flash_tool_dialog.h"

#include "dpi_util.h"

#include <CommCtrl.h>
#include <commdlg.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace voicestick {

namespace {

// 控件 ID
constexpr UINT kIdWarningLabel = 1001;
constexpr UINT kIdPortCombo     = 1002;
constexpr UINT kIdRefreshBtn    = 1003;
constexpr UINT kIdFirmwareEdit  = 1004;
constexpr UINT kIdBrowseBtn     = 1005;
constexpr UINT kIdModeCombo     = 1006;
constexpr UINT kIdBaudCombo     = 1007;
constexpr UINT kIdFlashBtn      = 1008;
constexpr UINT kIdCancelBtn     = 1009;
constexpr UINT kIdCloseBtn      = 1010;
constexpr UINT kIdProgressBar   = 1011;
constexpr UINT kIdStatusLabel   = 1012;
constexpr UINT kIdLogEdit       = 1013;

// 自定义窗口消息（工作线程 → UI 线程）
constexpr UINT kWmFlashEvent = WM_APP + 1;
constexpr UINT kWmFlashDone  = WM_APP + 2;

// 布局常量（Dp 缩放前像素值）
constexpr int kClientWidth  = 460;
constexpr int kClientHeight = 540;

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), len);
    return wide;
}

void AlignDialogData(std::vector<BYTE>* buffer, std::size_t alignment) {
    while (buffer->size() % alignment != 0) buffer->push_back(0);
}

void AppendDialogData(std::vector<BYTE>* buffer, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

void AppendDialogWord(std::vector<BYTE>* buffer, WORD value) {
    AppendDialogData(buffer, &value, sizeof(value));
}

void AppendDialogWideString(std::vector<BYTE>* buffer, const wchar_t* text) {
    while (text && *text) {
        AppendDialogWord(buffer, static_cast<WORD>(*text));
        ++text;
    }
    AppendDialogWord(buffer, 0);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                 HINSTANCE inst, DWORD style = SS_RIGHT) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                  UINT id, HINSTANCE inst, DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           inst, nullptr);
}

HWND CreateCombo(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst) {
    return CreateWindowExW(0, L"COMBOBOX", L"",
                           WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           inst, nullptr);
}

// 查找 esptool 自包含 Python 运行时路径。
// 开发构建：exe 同级 flash_payload\python\python.exe
// MSI 安装：exe 同级 FlashTool\python\python.exe
std::filesystem::path FindPythonExe() {
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) return {};
    const auto exe_dir = std::filesystem::path(exe_path).parent_path();
    const auto dev_path = exe_dir / L"flash_payload" / L"python" / L"python.exe";
    if (std::filesystem::exists(dev_path)) return dev_path;
    const auto msi_path = exe_dir / L"FlashTool" / L"python" / L"python.exe";
    if (std::filesystem::exists(msi_path)) return msi_path;
    return dev_path;  // 回退到开发路径，flash_tool 会报错提示
}

// 检测 VoiceStickApp 是否在运行（烧录前警告用户先退出）。
bool IsVoiceStickRunning() {
    return FindWindowW(L"VoiceStickWindow", nullptr) != nullptr;
}

// esptool 子进程运行器（CreateProcess + 管道读取）。
class FlashProcessRunner : public IFlashProcessRunner {
public:
    ~FlashProcessRunner() override { Cancel(); }

    int Run(const std::vector<std::wstring>& argv,
            const std::function<void(const std::string& line)>& on_line) override {
        if (argv.empty()) return -1;

        std::wstring cmd_line = JoinCommandLine(argv);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE pipe_read = nullptr, pipe_write = nullptr;
        if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) return -1;
        SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = pipe_write;
        si.hStdError = pipe_write;

        PROCESS_INFORMATION pi{};

        std::vector<wchar_t> cmd_buf(cmd_line.begin(), cmd_line.end());
        cmd_buf.push_back(L'\0');

        if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pipe_read);
            CloseHandle(pipe_write);
            return -1;
        }

        CloseHandle(pipe_write);
        process_handle_.store(pi.hProcess);
        const HANDLE thread_handle = pi.hThread;

        // 逐行读取 stdout/stderr
        std::string accumulated;
        char buffer[4096];
        DWORD bytes_read = 0;
        while (ReadFile(pipe_read, buffer, sizeof(buffer),
                        &bytes_read, nullptr) &&
               bytes_read > 0) {
            accumulated.append(buffer, bytes_read);
            std::size_t pos = 0;
            while (true) {
                auto newline = accumulated.find('\n', pos);
                if (newline == std::string::npos) break;
                std::string line = accumulated.substr(pos, newline - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (on_line) on_line(line);
                pos = newline + 1;
            }
            accumulated = accumulated.substr(pos);
        }

        CloseHandle(pipe_read);
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);

        // 先清空原子句柄再 CloseHandle，避免 Cancel() 对已关闭句柄调用 TerminateProcess。
        process_handle_.store(nullptr);
        CloseHandle(pi.hProcess);
        CloseHandle(thread_handle);

        // 处理最后无换行符的残留输出
        if (!accumulated.empty() && on_line) {
            on_line(accumulated);
        }

        return static_cast<int>(exit_code);
    }

    void Cancel() override {
        HANDLE h = process_handle_.load();
        if (h) {
            TerminateProcess(h, static_cast<UINT>(kFlashCancelledExitCode));
        }
    }

private:
    std::atomic<HANDLE> process_handle_{nullptr};
};

} // namespace

FlashToolDialog::FlashToolDialog(HINSTANCE instance, int show_cmd)
    : instance_(instance), show_cmd_(show_cmd) {}

FlashToolDialog::~FlashToolDialog() {
    if (flashing_ && flash_thread_) {
        if (flash_tool_) flash_tool_->Cancel();
        WaitForSingleObject(flash_thread_, 5000);
        CloseHandle(flash_thread_);
        flash_thread_ = nullptr;
    }
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
    if (hwnd_) DestroyWindow(hwnd_);
}

int FlashToolDialog::Run() {
    hwnd_ = CreateDialogIndirectParamW(instance_, BuildDialogTemplate(), nullptr,
                                       DialogProc, reinterpret_cast<LPARAM>(this));
    if (!hwnd_) return 1;
    ShowWindow(hwnd_, show_cmd_);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

INT_PTR CALLBACK FlashToolDialog::DialogProc(HWND hwnd, UINT msg,
                                             WPARAM wp, LPARAM lp) {
    auto* dialog = reinterpret_cast<FlashToolDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        dialog = reinterpret_cast<FlashToolDialog*>(lp);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        dialog->OnInitDialog(hwnd);
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(msg, wp, lp) : FALSE;
}

INT_PTR FlashToolDialog::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdRefreshBtn:
            if (HIWORD(wp) == BN_CLICKED) OnRefreshPorts();
            return TRUE;
        case kIdBrowseBtn:
            if (HIWORD(wp) == BN_CLICKED) OnBrowseFirmware();
            return TRUE;
        case kIdFlashBtn:
            if (HIWORD(wp) == BN_CLICKED) OnFlash();
            return TRUE;
        case kIdCancelBtn:
            if (HIWORD(wp) == BN_CLICKED) OnCancel();
            return TRUE;
        case kIdCloseBtn:
            if (HIWORD(wp) == BN_CLICKED) {
                SendMessageW(hwnd_, WM_CLOSE, 0, 0);
            }
            return TRUE;
        case kIdModeCombo:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int idx = static_cast<int>(
                    SendMessageW(mode_combo_, CB_GETCURSEL, 0, 0));
                switch (idx) {
                case 0: mode_ = FlashMode::kFullMerged; break;
                case 1: mode_ = FlashMode::kAppOnly; break;
                case 2: mode_ = FlashMode::kEraseThenFull; break;
                default: break;
                }
            }
            return TRUE;
        case kIdBaudCombo:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int idx = static_cast<int>(
                    SendMessageW(baud_combo_, CB_GETCURSEL, 0, 0));
                baud_ = (idx == 0) ? 115200 : (idx == 1) ? 460800 : 921600;
            }
            return TRUE;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        if (flashing_ && flash_thread_) {
            const auto lang = EffectiveUiLanguage(UiLanguage::kSystem);
            const bool zh = (lang == UiLanguage::kSimplifiedChinese);
            if (MessageBoxW(hwnd_,
                    zh ? L"正在烧录中，确定取消并关闭吗？"
                       : L"Flashing in progress. Cancel and close?",
                    zh ? L"确认" : L"Confirm",
                    MB_YESNO | MB_ICONQUESTION) == IDNO) {
                return TRUE;
            }
            OnCancel();
            WaitForSingleObject(flash_thread_, 5000);
            CloseHandle(flash_thread_);
            flash_thread_ = nullptr;
            flashing_ = false;
            flash_tool_.reset();
            runner_.reset();
            // 排空残留的 flash 事件消息，防止堆分配泄漏
            MSG drain;
            while (PeekMessageW(&drain, nullptr, kWmFlashEvent, kWmFlashDone, PM_REMOVE)) {
                if (drain.message == kWmFlashEvent) {
                    delete reinterpret_cast<FlashEvent*>(drain.lParam);
                }
            }
        }
        DestroyWindow(hwnd_);
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        hwnd_ = nullptr;
        return TRUE;
    case WM_DPICHANGED: {
        const UINT new_dpi = HIWORD(wp);
        if (new_dpi != 0 && new_dpi != dpi_) {
            dpi_ = new_dpi;
            const auto* rect = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            // 销毁所有子控件后按新 DPI 重建
            HWND child = GetWindow(hwnd_, GW_CHILD);
            while (child) {
                HWND next = GetWindow(child, GW_HWNDNEXT);
                DestroyWindow(child);
                child = next;
            }
            if (ui_font_) {
                DeleteObject(ui_font_);
                ui_font_ = nullptr;
            }
            warning_label_ = nullptr;
            port_combo_ = nullptr;
            refresh_button_ = nullptr;
            firmware_edit_ = nullptr;
            browse_button_ = nullptr;
            mode_combo_ = nullptr;
            baud_combo_ = nullptr;
            flash_button_ = nullptr;
            cancel_button_ = nullptr;
            close_button_ = nullptr;
            progress_bar_ = nullptr;
            status_label_ = nullptr;
            log_edit_ = nullptr;
            BuildControls();
            // 恢复控件状态（mode_、baud_、firmware_path_ 是成员变量，不受重建影响）
            SendMessageW(mode_combo_, CB_SETCURSEL,
                (mode_ == FlashMode::kFullMerged) ? 0 :
                (mode_ == FlashMode::kAppOnly) ? 1 : 2, 0);
            SendMessageW(baud_combo_, CB_SETCURSEL,
                (baud_ == 115200) ? 0 : (baud_ == 460800) ? 1 : 2, 0);
            if (!firmware_path_.empty()) {
                SetWindowTextW(firmware_edit_, firmware_path_.wstring().c_str());
            }
            OnRefreshPorts();
            if (flashing_) SetFlashing(true);
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        const auto dc = reinterpret_cast<HDC>(wp);
        const auto control = reinterpret_cast<HWND>(lp);
        if (control == warning_label_) {
            // 警告横幅：浅黄背景（COLOR_INFOBK / COLOR_INFOTEXT）
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, GetSysColor(COLOR_INFOBK));
            SetTextColor(dc, GetSysColor(COLOR_INFOTEXT));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_INFOBK));
        }
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case kWmFlashEvent: {
        // 工作线程通过堆分配传递事件，unique_ptr 保证释放
        std::unique_ptr<FlashEvent> event(reinterpret_cast<FlashEvent*>(lp));
        if (event) OnFlashEvent(*event);
        return TRUE;
    }
    case kWmFlashDone:
        ResetFlashState();
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

void FlashToolDialog::OnInitDialog(HWND /*hwnd*/) {
    const auto lang = EffectiveUiLanguage(UiLanguage::kSystem);
    const bool zh = (lang == UiLanguage::kSimplifiedChinese);
    SetWindowTextW(hwnd_, zh ? L"VoiceStick 固件烧录工具" : L"VoiceStick Flash Tool");

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // 按客户区尺寸调整窗口外框
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), Dp(kClientHeight)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    BuildControls();

    // 居中显示
    RECT window_rect{};
    GetWindowRect(hwnd_, &window_rect);
    const RECT work = GetWorkAreaForWindow(hwnd_);
    const int x = work.left + ((work.right - work.left) -
                               (window_rect.right - window_rect.left)) / 2;
    const int y = work.top + ((work.bottom - work.top) -
                              (window_rect.bottom - window_rect.top)) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    OnRefreshPorts();
}

void FlashToolDialog::BuildControls() {
    const auto lang = EffectiveUiLanguage(UiLanguage::kSystem);
    const bool zh = (lang == UiLanguage::kSimplifiedChinese);

    ui_font_ = CreateUiFont(dpi_);

    auto menu_id = [](UINT id) {
        return reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id));
    };

    const int lbl_x = Dp(10);
    const int lbl_w = Dp(75);
    const int lbl_h = Dp(20);
    const int ctrl_x = Dp(90);
    const int ctrl_w = Dp(250);

    // 警告横幅（带 ID，供 WM_CTLCOLORSTATIC 识别）
    warning_label_ = CreateWindowExW(0, L"STATIC",
        zh ? L"烧录会覆盖设备固件。请用 USB 线连接设备；若 VoiceStick 正在运行，建议先退出。"
           : L"Flashing overwrites firmware. Connect device via USB; quit VoiceStick if running.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        Dp(10), Dp(10), Dp(kClientWidth - 20), Dp(30),
        hwnd_, menu_id(kIdWarningLabel), instance_, nullptr);

    // 串口行 (y=45)
    CreateLabel(hwnd_, zh ? L"串口:" : L"Port:",
                lbl_x, Dp(48), lbl_w, lbl_h, instance_);
    port_combo_ = CreateCombo(hwnd_, ctrl_x, Dp(45), ctrl_w, Dp(200),
                              kIdPortCombo, instance_);
    refresh_button_ = CreateButton(hwnd_,
        zh ? L"刷新" : L"Refresh",
        Dp(350), Dp(45), Dp(100), Dp(24), kIdRefreshBtn, instance_);

    // 固件行 (y=77)
    CreateLabel(hwnd_, zh ? L"固件:" : L"Firmware:",
                lbl_x, Dp(80), lbl_w, lbl_h, instance_);
    firmware_edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        ctrl_x, Dp(77), ctrl_w, Dp(24), hwnd_,
        menu_id(kIdFirmwareEdit), instance_, nullptr);
    browse_button_ = CreateButton(hwnd_,
        zh ? L"浏览…" : L"Browse…",
        Dp(350), Dp(77), Dp(100), Dp(24), kIdBrowseBtn, instance_);

    // 模式行 (y=109)
    CreateLabel(hwnd_, zh ? L"模式:" : L"Mode:",
                lbl_x, Dp(112), lbl_w, lbl_h, instance_);
    mode_combo_ = CreateCombo(hwnd_, ctrl_x, Dp(109), ctrl_w, Dp(200),
                              kIdModeCombo, instance_);
    SendMessageW(mode_combo_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(zh ? L"整包烧录 (0x0)" : L"Full merged (0x0)"));
    SendMessageW(mode_combo_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(zh ? L"仅应用 (0x10000)" : L"App only (0x10000)"));
    SendMessageW(mode_combo_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(zh ? L"先擦除再整包" : L"Erase then full"));
    SendMessageW(mode_combo_, CB_SETCURSEL, 0, 0);

    // 波特率行 (y=141)
    CreateLabel(hwnd_, zh ? L"波特率:" : L"Baud:",
                lbl_x, Dp(144), lbl_w, lbl_h, instance_);
    baud_combo_ = CreateCombo(hwnd_, ctrl_x, Dp(141), Dp(120), Dp(200),
                              kIdBaudCombo, instance_);
    SendMessageW(baud_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"115200"));
    SendMessageW(baud_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"460800"));
    SendMessageW(baud_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"921600"));
    SendMessageW(baud_combo_, CB_SETCURSEL, 2, 0);

    // 按钮行 (y=175)
    flash_button_ = CreateButton(hwnd_,
        zh ? L"开始烧录" : L"Start",
        ctrl_x, Dp(175), Dp(100), Dp(30), kIdFlashBtn, instance_, BS_DEFPUSHBUTTON);
    cancel_button_ = CreateButton(hwnd_,
        zh ? L"取消" : L"Cancel",
        Dp(200), Dp(175), Dp(80), Dp(30), kIdCancelBtn, instance_);
    close_button_ = CreateButton(hwnd_,
        zh ? L"关闭" : L"Close",
        Dp(350), Dp(175), Dp(100), Dp(30), kIdCloseBtn, instance_);

    // 进度条 (y=215)
    progress_bar_ = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE,
        Dp(10), Dp(215), Dp(kClientWidth - 20), Dp(18),
        hwnd_, menu_id(kIdProgressBar), instance_, nullptr);
    SendMessageW(progress_bar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(progress_bar_, PBM_SETPOS, 0, 0);

    // 状态标签 (y=239)
    status_label_ = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        Dp(10), Dp(239), Dp(kClientWidth - 20), Dp(20),
        hwnd_, menu_id(kIdStatusLabel), instance_, nullptr);

    // 日志输出 (y=265，填满剩余区域)
    log_edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_READONLY,
        Dp(10), Dp(265), Dp(kClientWidth - 20), Dp(kClientHeight - 275),
        hwnd_, menu_id(kIdLogEdit), instance_, nullptr);

    // 统一设置字体
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }

    // 初始按钮状态：取消禁用
    EnableWindow(cancel_button_, FALSE);
}

void FlashToolDialog::OnRefreshPorts() {
    const auto lang = EffectiveUiLanguage(UiLanguage::kSystem);
    const bool zh = (lang == UiLanguage::kSimplifiedChinese);

    ports_ = EnumerateComPorts();
    SendMessageW(port_combo_, CB_RESETCONTENT, 0, 0);

    int best_idx = -1;
    int best_score = -1;
    for (int i = 0; i < static_cast<int>(ports_.size()); ++i) {
        const auto& port = ports_[i];
        std::wstring display = port.device + L" — " + port.description;
        SendMessageW(port_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(display.c_str()));
        const int score = ScoreComPort(port);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        SendMessageW(port_combo_, CB_SETCURSEL, best_idx, 0);
        SetWindowTextW(status_label_,
            ((zh ? L"已自动选择: " : L"Auto-selected: ")
             + ports_[best_idx].device).c_str());
        EnableWindow(flash_button_, TRUE);
    } else {
        SetWindowTextW(status_label_,
            zh ? L"未检测到串口，请连接设备后刷新。"
               : L"No serial ports found. Connect device and refresh.");
        EnableWindow(flash_button_, FALSE);
    }
}

void FlashToolDialog::OnBrowseFirmware() {
    const auto lang = EffectiveUiLanguage(UiLanguage::kSystem);
    const bool zh = (lang == UiLanguage::kSimplifiedChinese);

    wchar_t file_path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = file_path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Firmware (*.bin)\0*.bin\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = zh ? L"选择固件文件" : L"Select firmware file";

    if (GetOpenFileNameW(&ofn)) {
        firmware_path_ = file_path;
        SetWindowTextW(firmware_edit_, file_path);
    }
}

void FlashToolDialog::OnFlash() {
    const auto lang = EffectiveUiLanguage(UiLanguage::kSystem);
    const bool zh = (lang == UiLanguage::kSimplifiedChinese);

    // 校验串口
    const int port_idx = static_cast<int>(
        SendMessageW(port_combo_, CB_GETCURSEL, 0, 0));
    if (port_idx < 0 || port_idx >= static_cast<int>(ports_.size())) {
        MessageBoxW(hwnd_,
            zh ? L"请先选择串口。" : L"Please select a serial port first.",
            zh ? L"提示" : L"Notice", MB_OK | MB_ICONWARNING);
        return;
    }

    // 校验固件文件
    if (firmware_path_.empty() || !std::filesystem::exists(firmware_path_)) {
        MessageBoxW(hwnd_,
            zh ? L"请先选择有效的固件文件。" : L"Please select a valid firmware file.",
            zh ? L"提示" : L"Notice", MB_OK | MB_ICONWARNING);
        return;
    }
    if (firmware_path_.extension() != L".bin") {
        MessageBoxW(hwnd_,
            zh ? L"固件文件必须是 .bin 格式。" : L"Firmware file must be .bin format.",
            zh ? L"提示" : L"Notice", MB_OK | MB_ICONWARNING);
        return;
    }

    // VoiceStickApp 运行检测（烧录会复位设备，断开 BLE 连接）
    if (IsVoiceStickRunning()) {
        if (MessageBoxW(hwnd_,
                zh ? L"VoiceStick 正在运行，烧录将强制复位设备并断开蓝牙连接。建议先退出 VoiceStick。是否继续？"
                   : L"VoiceStick is running. Flashing will reset the device and disconnect Bluetooth. Quit VoiceStick first. Continue?",
                zh ? L"警告" : L"Warning",
                MB_YESNO | MB_ICONQUESTION) == IDNO) {
            return;
        }
    }

    // 整包/擦除模式：提示用户烧录后需手动短按电源键重启
    if (mode_ == FlashMode::kFullMerged || mode_ == FlashMode::kEraseThenFull) {
        if (MessageBoxW(hwnd_,
                zh ? L"整包烧录会覆盖 bootloader 与分区表。烧录完成后请手动短按电源键重启设备。是否继续？"
                   : L"Full flash overwrites bootloader and partition table. After flashing, manually press power button to restart. Continue?",
                zh ? L"确认" : L"Confirm",
                MB_YESNO | MB_ICONQUESTION) == IDNO) {
            return;
        }
    }

    // 查找 esptool Python 运行时
    const auto python_exe = FindPythonExe();
    if (python_exe.empty() || !std::filesystem::exists(python_exe)) {
        MessageBoxW(hwnd_,
            zh ? L"找不到 esptool Python 运行时。请确保工具安装完整。"
               : L"Cannot find esptool Python runtime. Ensure the tool is fully installed.",
            zh ? L"错误" : L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    // 构造烧录选项
    FlashOptions opts;
    opts.serial_port = ports_[port_idx].device;
    opts.firmware_path = firmware_path_.wstring();
    opts.baud = baud_;
    opts.mode = mode_;

    // 创建烧录工具（runner_ 持有所有权，flash_tool_ 持有裸指针）
    const HWND hwnd = hwnd_;
    runner_ = std::make_unique<FlashProcessRunner>();
    flash_tool_ = std::make_unique<FlashTool>(
        opts, python_exe, runner_.get(),
        [hwnd](const FlashEvent& event) {
            auto* heap_event = new FlashEvent(event);
            if (!PostMessageW(hwnd, kWmFlashEvent, 0,
                              reinterpret_cast<LPARAM>(heap_event))) {
                delete heap_event;
            }
        });

    // 清空日志和进度条
    SetWindowTextW(log_edit_, L"");
    SendMessageW(progress_bar_, PBM_SETPOS, 0, 0);
    SetWindowTextW(status_label_, zh ? L"准备烧录…" : L"Preparing…");

    SetFlashing(true);

    // 启动工作线程（esptool 子进程在后台运行，事件通过 PostMessage 回 UI）
    flash_thread_ = CreateThread(nullptr, 0, FlashThreadProc, this, 0, nullptr);
    if (!flash_thread_) {
        MessageBoxW(hwnd_,
            zh ? L"无法启动烧录线程。" : L"Failed to start flash thread.",
            zh ? L"错误" : L"Error", MB_OK | MB_ICONERROR);
        ResetFlashState();
    }
}

DWORD WINAPI FlashToolDialog::FlashThreadProc(void* param) {
    auto* self = static_cast<FlashToolDialog*>(param);
    const HWND hwnd = self->hwnd_;

    // Run() 是同步阻塞调用：运行 esptool 子进程，逐行解析进度并回调。
    // 回调在工作线程执行，通过堆分配 + PostMessageW 把事件安全传递到 UI 线程。
    self->flash_tool_->Run();

    PostMessageW(hwnd, kWmFlashDone, 0, 0);
    return 0;
}

void FlashToolDialog::OnCancel() {
    if (flash_tool_) {
        flash_tool_->Cancel();
    }
}

void FlashToolDialog::OnFlashEvent(const FlashEvent& event) {
    const auto lang = EffectiveUiLanguage(UiLanguage::kSystem);
    const bool zh = (lang == UiLanguage::kSimplifiedChinese);

    switch (event.kind) {
    case FlashEvent::kStage:
        if (!event.text.empty()) {
            SetWindowTextW(status_label_, event.text.c_str());
            AppendLog(event.text);
        }
        break;
    case FlashEvent::kProgress:
        SendMessageW(progress_bar_, PBM_SETPOS, event.percent, 0);
        if (!event.text.empty()) {
            SetWindowTextW(status_label_, event.text.c_str());
        }
        break;
    case FlashEvent::kLogLine:
        if (!event.text.empty()) {
            AppendLog(event.text);
        }
        break;
    case FlashEvent::kError:
        AppendLog(event.text);
        SetWindowTextW(status_label_, zh ? L"烧录失败" : L"Flash failed");
        break;
    case FlashEvent::kFinished:
        if (event.success) {
            SendMessageW(progress_bar_, PBM_SETPOS, 100, 0);
            SetWindowTextW(status_label_, zh ? L"烧录完成" : L"Flash complete");
            AppendLog(zh ? L"=== 烧录成功 ===" : L"=== Flash succeeded ===");
            MessageBoxW(hwnd_,
                zh ? L"固件烧录完成。请手动短按设备电源键重启。重启后设备会自动恢复广播。"
                   : L"Firmware flashed. Please manually press the device power button to restart.",
                zh ? L"完成" : L"Complete", MB_OK | MB_ICONINFORMATION);
        } else {
            SetWindowTextW(status_label_, zh ? L"烧录失败" : L"Flash failed");
            AppendLog(zh ? L"=== 烧录失败 ===" : L"=== Flash failed ===");
            MessageBoxW(hwnd_,
                zh ? L"烧录失败，请检查日志和设备连接。" : L"Flash failed. Check log and device connection.",
                zh ? L"错误" : L"Error", MB_OK | MB_ICONERROR);
        }
        break;
    }
}

void FlashToolDialog::AppendLog(const std::wstring& line) {
    if (!log_edit_) return;
    const std::wstring text = line + L"\r\n";
    int len = GetWindowTextLengthW(log_edit_);
    // 截断过长日志（保留最近约 20000 字符，避免日志框无限增长）
    if (len > 20000) {
        std::wstring current(static_cast<std::size_t>(len) + 1, L'\0');
        GetWindowTextW(log_edit_, current.data(), len + 1);
        current.resize(static_cast<std::size_t>(len));
        const std::size_t cut = current.find(L'\n', 10000);
        if (cut != std::wstring::npos) {
            current = current.substr(cut + 1);
        }
        SetWindowTextW(log_edit_, current.c_str());
        len = GetWindowTextLengthW(log_edit_);
    }
    SendMessageW(log_edit_, EM_SETSEL, len, len);
    SendMessageW(log_edit_, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text.c_str()));
}

void FlashToolDialog::SetFlashing(bool flashing) {
    flashing_ = flashing;
    EnableWindow(port_combo_, !flashing);
    EnableWindow(refresh_button_, !flashing);
    EnableWindow(firmware_edit_, !flashing);
    EnableWindow(browse_button_, !flashing);
    EnableWindow(mode_combo_, !flashing);
    EnableWindow(baud_combo_, !flashing);
    EnableWindow(flash_button_, !flashing);
    EnableWindow(cancel_button_, flashing);
    EnableWindow(close_button_, !flashing);
}

void FlashToolDialog::ResetFlashState() {
    if (flash_thread_) {
        CloseHandle(flash_thread_);
        flash_thread_ = nullptr;
    }
    flash_tool_.reset();
    runner_.reset();
    SetFlashing(false);
}

int FlashToolDialog::Dp(int px) const {
    return ScalePx(px, dpi_);
}

LPCDLGTEMPLATE FlashToolDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    // WS_MINIMIZEBOX + WS_EX_APPWINDOW 使窗口在任务栏显示且可最小化；
    // WS_CLIPCHILDREN 避免父窗口重绘擦除子控件背景。
    // 模板不含控件项（cdit=0），所有控件在 BuildControls 中手动创建。
    DLGTEMPLATE tmpl{};
    tmpl.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                 DS_SETFONT | WS_CLIPCHILDREN;
    tmpl.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                           WS_EX_APPWINDOW;
    tmpl.cdit = 0;
    tmpl.x = 0;
    tmpl.y = 0;
    tmpl.cx = 300;
    tmpl.cy = 200;

    AppendDialogData(&dialog_template_, &tmpl, sizeof(tmpl));
    AppendDialogWord(&dialog_template_, 0);  // menu
    AppendDialogWord(&dialog_template_, 0);  // class
    AppendDialogWideString(&dialog_template_, L"VoiceStickFlash");
    AppendDialogWord(&dialog_template_, 9);  // font size
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

} // namespace voicestick
