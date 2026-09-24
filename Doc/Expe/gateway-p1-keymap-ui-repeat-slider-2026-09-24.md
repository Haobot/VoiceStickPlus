# 网关 P1 按键映射 UI/长按连发/连发间隔滑块——交付复盘与小坑集合

> 日期：2026-09-23 ~ 2026-09-24
> 相关文件：`desktop/windows/src/{win32_app, xiaomi_keymap_hook, xiaomi_keymap_interceptor, xiaomi_keymap_dialog, app_config}.{h,cc}`、`desktop/windows/tests/core_tests.cc`
> 相关提交：本日提交（见 git log feat/stick-gateway）
> 设计与验收：`Doc/Plan/xiaomi-remote-stick-gateway.md` §8.2（同日追加）

## 症状（演进过程）

1. 网关模式（遥控器配对在 StickS3、不直连 Windows）下，用户**没有任何入口**
   配置按键映射——P1 软件路由链路（固件/协议/协调器/注入均已就绪）实际不可达。
2. 映射打通后用户反馈：Backspace 映射按住只删一字，期望「像音量键一样长按
   连续删除」。
3. 连发默认 120ms 节拍偏慢，用户要求可调。

## 修复（机制要点）

1. **UI 入口**：托盘设备子菜单的按键映射入口原以 `is_xiaomi` 门控；StickS3
   设备项新增同入口，命令处理按 `config_.paired_devices` 的 hardware 分发——
   直连 RC 走原 `ShowXiaomiKeymapDialog`（设备覆盖配置），网关走新
   `ShowGatewayKeymapDialog`（编辑全局默认 `[xiaomi.keys]`，与协调器
   `PushGatewayKeymapRoutesFor` 无配对 RC 时的取值口径一致，两侧天然同源）。
2. **长按连发**：`XiaomiGatewayKeyRepeater`（纯逻辑，`xiaomi_keymap_interceptor`
   系）——按下沿登记 hold（调用方已注入 down 序，保持组合键真按住语义），
   400ms 延迟后按 120ms 间隔产出完整 down+up 对；松开沿清除。节拍对齐既有
   `XiaomiTapRepeatTimingFor` 的 volume 档（400/120，MiVibe 真机值），复用钩子
   同一个 40ms `kRepeatTimerId`（`SyncRepeatTimer` 的 any_hold 纳入网关 hold）。
   映射热更被取消时 hold 即清（放行语义）；Stop/断连 Reset 防卡键。
3. **间隔滑块**：新配置 `xiaomi_gateway_repeat_interval_ms`（默认 120，钳位
   [30,300]）；对话框 `TRACKBAR_CLASS` 滑块，**WM_HSCROLL 里 `TB_THUMBTRACK`
   只刷数值标签、`TB_ENDTRACK`/离散步进（LINEUP/LINEDOWN/PAGEUP…）才触发
   回调**——回调做 config 落盘 + `ApplyUpdatedConfig`；`SyncXiaomiKeymapHook`
   入口处在启停判定**之前**调 `SetGatewayRepeatIntervalMs`，钩子未运行时也把
   新值带入，Start 后立即按新节拍工作。

## 经验

- **判据**：托盘子菜单的设备级功能入口，门控条件里只写设备类型不写使用模式
  （直连 vs 网关），是「功能链路全通但用户永远够不着」类缺陷的典型来源——
  新设备形态接入时逐项过一遍菜单入口的门控条件。
- **连发/重复类功能优先复用既有节拍真机值**（volume 400/120），不要新造参数；
  用户对「像音量键一样」的表述就是复用信号。
- **滑块等连续输入控件的回调节流放在控件层**（THUMBTRACK 刷 UI、ENDTRACK 触
  业务），比在业务回调里做防抖简单且无定时器管理负担。
- **全局 vs 设备覆盖配置的取值口径必须两侧一致**：对话框编辑哪个配置对象、
  协调器下发读哪个配置对象，若不同源就会出现「改了不生效」；网关映射选全局
  默认正是为了让编辑与下发同源。
- 桌面可映射键表 12 键（`kXiaomiMappableButtons`，无 mic/volume_mute）与固件
  可路由键表 13 键（含 volume_mute）是**有意口径差**：volume_mute 无映射场景
  固件保持直通默认即可，桌面无需为它发路由命令。

## 教训（本轮踩到的已知坑新变体）

- `LNK1104 VoiceStick.exe` 锁定：改桌面端构建前先杀运行中进程（已知坑，本轮
  两次撞上——重启桌面端验证新功能与继续改码的循环里容易忘）。
- 剪贴板 vault 测试在本机有剪贴板监听类软件时 `OpenClipboard(nullptr)` 瞬时
  失败裸 assert 即崩：已改为带 25×20ms 重试的 `VaultOpenClipboardWithRetry()`
  （Win32 官方建议的重试姿势）；**对系统共享资源断言要做瞬态重试**，这不是
  掩盖失败而是区分「真失败」与「瞬时占用」。
- `ConnectedDevice{"53A8", "VS-53A8", kHardwareStickS3}` 聚合初始化报
  string_view→string 无转换：`std::string_view` 常量进聚合初始化需显式
  `std::string(...)` 包装。
- Git Bash 里 `cmd //c '... 2>&1'` 的输出重定向会静默丢失——捕获构建输出一律
  走 .bat 包装脚本（`run_tests.py` 同款模式），不要在行内拼复杂引号。

## 验证

- 全部单测通过（含新增 `TestCoordinatorPushesGatewayKeymapRoutes`、
  `TestXiaomiGatewayKeyRepeater` 连发节拍/多键并存/热更取消/间隔钳位）。
- 真机（用户，2026-09-24）：back→Backspace 映射生效、按住连续删除生效、
  滑块调速生效，主观评价「效果非常好」。

## 遗留/观察项

- 长按启动延迟（400ms）暂不可调，用户未提需求；如后续要调，照连发间隔的
  配置项模式加一个字段即可。
- 直连 RC 的按键映射对话框同样显示该滑块（全局设置共享），直连模式自身的
  连发走直触发节拍表——两套节拍并存是有意设计（网关可调、直连按 MiVibe
  真机值固定），文档此处留痕防止未来误「统一」。
