# 小米遥控器按键映射消费端设计(key_map 拦截与注入)

- 状态:已评审,实施中
- 日期:2026-09(修复 8e051f2b 遗留的消费端缺口)
- 背景:8e051f2b 交付了按键映射的配置 UI 与 TOML 持久化,但消费端(拦截遥控器按键 → 查 key_map → 注入映射键)未实现,`win32_app.cc` 菜单注释声称「key_map 由 XiaomiAtvvSession 按键分发消费」与事实不符。表现为:配置 `back = "backspace"` 后按返回键无任何效果。
- 协议事实来源:`C:\Dev\FFE\George\MiVibe-Remote`(GPL-3.0,仅作协议/翻译事实参考;本项目 Apache-2.0,按事实重新实现,不复制代码)

## 1. 问题本质

小米遥控器 2 Pro 除语音键外的按键走标准 HID over GATT(0x1812),由 Windows HID 栈原生翻译为键盘/消费键:

| 按钮 | kbdhid 翻译特征(真机事实) |
|---|---|
| back | **RC003(RC-6459,2026-09-07 三轮探针 + MiVibe 研读定案):固件**有**上报 usage 0xF1(键盘页非标准),但被微软 HidOverGatt WUDF 宿主在翻译层内部丢弃——系统键盘层(LL/Raw Input/焦点应用)全静默,不可经系统输入链路映射**;RC001(MiVibe 记录)= `VK_BROWSER_BACK`(0xA6),特征保留 |
| home | `VK_BROWSER_HOME`(0xAC)或 `VK_HOME`(0x24) |
| ok | `VK_RETURN`(RC-6459 实测:scan 0x1C) |
| up/down/left/right | `VK_UP`/`VK_DOWN`/`VK_LEFT`/`VK_RIGHT`(RC-6459 实测:扩展键 E0,scan 0x48 等) |
| menu | `VK_APPS` |
| tv | `VK_OEM_3`(0xC0)+ 扫描码 0x29(与键盘 Grave 同特征) |
| power | `VK_SLEEP`(0x5F)或 `VK` 0xFF(未知)或扫描码 0x5E |
| volume_up/down | **RC003(RC-6459,2026-09-07 LL 层三轮探针 + 报告描述符枚举定案):Windows 端零键盘事件——描述符里音量键在厂商页 0xFF00 报告(Report 6/7/8)中,Windows HID 栈不翻译厂商页,不可经系统输入链路映射**;同遥控器连 Mac 音量键正常(固件按配对主机下发不同 Report Map/或走 AVRCP,推断);RC001(MiVibe 记录)= `VK_VOLUME_UP`/`VK_VOLUME_DOWN`(键盘页 0x80/0x81),特征保留 |

> **RC003 勘误与定案(2026-09-07,三轮探针 + MiVibe-Remote 研读互证)**:本机 RC-6459(REV&00A4,RC003 类固件)的返回键**固件确实上报**——HID Report GATT 特征(0x2A4D)的 9 字节报文 = `01 00 00` 前缀(report ID 1)+ 3×LE16 usage(同报最多 3 键),back = usage 0x00F1,tv = 0x0035;但 0xF1/0x35 是键盘页**非标准 usage**,微软 HidOverGatt WUDF 用户态驱动宿主收到报文后在内部丢弃它们(上游项目原注释:"Windows receives the report but exposes different subsets of it depending on the usage")。本项目的 LL 钩子、Raw Input、GATT 订阅全在 WUDFHost **下游**,三轮探针零事件是必然——**教训:下游全静默 ≠ 上游没发**。私有 BLE 服务(8 个可通知特征)与 ATVV 会话对 back 确实静默(这两通道结论不变);同设备方向/OK/home 键正常上报。此前"RC003 返回键=原生 Backspace(VK_BACK/0x0E)"的结论是**物理键盘(华硕 VID_0B05,实例 7&158463d9)Backspace 污染数据的误判**(LL 层 VK 特征与遥控器假设吻合所致)。方法论教训:**遥控器键的真伪判定必须同时核对 Raw Input 设备归属(hDevice)与按键时刻对照**,仅凭 LL 层 VK 特征不可定案;`hDevice=NULL` 的注入鼠标事件(空鼠类软件)与真实 HID 事件(`hDevice` 非 NULL)也可据此区分。BTHLE HID 服务的 GATT 特征对应用层不可用(FromIdAsync 默认访问/open_async 共享模式均 SharingViolation,get chars 返回 Unreachable;CreateFile GENERIC_READ 亦被系统 HID 栈独占),0 权限打开可枚举 usage(该设备 TLC=0x01/0x06,输入报文 121 字节,3 个 link collection)但不可读报文流。MiVibe-Remote 拿到 0xF1 的方式:Frida Gadget(x64 DLL,SHA256 校验,UAC 特权)注入 WUDFHost(定位:注册表 `BTHLEDevice\{00001812-…}_Dev_VID&012717_PID&32b8_…\…\Device Parameters\WUDFDiagnosticInfo` 的 `HostPid`),hook `ntdll!NtDeviceIoControlFile` 截获 IOCTL 0x80018483(READ_CHARACTERISTIC)成功返回的 9 字节输出,socket 回传 JSON 行;桌面端对 usage 集合做 diff 得 pressed/released 沿,再走与本项目同构的「LL 钩子佐证吞键 + 注入映射」防双触发(60/15ms 窗同源)。

> **音量键定案(2026-09-07 补充,与 back 同病)**:用户报告音量+/−按下无任何反应(无音量 OSD),同遥控器连 Mac 正常。实测:LL 键盘钩子 150/300/600 秒三轮窗口,音量+/音量−/back **零事件**(对照组:物理空格与遥控器左方向键同窗口内事件齐全,遥控器 HID 通道健康);0 权限句柄 `HidP_GetButtonCaps` 枚举报告描述符(注意:SDK 真实结构含 BitField/LinkCollection/IsRange 等字段,布局与常见记忆版本不同,且 UsageLength 参数是 in/out,入参须为缓冲元素数):**Report 1 = 键码页 0x07,usage 范围 0x0000..0x00FE**(标准键,Windows 翻译正常);**Report 6/7/8 = 厂商页 0xFF00,usage 范围 0x0000..0x00FF**(音量/返回等键所在)。Windows HID 栈(kbdhid)不翻译厂商页报告 → 零键盘事件。结论:音量键与 back 在 Windows 上同病——固件有发(或至少描述符有声明),但走非标准报告,系统翻译层不产出。Mac 可用最合理解释:固件按配对主机身份下发不同 Report Map(对 Mac 含标准音量 usage)或走 AVRCP(推断,Mac 侧未验证)。**工具链教训:读报告描述符原始字节的两条路(应用层 GATT 订阅/CreateFile 读权限、IOCTL_HID_GET_REPORT_DESCRIPTOR 0x0B0008)都被 HID 栈权限挡死,0 权限句柄只有 preparsedData/HidP 系列可用;探针脚本见 `%TEMP%\vs_probe\hid_caps.ps1`(Add-Type C#,ASCII 注释防 PS5.1 GBK 乱码吞行)。**

拦截的三个难题与对策:

1. **LL 钩子(WH_KEYBOARD_LL)拿不到按键来源设备**,无法区分「遥控器的 Home」与「物理键盘的 Home」。
   对策:**Raw Input(RIDEV_INPUTSINK)取 BREAK 沿归属**——注册 `(0x01,0x06)` 键盘页 + `(0x0C,0x01)` 消费页,`WM_INPUT` 的 `RAWKEYBOARD` 带 `hDevice`,经 `GetRawInputDeviceInfo(RIDI_DEVICENAME)` 取接口路径解析 VID/PID(小米 2 Pro:`0x2717`/`0x32B8`)精确归属。独立 Raw Input 线程只看松开沿,`PostMessage` 转主线程派发。这是对 MiVibe「WUDF/Frida 直读信号」的**零注入替代**(本项目红线:不引入 Frida)。
   > ⚠️ 教训(2026-09-07 真机排查定案):**不能用 `RIDI_DEVICEINFO` 读 VID/PID**——BTHLE 遥控器在 Raw Input 中呈现为 `RIM_TYPEKEYBOARD`,该查询只填 keyboard 联合体成员,`hid.dwVendorId` 恒 0,判定永远失败且无任何报错,佐证层静默失效(表现为映射「录入了但不生效」)。且 BTHLE 接口路径的 VID 字段为**六位**十六进制(`_Dev_VID&012717_PID&32b8_`,前两位疑似 Vendor ID Source 前缀),与 USB HID 名的四位(`VID_2717&PID_32B8`)并存,须按低 16 位比对(见 `XiaomiRawInputNameIsRemote`)。
2. **WM_INPUT 与 LL 钩子的相对时序未定义**,且二者与「吞」操作互斥(2026-09-07 三轮真机迭代定案,详见 `Doc/Expe/ll-hook-swallow-device-evidence-deadlock-2026-09-07.md`):LL 钩子是 RIT 同步调用,钩子内等待本次 WM_INPUT 永远等不到(佐证在钩子返回后 ~2-3ms 才到);钩子吞掉的键 MAKE/BREAK 沿双双不投递(「先吞后验」悖论);keydown 时刻决策只能靠先验,物理键盘误删且无法自愈。
   对策:**keyup 后置决策**——keydown/按住重复一律吞并登记 Pending(零副作用零等待);keyup 放行让 BREAK 沿投递携带 `hDevice`(孤立 up 无系统副作用,是取证动作);主线程收到证据后按归属收尾:遥控器→注入映射 down+up 对,物理→补偿原键 down+up 对;BREAK 异常丢失 200ms WM_TIMER 按物理兜底。
3. **按键归属确认后须吞掉原始键**(否则 back 的浏览器后退/方向键的焦点移动等原生副作用泄漏)。
   对策:keydown 即吞(返回 1),映射注入与物理补偿统一移到 keyup 后证据到达时由主线程 `SendInput` 完成(`LLKHF_INJECTED` 自带,自家钩子放行注入键,另带 `dwExtraInfo` 标记双保险)。代价:单击反馈延迟到松手瞬间、按住连删退化为单击多次(已知取舍,见经验文档遗留段)。

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
| 物理键盘按下与遥控器候选同 VK(Backspace/Home/方向/`) | 无 Raw Input 佐证 → 放行,不误吞 |
| 物理键盘同特征键按住(自动重复流) | 放行闩锁:等待失败确认后 120ms 窗内重复零等待,不阻塞键盘管线 |
| 佐证窗内恰好同名键并发(键盘+遥控器同键 15/60ms 内) | 极小概率误吞一次,松开即恢复 |
| 佐证信号乱序迟到(蓝牙抖动) | 首次 keydown 等待窗内命中即吞;超时放行(RC003 back 泄漏=一次原生 Backspace,与映射目标等效,无害) |
| 注入键触发全局热键/其他 LL 钩子 | 注入自带 `LLKHF_INJECTED`,自家两钩子均放行;第三方行为属用户配置责任 |
| 映射键含修饰键时的系统状态泄漏 | 每次注入 down/up 成对完整序列,不持有修饰键状态 |

## 4. 测试

- 单测(`core_tests.cc` `TestXiaomiKeymapInterceptor`):特征识别表全量、无映射/空串/非法串放行、佐证窗内吞+注入序列、佐证缺失/过期放行、闩锁自动重复与 keyup 关联、60/15ms 双窗边界、注入 down/up 序与反序
- 真机验收:配置 `back = "backspace"` → 记事本输入文字后按返回键删除字符;方向键未配置映射时原生行为不变;物理键盘 Backspace/Home/方向键不受遥控器映射影响(打字删除无可感知延迟);长按返回键连续删除
