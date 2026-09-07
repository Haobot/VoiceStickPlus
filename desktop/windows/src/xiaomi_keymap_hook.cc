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
    // LL 钩子在调用线程（主线程，有消息泵）安装；回调与安装同线程上下文，
    // interceptor_ 与 signal 读取无并发（signal 写来自 Raw Input 线程，atomic）。
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        active_instance_ = nullptr;
        LogApp("XiaomiKeymapHook: SetWindowsHookEx WH_KEYBOARD_LL failed err=" +
               std::to_string(GetLastError()));
        return;
    }
    for (auto& slot : signal_ms_) slot.store(0, std::memory_order_relaxed);
    interceptor_.Reset();
    raw_input_thread_ = std::thread([this] { RawInputThreadMain(); });
    LogApp("XiaomiKeymapHook: started (LL hook + raw input correlator)");
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

void XiaomiKeymapHook::RecordSignal(std::string_view button) {
    const int idx = ButtonIndex(button);
    if (idx < 0) return;
    signal_ms_[idx].store(NowSteadyMs(), std::memory_order_relaxed);
}

std::int64_t XiaomiKeymapHook::LoadSignalMs(std::string_view button) {
    const int idx = ButtonIndex(button);
    if (idx < 0) return -1;
    return signal_ms_[idx].load(std::memory_order_relaxed);
}

// 竞态收口：WM_INPUT 与 LL 钩子的相对时序未定义（同一 HID 报告的两条分发路径
// 线程调度竞态），首次 keydown 在等待窗内轮询佐证（对齐 VoiceF5Suppressor 的
// 关联等待模式；窗 15/60ms 远低于 LowLevelHooksTimeout）。
bool XiaomiKeymapHook::WaitForSignal(std::string_view button,
                                     std::int64_t now_ms,
                                     std::int64_t window_ms) {
    const std::int64_t deadline = now_ms + window_ms;
    while (NowSteadyMs() < deadline) {
        Sleep(2);
        const std::int64_t signal = LoadSignalMs(button);
        if (signal >= now_ms - window_ms) return true;  // 等待期间新佐证到达
    }
    return false;
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

LRESULT CALLBACK XiaomiKeymapHook::LowLevelKeyboardProc(int code,
                                                        WPARAM w_param,
                                                        LPARAM l_param) {
    auto* self = active_instance_;
    if (code != HC_ACTION || !self || !self->hook_) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
    // 自家与其他工具注入的合成键不干预（注入的映射键自带 LLKHF_INJECTED，
    // 防递归；对齐 VoiceF5Suppressor）。
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
    const std::int64_t now = NowSteadyMs();
    // 快路径：佐证已先行到达（典型时序），零等待决策；未命中且需要关联
    //（首次 keydown 无佐证）时限时等待兜底——等待失败确认是物理键盘同特征
    // 键，补记放行闩锁，重复流不再等待（防阻塞键盘管线拖慢打字）。
    XiaomiKeymapDecision decision = self->interceptor_.OnHookEvent(
        *button, is_down, now, self->LoadSignalMs(*button), *key_map);
    LogApp("XiaomiKeymapHook: candidate=" + std::string(*button) +
           (is_down ? " down" : " up") + " vk=" +
           std::to_string(static_cast<int>(info->vkCode)) + " scan=" +
           std::to_string(static_cast<int>(info->scanCode)) +
           " signal=" + std::to_string(self->LoadSignalMs(*button)) +
           " now=" + std::to_string(now) + " swallow=" +
           (decision.swallow ? "1" : "0") + " wait=" +
           (decision.needs_correlation ? "1" : "0"));
    if (!decision.swallow && decision.needs_correlation && is_down) {
        if (self->WaitForSignal(*button, now,
                                XiaomiKeymapCorrelateWindowMs(*button))) {
            decision = self->interceptor_.OnHookEvent(
                *button, is_down, NowSteadyMs(), self->LoadSignalMs(*button),
                *key_map);
            LogApp("XiaomiKeymapHook: correlated candidate=" + std::string(*button) +
                   " swallow=" + (decision.swallow ? "1" : "0"));
        } else {
            self->interceptor_.RecordPass(*button, NowSteadyMs());
            LogApp("XiaomiKeymapHook: pass-latched candidate=" + std::string(*button));
        }
    }
    if (!decision.swallow) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    self->InjectVks(decision.inject, is_down);
    return 1;
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
            if (!is_xiaomi_device(raw->header.hDevice)) continue;
            const RAWKEYBOARD& keyboard = raw->data.keyboard;
            // 仅记按下沿（佐证用于 keydown 判定；keyup 走闩锁关联）。
            if (keyboard.Flags & RI_KEY_BREAK) continue;
            const auto button = XiaomiButtonFromVkScan(keyboard.VKey,
                                                       keyboard.MakeCode);
            LogApp("XiaomiKeymapHook: raw vk=" +
                   std::to_string(static_cast<int>(keyboard.VKey)) + " make=" +
                   std::to_string(static_cast<int>(keyboard.MakeCode)) +
                   " button=" +
                   (button.has_value() ? std::string(*button)
                                       : std::string("-")));
            if (button.has_value()) RecordSignal(*button);
        }
    }
    if (raw_input_hwnd_) {
        DestroyWindow(raw_input_hwnd_);
        raw_input_hwnd_ = nullptr;
    }
    LogApp("XiaomiKeymapHook: raw input correlator exited");
}

} // namespace voicestick
