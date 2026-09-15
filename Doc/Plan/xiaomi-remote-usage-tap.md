# 小米遥控器 usage 直读移植方案(参考 MiVibe-Remote)

> 状态:**重启——A 方案(2026-09-15,用户需求变化,见 §5 决策史)**。2026-09-07 曾定案 C(维持现状);09-15 用户要求三键(back/volume_up/volume_down)全量可识别,触发上文既定重启条件,按「可选高级功能、默认关闭、运行时提权、hook 彻底自研(GPL/Frida 双规避)」激活方案 A,并按 §6 MiVibe 深读细节修订实施参数。
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
| 可行性验证状态 | MiVibe 已实证可行 | **已证伪**(2026-09-07 提权实验,见 §4) | — |
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

## 4. 方案 B ~~详细设计~~ → **已证伪(2026-09-07 提权实验定案)**

原假设:管理员下 SetupAPI/`Disable-PnpDevice` 禁用遥控器 0x1812 HID 设备节点 → WUDF 宿主卸载 → GATT 占用解除 → VoiceStick 订阅 0x2A4D 自管全键。

**实验结果(三轮迭代,第三轮可信)**:禁用确凿生效(父节点 `CM_PROB_DISABLED`/Status=Error,子 HID 键盘节点 PHANTOM),但:

- `get_gatt_services_async` 正常,0x1812 服务**仍在枚举**(9 服务全在);
- `open_async(SHARED_READ_AND_WRITE)` 仍返回 **5 = SharingViolation**;
- `get_characteristics_async` 仍 **3 = Unreachable**——与未禁用时的历史状态完全一致。

**结论:0x2A4D 特征的独占与 HID 设备节点无关,来自 BthLE 蓝牙栈服务层对已配对 GATT 服务的持久保留。** 禁用节点只卸载 HID 客户端驱动栈(HidOverGatt 宿主 + kbdhid),不触碰服务层独占——"禁用设备节点释放 GATT"路线在 Windows BTHLE 架构下不成立,方案 B 出局。理论变体 B'(改配对注册表让 BthLEEnum 不挂载 0x1812 服务)属深水区(改系统配对数据库、成功率低、代价与 B 相同),不推荐。

> 实验方法教训(记入记忆):①`Disable-PnpDevice` 必须 `-ErrorAction Stop -PassThru` + 等待 ≥8 秒后用 `(Get-PnpDevice).Problem == CM_PROB_DISABLED` 确认——前两轮 3-4 秒轮询 + 无错误捕获,禁用未生效/未确认,产生自相矛盾的假观察;②winrt python 包 `str(uuid)` 返回**裸 UUID 不带花括号**,探针常量带花括号会静默匹配失败(本实验第一轮"0x1812 服务消失"即此 bug 假象);③探针必须打印中间层完整列表(服务/特征 UUID),否则提前退出的失败会伪装成"上游消失"。

## 5. 决策请求

| 问题 | 选项 |
|---|---|
| 移植路线 | **A** 自研注入(能力全/侵入高)/ **C** 维持现状(home 已可删字)。~~B~~ 已证伪(§4) |

推荐 **A**:B 出局后,唯一能拿到 back 的路线。MiVibe 已实证机制可行;侵入性代价(一次性 UAC、杀软残余风险)明确且可控。若用户不接受任何注入,**C** 零成本(back 放弃,home 替代)。

> **决策定案(2026-09-07,用户确认):维持 C 方案,不启用注入路线。** 同日音量键定案后复核:非标准通道键共 3 个(back/volume_up/volume_down,厂商页 0xFF00 报告不被 kbdhid 翻译,见 `xiaomi-keymap-consumer.md` §1),用户在了解 A 路线全部优劣(信号完整/归属精确/零延迟 vs UAC+杀软对抗/WUDFHost 崩溃连带/自研 hook 维护/签名分发影响/仅为 3 键)后仍选择 C——三键接受不可用,home/方向/OK/menu/tv/power 六键映射已够用且 v4 链路真机验证通过。除非未来需求变化,不再重新评估;若重启此路线,按"可选高级功能、默认关闭、运行时提权、hook 部分彻底自研(GPL/Frida 双规避)"实施。

> **决策更新(2026-09-15,重启 A 方案)**:用户新需求——「实现对小米蓝牙遥控器 BLE HID 设备的按键全量识别,返回键/音量加/音量减三键必须可用」,即 09-07 放弃的目标重新成立,构成上文预留的重启条件。当日对 MiVibe-Remote 现行源码(`platforms/windows/source/bridges/xiaomi/`)二次深读,机制结论与 09-07 研读一致(路线唯一性不变),新增工程细节见 §6;实施参数按 §6 真机值修订。

## 6. MiVibe-Remote 深读增补(2026-09-15)与实施参数修订

对 MiVibe 现行 Windows 实现的二次深读(`hid_report_tap.py` / `hid_tap_runtime.py` / `hid_tap_injector.py` / `atvv_live_bridge.py`),机制与 09-07 研读一致——**没有安装任何驱动,本质是特权注入只读探针**;以下工程细节为本次新增,方案 A 实施时吸收。

### 6.1 MiVibe 工程细节清单

| 机制 | MiVibe 现行实现(真机在用) | 方案 A 吸收方式 |
|---|---|---|
| hook 载荷 | Frida Gadget 17.15.3 x64 DLL(xz 压缩内嵌,双层 SHA-256 锁定:压缩包+解压 DLL) | 自研 Detours DLL,MSI 随包分发,构建期算 hash 写入安装清单(自研故 hash 自控,无运行时下载) |
| 部署与加固 | 解压至 `%PROGRAMDATA%\MiVibeRemote\hid-tap\<ver>-x64-<hash12>\`,`icacls /inheritance:r` 锁 ACL(SYSTEM/Admins=F,Users=RX),防降级替换 | 同路径策略 `%PROGRAMDATA%\VoiceStick\hid-tap\`,同 ACL 锁;注入前校验 DLL hash |
| 注入器 | 独立提权子进程(`ShellExecuteW "runas"` 隐藏启动),四重校验:IsUserAnAdmin、目标 PID==注册表当前 HostPid、进程名必须 `wudfhost.exe`、DLL SHA-256 一致;SeDebugPrivilege + VirtualAllocEx/WriteProcessMemory/CreateRemoteThread(LoadLibraryW) | 同构:`VoiceStickTapInject.exe`,同四重校验(防误注入/防资产篡改) |
| 回传通道 | 127.0.0.1:30684 TCP,JSON 行协议(ready/heartbeat/gatt_read/error),回传 hex | 命名管道 `\\.\pipe\VoiceStickHidTap`(无端口占用冲突,当前用户 ACL),定长二进制帧+心跳帧 |
| 健康监测 | 载荷 5s 心跳;宿主侧心跳停 15s→UNHEALTHY 重连;每 2s 轮询注册表 HostPid,PID 变→HOST CHANGED→重注入(再次 UAC) | 同参数:心跳 5s/不健康阈值 15s/HostPid 轮询 2s;宿主变化走「托盘气泡提示+按需 UAC 重注入」 |
| 防双触发 | LL 键盘钩子在系统翻译到达时**等 tap 同键直连信号**(窗口:特殊键 60ms/常规键 15ms),命中才吞(返 1),否则放行——物理键盘零误伤 | 融入现有 interceptor:tap 沿写入「按钮→最近佐证时刻」表且**优先级高于 Raw Input**;系统可见键沿用现有 keyup 后置决策,tap 命中即定性为遥控器 |
| 长按重复 | back:首次 280ms 后每 40ms;volume:首次 400ms 后每 120ms(真机值) | 取同值做初值(`hid_tap_back_repeat_delay_ms=280/interval=40`,`volume 400/120`),设置页可调 |
| 兼容性门控 | `hid_report_tap_enabled`(默认开)AND `hid_tap_compatible`(默认**关**)双开关;Gadget 缺失/注入被拒→自动回落传统 Raw Input 映射线程,不阻塞启动 | 同策略:配置 `xiaomi.hid_tap_enabled` 默认**关**;tap 不可用时回落现有 9 键管线,三键静默不可用(与 09-07 现状一致) |
| 直连 GATT 备胎 | 保留 `XiaomiGattHidSession`(WinRT 订阅 0x2A4D)作无微软 HID 驱动机器的备胎;注释确认「正常 HID 子节点在启用时该路 access denied」 | 不实现——本项目 09-07 已实测同一结论(FromIdAsync SharingViolation/Unreachable),备胎无增益 |

### 6.2 修订后的实施要点(相对 §3 初稿的增量)

1. **默认关闭 + 运行时提权**:配置新增 `xiaomi.hid_tap_enabled`(默认 false);设置页「遥控器」分组加「增强按键识别(返回/音量键)」开关与状态行(未启用/tap 心跳正常/宿主已重启待重新授权/注入被拒)。开启时才触发注入器 UAC,关闭即停监视并释放按键状态。
2. **usage 前转集合**:13 键全量(MiVibe 与本项目实测表互证一致,含 volume_mute 0x7F——RC003 是否有独立静音键待真机确认,先转发不进映射表)。三键(back 0x00F1/vol+ 0x0080/vol− 0x0081)为系统不可见键,tap 沿直触发映射;其余 10 键 tap 沿仅作佐证。
3. **UAC 频次控制**:WUDFHost 对 BTHLE 设备按连接起停,重连可能换宿主进程→每次重注入都要提权。缓解:①默认按需弹 UAC+托盘气泡引导;②提供可选「计划任务静默重注入」(schtasks 最高权限一次性设置,默认关,设置页显式开启);③宿主 PID 未变时不重复注入(幂等:枚举目标进程模块,已载入则跳过)。
4. **杀软与签名**:自研 DLL+注入器随 MSI 走既有签名渠道;DLL 固定路径+hash 校验+文档明示原理(README/网站 FAQ 增补「增强按键识别的原理与权限说明」)。接受残余误报风险,用户侧可关闭功能。
5. **崩溃隔离**:hook DLL 只读旁路(IOCTL 过滤三重:号==0x80018483、NT_SUCCESS、输出长==9),任何异常吞掉不冒泡——WUDFHost 崩溃会连带遥控器/其它 BTHLE 外设掉线,载荷必须零侵入;管道写失败静默,宿主侧超时判不可用即回落。
6. **TDD 边界不变**:报文校验(9 字节/`01 00 00` 前缀)、usage 集合解析与 diff、沿生成、断连全释放、tap 佐证融合优先级、长按重复节拍——全部纯逻辑进 `voicestick_core` 先红后绿;DLL/注入器/管道以真机冒烟脚本验证(注入→心跳→13 键逐个出沿→回落路径),冒烟不过不算完成。
