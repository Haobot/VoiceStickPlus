// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 虚拟麦克风改名：把系统录音端虚拟麦克风（VB-CABLE 的 "CABLE Output" 等）的设备
// 友好名改为 VoiceStick 设备名（如 "VS-091C"），让应用（Clash/浏览器等）的麦克风
// 列表直接显示设备名，用户无需理解 "CABLE Output" 是什么。
//
// 权限模型：写设备名需管理员权限（COM/注册表在 HKLM），主进程 asInvoker 不具备。
// 流程：主进程 runas 提权启动自身带 --rename-virtual-mic 参数的子进程执行改名，
// 等待退出码（见 main.cc 的参数分支与 LaunchRenameVirtualMicElevated）。
//
// 两条写入路径（子进程内，先 COM 后注册表回退）：
//  1) IPolicyConfig::SetPropertyValue（未公开 COM 接口，与
//     default_audio_device_controller.cc 同源移植自 SoundSwitch）：经音频服务写属性，
//     理论上即时生效并广播设备属性变更通知。
//  2) 回退：直接写 HKLM MMDevices 注册表的 PKEY_Device_FriendlyName 值 + 重启
//     音频服务（Audiosrv/AudioEndpointBuilder）刷新 endpoint 属性缓存。
//
// 副作用与约定：
//  - 名字是录音端设备（endpoint）级显示名，系统全局生效；VB-CABLE 重装后恢复默认名。
//  - 改名后配置 [wechat_input_method].virtual_mic_capture_name 需同步为新名
//    （auto_switch 按名字找设备），由调用方（settings_dialog）负责。

#ifndef VOICESTICK_VIRTUAL_MIC_RENAMER_H_
#define VOICESTICK_VIRTUAL_MIC_RENAMER_H_

#include "app_config.h"

#include <Windows.h>

#include <string>

namespace voicestick {

// 注意：不要在此前向声明 `class AppConfig;`——MSVC 按各翻译单元首次看到的
// class/struct 关键字修饰符号名（AEBV vs AEBU），与 app_config.h 的 `struct
// AppConfig` 不一致会造成 LNK2019。统一直接包含定义头。

// PKEY_Device_FriendlyName 在 MMDevices 注册表 Properties 键下的值名
// （"{fmtid},pid" 格式；fmtid={a45c254e-df1c-4efd-8020-67d146a850e0}, pid=14）。
// 公开给测试回读校验用。
inline constexpr wchar_t kEndpointFriendlyNameValueName[] =
    L"{a45c254e-df1c-4efd-8020-67d146a850e0},14";

// 选虚拟麦克风显示名：第一台 StickS3（非小米遥控器）配对设备的 name（空则
// "VS-<id>"）；无 StickS3 配对设备回退 "VoiceStick"。纯函数，可单测。
std::wstring PickVirtualMicDisplayName(const AppConfig& config);

// IMMDevice endpoint id（"{0.0.1.00000000}.{guid}"）→ MMDevices 注册表键名
// （"{guid}"）。格式非法（找不到 "}/{guid}" 结构）返回空串。纯函数，可单测。
std::wstring EndpointRegistryKeyName(const std::wstring& endpoint_id);

// 写 <root>\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\
// {guid}\Properties 下的 PKEY_Device_FriendlyName 值（REG_SZ）。
// root 参数化：生产传 HKEY_LOCAL_MACHINE，单测传 HKEY_CURRENT_USER。
// endpoint_id 非法或写入失败返回 false。
bool WriteEndpointFriendlyNameToRegistry(HKEY root, const std::wstring& endpoint_id,
                                         const std::wstring& new_name);

// 提权子进程主逻辑：先 IPolicyConfig::SetPropertyValue，失败回退注册表写入 +
// 重启音频服务。结果写应用日志。返回进程退出码（0 成功）。
int RunRenameVirtualMicElevated(const std::wstring& endpoint_id,
                                const std::wstring& new_name);

// 主进程：ShellExecuteEx "runas" 提权启动自身（--rename-virtual-mic 参数）执行
// 改名并等待完成（UAC 弹窗由系统处理）。失败（UAC 被拒/超时/退出码非 0）返回
// false 并在 error 给出原因描述。
bool LaunchRenameVirtualMicElevated(const std::wstring& exe_path,
                                    const std::wstring& endpoint_id,
                                    const std::wstring& new_name,
                                    std::string* error);

}  // namespace voicestick

#endif  // VOICESTICK_VIRTUAL_MIC_RENAMER_H_
