# 小米遥控器 2 Pro 接入：采集链路与 F5 抑制排障经验（2026-09-02）

> 记录时点结论，寄存器值/阈值/文件:行号引用前以当前源码为准。功能设计见
> `Doc/Plan/xiaomi-remote-2-pro-support.md`，工具用法见 `Doc/Ref/e2e-test-toolchain.md`。

背景：小米蓝牙遥控器 2 Pro（ATVV 协议）接入 Windows 桌面端的真机联调与 golden
采集过程中，踩了一批 BLE 人格、ctypes 钩子、采集手法的坑。功能本身（配对/双击
Enter/睡眠重连/双机切换）全部真机通过，golden 基线 3 句火山 ASR CER 0/0/0.182。
本文沉淀排障结论，供后续 macOS 移植（P5）与采集工具使用者参考。

## 设备行为事实（真机实测）

- **ATVV 语音人格只授给第一个连上的主机**。遥控器同时维持多个人格：完整人格
  含 ATVV（`AB5E0001-...`）+ HID（`1812`）等服务；当已有主机（如 VoiceStick.exe）
  持有语音链路时，第二个 BLE 连接能 connect 成功，但 GATT 表里只剩 7 个基础服务
  （1801/1800/180F/180A/`8a7a0001-...`/`000001bf`/FE59-DFU），无 ATVV 无 HID。
  症状是 "ATVV service not found"，极易误判为配对/缓存问题。**采集或调试前必须
  先退出 VoiceStick.exe**；反之桌面端连不上时也要检查是否有采集/探测进程占着。
- **2 Pro 是 `0x04` 直开入径**：按语音键直接发 STREAM_START（不发 MIC_OPEN
  0x08 等前导），音频几乎同帧到达；松开按键即 STOP（`00 02`），会话时长跟随
  按键时长（实测 0.6s~4.5s 均有）。不存在远程端 VAD 自动截断 1 秒的行为——
  早期采集到的"1 秒会话"全部是用户提前松键或没说话。
- **遥控器睡眠后快速重连会拿到陈旧 GATT 缓存**，表现为 service 枚举缺失。
  bleak 侧根治：连接时传 `winrt=dict(use_cached_services=False)`。
- **语音键经 OS 级 HID 通道连带发 F5**，按住期间以 ~30ms 自动重复。F5（HID 栈）
  可先于 ATVV 帧（ATT 栈）约 1ms 到达——桌面端 `voice_f5_suppressor.cc` 的
  三层抑制（近窗命中 / 首次 keydown 关联等待至多 80ms / keyup 键程闩锁）就是为
  这个竞态设计的，纯读协议文档推不出来，只能真机实测发现。

## ctypes 调 Win32 钩子的两个 64 位签名坑（atvv_capture.py F5Suppressor）

Python ctypes 默认 `restype=c_int`，在 x64 上会静默截断 64 位句柄/指针：

1. `GetModuleHandleW` 返回的 HMODULE 被截成 32 位 → `SetWindowsHookExW` 报
   `ERROR_MOD_NOT_FOUND(126)`，钩子静默装不上（打印 "on" 与否取决于截断后值
   是否非零，不可信）。
2. 修好安装后，`CallNextHookEx` 的 LPARAM（64 位指针值）按默认签名转换溢出
   `OverflowError`，回调内抛异常、按键事件一多直接刷爆日志。

修法（探针 `SetWindowsHookExW`+`SendInput` 合成按键验证通过）：所有涉及的
API 显式声明 `restype`/`argtypes`（`HMODULE`/`c_void_p`/`WPARAM`/`LPARAM`），
LL 钩子回调线程必须有消息泵。另外 `SendInput` 的 `INPUT` 结构体在 x64 是
40 字节（含 MOUSEINPUT union），只定义 KEYBDINPUT 得到 32 字节会导致
`SendInput` 静默返回 0，探针阶段曾被这个假阴性误导一轮。

## 采集手法谜题与"数数实验"方法论

症状：采集到的音频能量包络/频谱异常（低频一团、无共振峰、或只有开头一声按键
音后掉数字静音），火山 bench 全部 "no definite result"；而同一遥控器、同一解码
算法（C++ `ImaAdpcmDecoder` 与 Python 版逐字节等价）的活体链路 ASR 完全正常。

排查动线（正确的部分）：同字节双解码器交叉排除了解码差异；帧计数与时间窗吻合
排除了丢帧；最终用**数数实验**一锤定音——让用户按住语音键大声数
「一二三四五六七八九十」，已知内容+强周期信号使包络/pitch/ASR 三路同时可判。
结论：**链路从未损坏，全部异常都是采集手法**（没贴嘴、开口晚、提前松）。

教训：遇到"活体能用、采集不能用"，先怀疑测量手法/测量环境，再怀疑链路；设计
判别实验时用已知内容的高辨识度信号（数数优于句子），并给采集工具加**用户可见
反馈**（弹窗）与**自动重连**——纯日志文件的后台采集让用户反复误判"没录上"，
人机对齐窗口的协调成本远超工具改造本身。

## 遗留小瑕疵（已知、未修、不影响主功能）

- 桌面端连接阶段日志前缀误用 `VS-6459`（应 `RC-`），仅显示问题。
- 采集器 `first_audio_latency` 对 2 Pro 恒为 None（该指标 keyed off 0x08
  MIC_OPEN，2 Pro 直开不发）。
- 控制/音频两特征存在到达序竞态：会话首包音频可能先于 0x04 到达被丢（约
  15ms 音频，对 ASR 无实质影响）。
