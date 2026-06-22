 启用 BLE bonding 以加快 Stick 重启/主机重启后的重连

 Context

 当前 Stick 与 Windows 端的“配对”主要是应用层概念：Windows 保存了 paired_devices、设备地址和设备 ID，但固件端 NimBLE 明确禁用了
 bonding，Windows 端连接前还会主动调用 TryUnpairAsync() 清理系统 bond。结果是 Stick 或电脑重启后，双方不能复用 BLE
 长期密钥/系统配对信息，只能依赖广告扫描和重新建立 GATT 连接，重连等待时间偏长。

 目标是让已经连接过的主机信息持久化到 Stick 的 NVS 中，并让 Windows 保留系统 bond，从而在 Stick 重启或主机重启后更快恢复连接；同时保留 stale    
 bond 失败后的自动修复能力。

 推荐方案

 1. 固件启用 NimBLE bonding，但保持 Just Works 低交互配对：
   - 在 firmware/components/voice_ble/voice_ble.c 的 voice_ble_init() 中，将 ble_hs_cfg.sm_bonding 从 0 改为 1。
   - 设置 ble_hs_cfg.sm_mitm = 0，避免要求显示码/输入码。
   - 设置 ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT。
   - 设置 ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC 和 ble_hs_cfg.sm_their_key_dist =
 BLE_SM_PAIR_KEY_DIST_ENC，让双方保存加密长期密钥。
   - 复用现有 nvs_flash_init()；NimBLE 会通过 NVS 持久化 bond 信息。
 2. Windows 端停止“连接前无条件 unpair”：
   - 在 desktop/windows/src/ble_central_win.cc 的 BleCentralWin::ConnectDeviceAsync() 中，移除连接前 probe + TryUnpairAsync() 的预清理块。      
   - 保留现有服务发现失败后的 IsLikelyStaleBondError() / GattCommunicationStatus::Unreachable 分支，只在确认为 stale bond 时再
 TryUnpairAsync()，避免破坏正常 bond 快速重连。
   - 更新相关日志文案，区分“保留 bond 的正常连接”和“失败后清理 stale bond”。
 3. 暂不强制 GATT 特征加密：
   - 现阶段不把特征权限改成 encrypted-only，避免扩大兼容性风险。
   - 本轮重点是保存和复用 bond，改善重连速度。
 4. 升级兼容策略：
   - 已经使用旧固件/旧 Windows 端配对过的用户，第一次升级后可能存在 Windows/Stick bond 状态不一致。
   - 通过保留失败后的 stale bond 清理逻辑自动恢复；必要时用户手动重新配对一次。

 关键文件与修改点

 firmware/components/voice_ble/voice_ble.c

 复用现有：
 - voice_ble_init() 中已有 nvs_flash_init()。
 - 当前 NimBLE 初始化流程：nimble_port_init()、ble_svc_gap_init()、ble_svc_gatt_init()、ble_hs_cfg.reset_cb/sync_cb。

 修改：
 - 将安全配置从禁用 bonding 改为启用 bonding + Just Works。
 - 可增加 GAP security/bond 相关日志，便于验证配对是否完成。

 desktop/windows/src/ble_central_win.cc

 复用现有：
 - ConnectDeviceAsync()。
 - TryUnpairAsync()。
 - IsLikelyStaleBondError()。
 - GattCommunicationStatus::Unreachable fallback。

 修改：
 - 删除连接前第一个无条件 unpair 块。
 - 保留失败后 unpair/radio reset/reopen 的恢复逻辑。

 验证计划

 1. Windows 构建与测试：
   - 构建 Windows 应用。
   - 运行 ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests。
 2. 固件构建与烧录：
   - 运行 ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests。
 2. 固件构建与烧录：
   - 运行 idf.py -C firmware build。
   - 构建通过后按用户偏好默认烧录到当前设备端口 COM17。
 3. 手动验证：
   - 升级后先重新配对一次，确认可正常录音。
   - Stick 断电/重启后，打开 Windows 应用，确认无需重新配对且连接速度明显变快。
   - Windows 关机再启动，确认启动 VoiceStick 后能快速恢复连接。
   - 删除 Stick NVS 或刷入清空 bond 的固件后，验证 stale bond fallback 能自动清理 Windows 端旧 bond 并允许重新配对。

 风险与边界 
 
 - 首次启用 bonding 后，旧配对状态可能不一致，可能需要重新配对一次。
 - Windows BLE 栈缓存行为不完全可控，仍可能偶发需要手动关闭/打开蓝牙或删除设备。
 - 多主机同时绑定可能受 NimBLE bond 数量限制；本轮先优化单主机/常用主机快速重连体验。
 - 不把 GATT 特征强制加密意味着这不是完整安全加固，只是启用 bond 持久化和快速重连。