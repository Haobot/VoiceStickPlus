# 小米遥控器按键映射消费端设计(key_map 拦截与注入)

- 状态:已评审,实施中
- 日期:2026-09(修复 8e051f2b 遗留的消费端缺口)
- 背景:8e051f2b 交付了按键映射的配置 UI 与 TOML 持久化,但消费端(拦截遥控器按键 → 查 key_map → 注入映射键)未实现,`win32_app.cc` 菜单注释声称「key_map 由 XiaomiAtvvSession 按键分发消费」与事实不符。表现为:配置 `back = "backspace"` 后按返回键无任何效果。
- 协议事实来源:`C:\Dev\FFE\George\MiVibe-Remote`(GPL-3.0,仅作协议/翻译事实参考;本项目 Apache-2.0,按事实重新实现,不复制代码)

## 1. 问题本质

小米遥控器 2 Pro 除语音键外的按键走标准 HID over GATT(0x1812),由 Windows HID 栈原生翻译为键盘/消费键:

| 按钮 | kbdhid 翻译特征(真机事实,MiVibe 验证) |
|---|---|
| back | `VK_BROWSER_BACK`(0xA6),焦点应用收到浏览器后退副作用 |
| home | `VK_BROWSER_HOME`(0xAC)或 `VK_HOME`(0x24) |
| ok | `VK_RETURN` |
| up/down/left/right | `VK_UP`/`VK_DOWN`/`VK_LEFT`/`VK_RIGHT` |
| menu | `VK_APPS` |
| tv | `VK_OEM_3`(0xC0)+ 扫描码 0x29(与键盘 Grave 同特征) |
| power | `VK_SLEEP`(0x5F)或 `VK` 0xFF(未知)或扫描码 0x5E |
| volume_up/down | `VK_VOLUME_UP`/`VK_VOLUME_DOWN` |

拦截的三个难题与对策:

1. **LL 钩子(WH_KEYBOARD_LL)拿不到按键来源设备**,无法区分「遥控器的 Home」与「物理键盘的 Home」。
   对策:**Raw Input(RIDEV_INPUTSINK)佐证**——注册 `(0x01,0x06)` 键盘页 + `(0x0C,0x01)` 消费页,`WM_INPUT` 的 `RAWKEYBOARD` 带 `hDevice`,经 `GetRawInputDeviceInfo(RIDI_DEVICEINFO)` 读 VID/PID(小米 2 Pro:`0x2717`/`0x32B8`)精确归属。佐证信号由独立 Raw Input 线程记录「按钮 → 最近佐证时刻」,LL 钩子里在等待窗内查窗。这是对 MiVibe「WUDF/Frida 直读信号」的**零注入替代**(本项目红线:不引入 Frida)。
2. **WM_INPUT 与 LL 钩子的相对时序未定义**:同一物理输入的 raw 分发与系统队列翻译可能乱序。
   对策:候选键首次 keydown 在钩子内限时等待(tv/home/menu/power 60ms,其余 15ms,对齐 MiVibe 真机参数);超时放行(物理键盘同名键不受影响)。
3. **按键归属确认后须吞掉原始键**(否则 back 的浏览器后退/方向键的焦点移动等原生副作用泄漏)。
   对策:确认佐证后 LL 钩子返回 1 吞原始键,同时 `SendInput` 注入映射键(`LLKHF_INJECTED` 自带,自家钩子放行注入键,另带 `dwExtraInfo` 标记双保险);按住序列闩锁——吞过 down 后该键的自动重复直接吞、keyup 关联吞(镜像 `VoiceF5Suppressor` 闩锁模式,松开阶段佐证窗可能已过期)。

## 2. 架构

```
小米遥控器 HID ──┬─► Windows HID 栈 → LL 钩子候选(VK/SC 特征表)─┐
                 │                                                  ▼
                 └─► Raw Input 线程(hDevice VID/PID 佐证)──► XiaomiKeymapInterceptor(core 纯逻辑)
                                                                    │ 决策:吞 + 注入序列
                                                                    ▼
                                                        SendInput(映射 KeySpec,带 extra_info)
```

- **core 纯逻辑**(`src/xiaomi_keymap_interceptor.h/.cc`,进 `voicestick_core`,CTest 单测):
  - `XiaomiButtonFromVkScan(vk, scan)`:kbdhid 翻译特征表 → 候选按钮 ID(12 键,同 `xiaomi_buttons.h`;识别≠归属,归属靠佐证)
  - `XiaomiKeymapInterceptor::OnHookEvent(button, is_down, now_ms, signal_ms, key_map)`:决策状态机。key_map 无条目/空串(显式取消)/非法 spec → 放行;keyup 按闩锁关联;自动重复免再佐证
  - `XiaomiKeymapInjectDownVks/InjectUpVks(KeySpec)`:注入 VK 序列(down 修饰键序+主键,up 反序)
- **Win32 层**(`src/xiaomi_keymap_hook.h/.cc`,进 `VoiceStickApp`):
  - `XiaomiKeymapHook`(进程单例,对齐 `VoiceF5Suppressor`):Raw Input 线程(独立消息泵)+ LL 钩子(主线程安装)+ `SendInput` 注入;`Start(key_map)` / `UpdateKeymap(key_map)` / `Stop()`
- **接线**(`win32_app.cc`):`SyncXiaomiKeymapHook()` 门控 =「有配对/连接 RC 设备 且 有效 key_map 非空」;刷新时机对齐 `SyncF5Suppressor`(启动/配对完成/配置热更/连接集变化)。有效 key_map 取活跃 RC 设备覆盖,回落全局默认(同型号多台遥控器 Raw Input 无法区分,key_map 取活跃设备,与 F5 抑制同粒度)。
- 语音键(mic)不参与映射(沿用 UI 既有排除);F5 抑制逻辑不动。

## 3. 已知边界

| 场景 | 行为 |
|---|---|
| 物理键盘按下与遥控器候选同 VK(Home/方向/`) | 无 Raw Input 佐证 → 放行,不误吞 |
| 佐证窗内恰好同名键并发(键盘+遥控器同键 15/60ms 内) | 极小概率误吞一次,松开即恢复 |
| 佐证信号乱序迟到(蓝牙抖动) | 首次 keydown 等待窗内命中即吞;超时放行(原始键泄漏一次原生行为) |
| 注入键触发全局热键/其他 LL 钩子 | 注入自带 `LLKHF_INJECTED`,自家两钩子均放行;第三方行为属用户配置责任 |
| 映射键含修饰键时的系统状态泄漏 | 每次注入 down/up 成对完整序列,不持有修饰键状态 |

## 4. 测试

- 单测(`core_tests.cc` `TestXiaomiKeymapInterceptor`):特征识别表全量、无映射/空串/非法串放行、佐证窗内吞+注入序列、佐证缺失/过期放行、闩锁自动重复与 keyup 关联、60/15ms 双窗边界、注入 down/up 序与反序
- 真机验收:配置 `back = "backspace"` → 记事本按返回键删除字符;方向键未配置映射时原生行为不变;物理键盘 Home/方向键不受遥控器映射影响;长按返回键连续删除
