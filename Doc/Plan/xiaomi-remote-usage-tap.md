# 小米遥控器 usage 直读移植方案(参考 MiVibe-Remote)

> 状态:**待用户决策**(方案 A/B/C 三选一,见 §5)。本文档为决策与实施蓝图。
> 事实链依据:`Doc/Plan/xiaomi-keymap-consumer.md` §1 勘误段(2026-09-07 定案)。

## 1. 背景与目标

RC003(小米蓝牙遥控器 2 Pro)固件经 HID Report GATT 特征(0x2A4D)上报**全部 13 键**——9 字节报文 = `01 00 00` 前缀(report ID 1)+ 3×LE16 usage(同报最多 3 键);但 back(0x00F1)、tv(0x0035)等键盘页非标准 usage 会被微软 HidOverGatt WUDF 用户态驱动宿主在翻译层内部丢弃,系统键盘层(LL 钩子/Raw Input/焦点应用)全静默。本项目现有 keymap 消费端(LL 钩子 + Raw Input 佐证)只能消费系统层可见的 11 键,back 不可映射。

目标:拿到被丢弃的 usage 沿(pressed/released),使 back(及 tv)可映射,且**不破坏**遥控器原生键盘功能与现有 11 键映射。

RC003 全键 usage 表(MiVibe `hid_report_tap.py` 与本项目 Raw Input 实测互证):

| 按钮 | usage | 按钮 | usage | 按钮 | usage |
|---|---|---|---|---|---|
| back | 0x00F1 | ok | 0x0028 | menu | 0x0065 |
| tv | 0x0035 | home | 0x004A | power | 0x0066 |
| | | right | 0x004F | volume_mute | 0x007F |
| | | left | 0x0050 | volume_up | 0x0080 |
| | | down | 0x0051 | volume_down | 0x0081 |
| | | up | 0x0052 | | |

官方 API 路线已全部堵死(实测):BTHLE GATT 特征 FromIdAsync 默认访问/open_async 共享均 SharingViolation、get chars Unreachable;CreateFile GENERIC_READ 被系统 HID 栈独占;键盘页 TLC 无 RAWHID 通道(Windows 强制走 RAWKEYBOARD 翻译,丢弃发生在翻译时)。**结论:只剩"在丢弃点之前截获"或"让系统不占用"两条路**,与 MiVibe 上游审计一致。

## 2. 方案对比

| 维度 | A. 自研注入 WUDFHost(推荐) | B. 禁用 HID 节点 + GATT 自管 | C. 维持现状 |
|---|---|---|---|
| back/tv 可映射 | ✅ 全 13 键 | ✅ 全 13 键 | ❌(home 替代) |
| 原生键盘功能 | ✅ 保留(系统翻译照常) | ❌ 全失,VoiceStick 注入替代 | ✅ 保留 |
| VoiceStick 不运行时 | 遥控器完全正常 | 按键全部无效 | 遥控器正常 |
| 权限 | 注入时需 admin/UAC 一次 | 禁用节点需 admin 一次(重配对后需再做) | 无 |
| 侵入性 | 高(注入系统进程) | 中(改设备状态,官方 API) | 无 |
| 杀软误报风险 | 有(注入行为特征) | 低 | 无 |
| Windows 更新脆弱性 | WUDFHost 内部 IOCTL 号变更即失效 | 低 | 无 |
| 核心假设验证状态 | MiVibe 已实证可行 | **未验证**(禁用后 GATT 能否订阅待实验) | — |
| 预估工作量 | 4-6 天 | 3-4 天(+实验) | 0 |

## 3. 方案 A 详细设计:自研注入 WUDFHost 截获 GATT 报文

不引入 Frida 库(红线字面),以最小自研件复刻其思路:特权注入 + hook `ntdll!NtDeviceIoControlFile` + 截获 IOCTL `0x80018483`(READ_CHARACTERISTIC)成功返回的 9 字节输出。

### 3.1 架构

```text
┌─ VoiceStick.exe(用户态,不提权)────────────────────────────┐
│ xiaomi_keymap_hook(现有,不变)                              │
│   ├─ LL 钩子:特征表候选 → 佐证窗决策(吞+注入/放行)        │
│   └─ Raw Input 佐证(现有,降级保留)                        │
│ xiaomi_usage_tap(新):命名管道客户端                       │
│   ├─ 收 9 字节报文 → usage 集合 diff → pressed/released 沿  │
│   ├─ back/tv(系统层不可见键):沿直触发映射动作            │
│   └─ 其余 11 键:tap 信号 = 精确佐证源(优先于 Raw Input)  │
│ WUDFHost 监视:轮询注册表 HostPid,变化→提示重注入          │
└──────────────────────────────────────────────────────────┘
        ▲ \\.\pipe\VoiceStickHidTap(命名管道,当前用户 ACL)
┌─ WUDFHost.exe(被注入)──────────────────────────────────┐
│ VoiceStickHidTap.dll(新):Detours hook                    │
│   NtDeviceIoControlFile → IOCTL==0x80018483 && 长度 9     │
│   && NT_SUCCESS → 读输出缓冲 → 写管道(定长二进制帧)      │
└──────────────────────────────────────────────────────────┘
        ▲ CreateRemoteThread(LoadLibraryW)
┌─ VoiceStickTapInject.exe(新,独立小工具,按需 UAC)────────┐
│ 定位 HostPid(注册表 WUDFDiagnosticInfo)→ 注入 DLL        │
└──────────────────────────────────────────────────────────┘
```

### 3.2 模块分解与关键技术点

1. **`xiaomi_usage_tap`(主程序侧,纯逻辑可 TDD)**
   - 9 字节报文校验(`01 00 00` 前缀)与 usage 集合解析(LE16 ×3,去 0)
   - 集合 diff → pressed/released 沿;断连/管道 EOF → 全释放(防按键卡死)
   - 与现有佐证窗融合:tap 信号时刻喂入 interceptor 的「按钮 → 最近佐证时刻」表,**优先级高于 Raw Input 佐证**(tap 带 GATT 层真源归属,物理键盘不可能产生 tap 信号,零误判)
   - back/tv 直触发路径:pressed → 查 key_map → 注入映射键(复用现有 SendInput 通道);back 长按重复(MiVibe 同款:首次 500ms 后 ~30ms 间隔,参数待真机调);released → 停重复
   - tap 不可用(WUDFHost 未注入/刚重启)自动回落现有 Raw Input 佐证,**现有 11 键映射不受影响**
2. **`VoiceStickHidTap.dll`(hook 载荷)**
   - hook 引擎:Microsoft Detours(MIT 许可,vcpkg `detours`,微软官方维护)——自研 x64 inline hook 需处理指令重定位,不值得
   - DllMain(或导出 Init)安装 DetourAttach;过滤条件三重(IOCTL 号/长度 9/NT_SUCCESS)最小化开销与误报
   - 管道写失败静默重试(WUDFHost 内不可弹 UI);DLL 不依赖 CRT 动态链接(/MT 静态),避免向系统进程带依赖
   - 部署位置:`%PROGRAMDATA%\VoiceStick\hid-tap\`,管理员写/用户读 ACL(对齐 MiVibe 的加固思路)
3. **`VoiceStickTapInject.exe`(注入器)**
   - HostPid 定位:注册表 `HKLM\SYSTEM\CurrentControlSet\Enum\BTHLEDevice\{00001812-…}_Dev_VID&012717_PID&32b8_…\<instance>\Device Parameters\WUDFDiagnosticInfo` 的 `HostPid`(VID/PID 按低 16 位匹配,复用 `XiaomiRawInputNameIsRemote` 的解析规则)
   - `OpenProcess`(VM_OPERATION|VM_WRITE|CREATE_THREAD|VM_READ|QUERY_INFORMATION)→ `VirtualAllocEx` 写 DLL 路径 → `CreateRemoteThread(LoadLibraryW)`
   - 幂等:注入前查 WUDFHost 是否已载入本 DLL(枚举模块),避免重复
   - UAC 策略:默认按需弹 UAC(`ShellExecute` runas);提供可选「计划任务自动重注入」(schtasks 最高权限一次性设置,免每次弹窗),默认不开启
4. **WUDFHost 重启监视(主程序侧)**
   - 30s 轮询 HostPid;变化 → 管道断开确认 → 托盘气泡提示「遥控器重新连接,点击重新启用增强模式」→ 触发注入器 UAC

### 3.3 TDD 计划(红-绿-重构)

- 纯函数层(`xiaomi_usage_tap` 解析/diff/沿生成/断连全释放)→ `core_tests.cc` 新增用例,先红后绿
- 佐证融合决策(tap 优先/Raw Input 回落/超时放行)→ interceptor 既有测试模式扩展
- hook DLL 与注入器:系统级,无单测覆盖,以冒烟脚本验证(注入 → 管道心跳 → 真机按键出报文),冒烟不通过不算完成
- 回归:现有 keymap 全部用例 + 全套 CTest 必须绿

### 3.4 风险与对策

- **杀软误报**(注入+远程线程是恶意软件常见特征):发布产物走既有签名/白名单渠道;DLL 固定路径+固定 hash;文档向用户明示原理。接受残余风险。
- **Windows 更新改 IOCTL/宿主行为**:过滤条件失效仅意味着 tap 静默,主程序回落 Raw Input 佐证,不崩溃(管道无数据→超时判不可用)。
- **GPL 合规**:MiVibe-Remote 为 GPL-3.0,仅参考协议事实(IOCTL 号/报文格式/usage 表/注册表路径)与架构思路,**实现全部自写,不复制任何代码**。
- **红线声明**:本方案不引入 Frida 库;但「注入系统进程」与红线精神(当初为避免 Frida 重依赖而设)存在张力,故列为需用户明示拍板项。

## 4. 方案 B 详细设计:禁用 HID 设备节点 + GATT 自管

管理员下 SetupAPI(`SetupDiSetClassInstallParams` + `DICS_DISABLE`)禁用遥控器 0x1812 服务的 HID 设备节点 → WUDF 宿主卸载、GATT 占用预期解除 → VoiceStick(复用 `ble_central_win` 的 WinRT BLE 栈)订阅 0x2A4D notify 自解析 usage 报文 → **全部 13 键自管**:系统翻译不复存在,方向/OK/音量等也全部由 VoiceStick 注入。

- 优点:零注入、全官方 API、一次性拿到全部键
- 致命代价:VoiceStick 不运行时遥控器**所有按键失效**(遥控器沦为 VoiceStick 专属外设);系统音量键从硬件路径变为软件注入;重新配对后节点重建需再次禁用
- **核心假设未验证**:禁用节点后 SharingViolation 是否解除(需 admin 实验:禁用 → 跑 GATT 订阅探针 → 恢复启用;实验期间遥控器键盘短暂失灵)
- 若实验失败(占用不解除或报文不来),方案 B 作废,回到 A/C

## 5. 决策请求

| 问题 | 选项 |
|---|---|
| 移植路线 | **A** 自研注入(能力全/侵入高)/ **B** 禁用节点+自管(零注入/接管全键,需先实验)/ **C** 维持现状(home 已可删字) |

推荐 **A**:唯一同时满足「back 可映射」+「遥控器保持正常 Windows 键盘」的路线,MiVibe 已实证机制可行;侵入性代价(一次性 UAC、杀软残余风险)明确且可控。若用户不接受任何注入,**B 先实验再定**;若 back 非刚需,**C** 零成本。
