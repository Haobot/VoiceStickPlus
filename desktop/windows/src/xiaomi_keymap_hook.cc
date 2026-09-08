#include "xiaomi_keymap_hook.h"

#include "log.h"
#include "xiaomi_buttons.h"

#include <chrono>
#include <cstdlib>
#include <utility>

namespace voicestick {

XiaomiKeymapHook* XiaomiKeymapHook::active_instance_ = nullptr;

namespace {

constexpr UINT kRimTypeKeyboard = 1;
constexpr UINT kRidiDeviceName = 0x20000007;
constexpr UINT kRidevInputSink = 0x00000100;

std::int64_t NowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 宽字符转 UTF-8（日志用，对齐各 dialog 的同名局部实现）。
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

int ButtonIndex(std::string_view button) {
    for (size_t i = 0; i < kXiaomiMappableButtons.size(); ++i) {
        if (kXiaomiMappableButtons[i] == button) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

XiaomiKeymapHook::~XiaomiKeymapHook() { Stop(); }

void XiaomiKeymapHook::Start(std::map<std::string, std::string> key_map) {
    UpdateKeymap(std::move(key_map));
    if (hook_) return;  // 幂等：已运行仅刷新 key_map
    active_instance_ = this;
    // LL 钩子与异步判定窗口都在主线程（须有消息泵）；interceptor_ 仅主线程
    // 触达（LL 回调经主线程消息机制执行），无并发。
    WNDCLASSW wc{};
    wc.lpfnWndProc = DispatchWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VoiceStickXiaomiKeymapDispatch";
    RegisterClassW(&wc);
    dispatch_hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, wc.hInstance,
                                     nullptr);
    if (!dispatch_hwnd_) {
        LogApp("XiaomiKeymapHook: dispatch window create failed err=" +
               std::to_string(GetLastError()));
        active_instance_ = nullptr;
        return;
    }
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        LogApp("XiaomiKeymapHook: SetWindowsHookEx WH_KEYBOARD_LL failed err=" +
               std::to_string(GetLastError()));
        DestroyWindow(dispatch_hwnd_);
        dispatch_hwnd_ = nullptr;
        active_instance_ = nullptr;
        return;
    }
    interceptor_.Reset();
    pending_timer_on_ = false;
    raw_input_thread_ = std::thread([this] { RawInputThreadMain(); });
    LogApp("XiaomiKeymapHook: started (LL hook + post-keyup correlation)");
}

void XiaomiKeymapHook::UpdateKeymap(
    std::map<std::string, std::string> key_map) {
    key_map_.store(
        std::make_shared<const std::map<std::string, std::string>>(
            std::move(key_map)),
        std::memory_order_release);
}

void XiaomiKeymapHook::Stop() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (dispatch_hwnd_) {
        if (pending_timer_on_) {
            KillTimer(dispatch_hwnd_, kPendingTimerId);
            pending_timer_on_ = false;
        }
        DestroyWindow(dispatch_hwnd_);
        dispatch_hwnd_ = nullptr;
    }
    if (active_instance_ == this) active_instance_ = nullptr;
    if (raw_input_thread_.joinable()) {
        // 有限重试：线程函数开头才登记 id，Start 后立刻 Stop 时可能尚未就绪。
        for (int i = 0; i < 200; ++i) {
            if (raw_input_thread_id_ != 0 &&
                PostThreadMessageW(raw_input_thread_id_, WM_QUIT, 0, 0)) {
                break;
            }
            Sleep(1);
        }
        raw_input_thread_.join();
    }
    interceptor_.Reset();
}

void XiaomiKeymapHook::InjectVks(const std::vector<UINT>& vks, bool down) {
    if (vks.empty()) return;
    std::vector<INPUT> inputs(vks.size());
    for (size_t i = 0; i < vks.size(); ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = static_cast<WORD>(vks[i]);
        inputs[i].ki.wScan =
            static_cast<WORD>(MapVirtualKeyW(vks[i], MAPVK_VK_TO_VSC));
        inputs[i].ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        inputs[i].ki.dwExtraInfo = kInjectExtraInfo;
    }
    if (SendInput(static_cast<UINT>(inputs.size()), inputs.data(),
                  sizeof(INPUT)) != inputs.size()) {
        LogApp("XiaomiKeymapHook: SendInput incomplete err=" +
               std::to_string(GetLastError()));
    }
}

// 统一执行归属判定动作：主注入（down 方向）+ 紧随其后的补对 up 序。
void XiaomiKeymapHook::ApplyAction(const XiaomiKeymapHookAction& action,
                                   const char* tag, std::string_view button) {
    LogApp(std::string("XiaomiKeymapHook: ") + tag + " button=" +
           std::string(button) + " inject=" +
           std::to_string(action.inject.size()) + "+" +
           std::to_string(action.inject_up.size()));
    InjectVks(action.inject, true);
    if (!action.inject_up.empty()) InjectVks(action.inject_up, false);
}

LRESULT CALLBACK XiaomiKeymapHook::LowLevelKeyboardProc(int code,
                                                        WPARAM w_param,
                                                        LPARAM l_param) {
    auto* self = active_instance_;
    if (code != HC_ACTION || !self || !self->hook_) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
    // 自家与其他工具注入的合成键不干预（注入的映射/补偿键自带
    // LLKHF_INJECTED，防递归；对齐 VoiceF5Suppressor）。
    if ((info->flags & LLKHF_INJECTED) != 0 ||
        info->dwExtraInfo == kInjectExtraInfo) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const bool is_down = (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN);
    const bool is_up = (w_param == WM_KEYUP || w_param == WM_SYSKEYUP);
    if (!is_down && !is_up) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const auto button = XiaomiButtonFromVkScan(info->vkCode, info->scanCode);
    if (!button.has_value()) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const auto key_map =
        self->key_map_.load(std::memory_order_acquire);
    if (!key_map || key_map->empty()) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    // keyup 后置决策（2026-09-07 三次迭代定案）：keydown/按住重复一律吞（零
    // 副作用零等待）；keyup 放行让 BREAK 沿投递提供设备证据，归属判定与注入
    // 移到主线程消息完成（OnBreakMessage/OnPendingTimer）。
    XiaomiKeymapHookAction action =
        is_down ? self->interceptor_.OnKeyDown(*button, info->vkCode,
                                                info->scanCode, NowSteadyMs(),
                                                *key_map)
                : self->interceptor_.OnKeyUp(*button, NowSteadyMs(), *key_map);
    if (!action.swallow) {
        if (is_down && self->interceptor_.HasPending() &&
            self->dispatch_hwnd_ && !self->pending_timer_on_) {
            // 兜底定时器：BREAK 异常丢失时按物理键盘补偿。
            self->pending_timer_on_ = SetTimer(self->dispatch_hwnd_,
                                               kPendingTimerId,
                                               kPendingTimerMs,
                                               nullptr) != 0;
        }
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    self->ApplyAction(action, is_down ? "swallow-down" : "swallow-up",
                      *button);
    if (self->interceptor_.HasPending() && self->dispatch_hwnd_ &&
        !self->pending_timer_on_) {
        self->pending_timer_on_ = SetTimer(self->dispatch_hwnd_,
                                           kPendingTimerId, kPendingTimerMs,
                                           nullptr) != 0;
    }
    return 1;
}

LRESULT CALLBACK XiaomiKeymapHook::DispatchWndProc(HWND hwnd, UINT msg,
                                                   WPARAM w_param,
                                                   LPARAM l_param) {
    auto* self = active_instance_;
    if (self) {
        if (msg == kMsgBreakEvidence) {
            self->OnBreakMessage(static_cast<int>(w_param), l_param != 0);
            return 0;
        }
        if (msg == WM_TIMER && w_param == kPendingTimerId) {
            self->OnPendingTimer();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

void XiaomiKeymapHook::OnBreakMessage(int button_index, bool from_remote) {
    if (button_index < 0 ||
        button_index >= static_cast<int>(kXiaomiMappableButtons.size())) {
        return;
    }
    const auto key_map = key_map_.load(std::memory_order_acquire);
    if (!key_map) return;
    const auto button = kXiaomiMappableButtons[button_index];
    auto action = interceptor_.OnBreakEvidence(button, NowSteadyMs(),
                                                from_remote, *key_map);
    if (!action.has_value()) return;  // 无待判定 pending：残留/按住中/已兜底
    ApplyAction(*action, from_remote ? "break-remote-inject"
                                     : "break-physical-compensate",
                button);
    if (!interceptor_.HasPending() && pending_timer_on_) {
        KillTimer(dispatch_hwnd_, kPendingTimerId);
        pending_timer_on_ = false;
    }
}

void XiaomiKeymapHook::OnPendingTimer() {
    const auto key_map = key_map_.load(std::memory_order_acquire);
    if (!key_map) return;
    const std::int64_t now = NowSteadyMs();
    for (const auto& [button, released] : interceptor_.PendingAwaitingBreak()) {
        auto action = interceptor_.OnPendingTimeout(button, now);
        if (action.has_value()) {
            ApplyAction(*action, "break-timeout-compensate", button);
        }
    }
    if (!interceptor_.HasPending() && pending_timer_on_) {
        KillTimer(dispatch_hwnd_, kPendingTimerId);
        pending_timer_on_ = false;
    }
}

void XiaomiKeymapHook::RawInputThreadMain() {
    raw_input_thread_id_ = GetCurrentThreadId();
    // 独立隐藏窗口接收 WM_INPUT（RIDEV_INPUTSINK 后台接收，无需前台）。
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VoiceStickXiaomiKeymapRawInput";
    RegisterClassW(&wc);
    raw_input_hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, wc.hInstance,
                                      nullptr);
    if (!raw_input_hwnd_) {
        LogApp("XiaomiKeymapHook: raw input window create failed err=" +
               std::to_string(GetLastError()));
        return;
    }
    // 键盘页(0x01/0x06) + 消费页(0x0C/0x01)：遥控器方向/OK/Enter 走键盘页，
    // back/home/音量等走消费页翻译，两集合都注册才能覆盖全部特征键。
    RAWINPUTDEVICE devices[] = {
        {0x01, 0x06, kRidevInputSink, raw_input_hwnd_},
        {0x0C, 0x01, kRidevInputSink, raw_input_hwnd_},
    };
    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        LogApp("XiaomiKeymapHook: RegisterRawInputDevices failed err=" +
               std::to_string(GetLastError()));
        return;
    }
    LogApp("XiaomiKeymapHook: raw input correlator running");

    // hDevice → 是否小米遥控器 的判定缓存（仅本线程访问）。BTHLE 遥控器在
    // Raw Input 中是 RIM_TYPEKEYBOARD，RIDI_DEVICEINFO 的 hid 联合体成员不填
    //（dwVendorId 恒 0，2026-09-07 真机排查定案），必须取 RIDI_DEVICENAME
    // 接口路径解析 VID/PID；未知设备记一次日志，避免再出现静默盲区。
    std::map<HANDLE, bool> xiaomi_device_cache;
    const auto is_xiaomi_device = [&xiaomi_device_cache](HANDLE device) {
        if (!device) return false;
        const auto cached = xiaomi_device_cache.find(device);
        if (cached != xiaomi_device_cache.end()) return cached->second;
        bool matched = false;
        std::wstring name;
        UINT size = 0;
        if (GetRawInputDeviceInfoW(device, kRidiDeviceName, nullptr, &size) !=
            UINT(-1)) {
            std::wstring buf(size + 1, L'\0');
            if (GetRawInputDeviceInfoW(device, kRidiDeviceName, buf.data(),
                                       &size) != UINT(-1)) {
                name = buf.substr(0, size);
                matched = XiaomiRawInputNameIsRemote(name);
            }
        }
        if (!matched) {
            LogApp("XiaomiKeymapHook: raw input device not remote: " +
                   Utf8(name));
        }
        xiaomi_device_cache[device] = matched;
        return matched;
    };

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_INPUT) {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT,
                            nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size == 0 || size > 512) continue;
            std::vector<BYTE> buffer(size);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam),
                                RID_INPUT, buffer.data(), &size,
                                sizeof(RAWINPUTHEADER)) == UINT(-1)) {
                continue;
            }
            const auto* raw =
                reinterpret_cast<const RAWINPUT*>(buffer.data());
            if (raw->header.dwType != kRimTypeKeyboard) continue;
            // 松开沿 = 设备归属证据（按下沿被吞的键不投递，松开沿随放行的
            // keyup 正常到达，带 hDevice）。转主线程完成归属判定注入。
            if ((raw->data.keyboard.Flags & RI_KEY_BREAK) == 0) continue;
            const auto button = XiaomiButtonFromVkScan(raw->data.keyboard.VKey,
                                                       raw->data.keyboard.MakeCode);
            if (!button.has_value()) continue;
            const int idx = ButtonIndex(*button);
            if (idx < 0) continue;
            const bool from_remote = is_xiaomi_device(raw->header.hDevice);
            if (dispatch_hwnd_) {
                PostMessageW(dispatch_hwnd_, kMsgBreakEvidence,
                             static_cast<WPARAM>(idx),
                             static_cast<LPARAM>(from_remote ? 1 : 0));
            }
        }
    }
    if (raw_input_hwnd_) {
        DestroyWindow(raw_input_hwnd_);
        raw_input_hwnd_ = nullptr;
    }
    LogApp("XiaomiKeymapHook: raw input correlator exited");
}

} // namespace voicestick
