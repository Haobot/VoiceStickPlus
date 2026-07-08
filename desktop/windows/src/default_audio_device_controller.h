// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 默认录音设备切换器接口。
// 用于 wechat_input_method 模式 auto_switch_default_recording_device：录音期把默认
// 录音设备（eConsole 角色）切到虚拟麦克风（CABLE Output），松开切回原设备。
// 角色分离：只切 eConsole，eCommunications 保持真实麦不动，使 Teams/Skype 等
// 通信类会议软件零干扰。
// 读取/枚举用 IMMDeviceEnumerator，设置用 IPolicyConfig（真实 COM 实现见
// DefaultAudioDeviceController；测试注入 Fake 解耦真实 COM 调用）。

#ifndef VOICESTICK_DEFAULT_AUDIO_DEVICE_CONTROLLER_H_
#define VOICESTICK_DEFAULT_AUDIO_DEVICE_CONTROLLER_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

// 音频设备角色。对应 Windows ERole：Console=默认设备，Communications=默认通信设备。
enum class DeviceRole {
    kConsole,
    kCommunications,
    kMultimedia,
};

struct AudioDeviceInfo {
    std::wstring id;             // endpoint id（IMMDevice::GetId）
    std::wstring friendly_name;  // PKEY_Device_FriendlyName
};

class IDefaultAudioDeviceController {
 public:
    virtual ~IDefaultAudioDeviceController() = default;
    // 读取当前 eCapture 默认设备（指定角色）。
    virtual std::optional<AudioDeviceInfo> GetDefaultCapture(DeviceRole role) = 0;
    // 枚举 eCapture ACTIVE 设备，friendly name 子串匹配（大小写不敏感）。
    virtual std::optional<AudioDeviceInfo> FindCaptureByName(
        std::wstring_view name_substring) = 0;
    // 把指定设备设为默认录音设备的指定角色集合。返回是否成功。
    virtual bool SetDefaultCapture(const std::wstring& device_id,
                                  std::vector<DeviceRole> roles) = 0;
};

}  // namespace voicestick

#endif  // VOICESTICK_DEFAULT_AUDIO_DEVICE_CONTROLLER_H_
