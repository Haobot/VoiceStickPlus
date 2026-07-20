# 经验:Deep sleep 后卡 Pairing——僵尸 BLE 会话导致的重连死锁

- 日期:2026-07-17
- 提交:`6763f55` `fix: deep sleep 唤醒后卡 pairing 无法回连录音`
- 现象:设备闲置超过 10 分钟后,拿起屏幕不亮;按下录音键屏幕亮起但一直停在 pairing 界面,Windows 端却显示"已连接",无法录音。

## 一、根因(三层缺陷叠加,缺一不可)

1. **固件 deep sleep 前不做优雅断连**。`enter_power_off()` 全程不碰 BLE,直接 `esp_deep_sleep_start()`。连接态闲置 5 分钟(`POWEROFF_TIMEOUT_MS`)设备就带着活连接断电沉睡,链路无声消失,对端只能靠 supervision timeout 发现。
2. **WinRT 断连事件不可靠**:`BluetoothLEDevice.ConnectionStatusChanged` 是 Windows 端唯一的断连检测通道,而对"空闲 GATT 连接的对端无 LL_TERMINATE 静默消失"这一场景,该事件经常不向应用投递。事件不来 → 会话永不清除。
3. **重连逻辑自我封锁**:`HandleAdvertisement` 里 `if (sessions_by_device_id_.contains(*device_id)) return;` 把设备唤醒后的广播静默否决。广告 watcher 一直在跑、广播能收到能匹配,却被旧会话挡死——设备卡 pairing,主机显示已连接,永久死锁。录音则因固件 `ble_ready=0` 被拒。

## 二、修复(两端各一刀,互补)

- **固件**(治源头):新增 `voice_ble_disconnect(timeout_ms)`,`ble_gap_terminate` 后同步等 DISCONNECT 事件(10ms 轮询,超时 1s 不阻塞关机);`enter_power_off()` 进 deep sleep 前调用,让主机立刻收到断连事件,走已验证可用的正常重连路径。
- **Windows**(兜底线):`HandleAdvertisement` 检测到"已配对设备带着本地会话重新广播"时,先 `HandleDeviceDisconnected` 拆除僵尸会话再重连。固件只在未连接时广播(`start_advertising_with_mode` 在 `s_connected` 时直接返回),所以这条广告本身就是旧链路已死的铁证——不依赖任何 WinRT 事件,旧固件或事件仍丢失时也能自愈。

## 三、验证

- ESP-IDF 增量构建通过;Windows 全量重建通过;CTest 2/2(单测 + 集成)通过。
- 此 Bug 是跨设备时序问题,现有测试框架无法自动化复现(固件无单测、WinRT 外壳不在单测覆盖内),最终确认靠真机:静置 5+ 分钟 → 按键唤醒 → 自动回连 → 正常录音。
- 已串口烧录到 COM19 设备。

## 四、长期技术记忆

1. **WinRT `ConnectionStatusChanged` 不能作为唯一断连检测通道**。对端静默消失时它可能永不投递;`ConnectionStatus` 属性也会长时间停留在 Connected。需要第二通道:本仓库用的是"广告反证法"(见下),备选还有 `GattSession.SessionStatusChanged`、写入失败触发拆除、周期心跳。仓库此前已对广告 watcher 的同类静默失效做过 `RestartForResume()`,连接对象的同构问题这次才补上。
2. **广告反证法:外设只在未连接时广播,则"已配对设备带会话广播"=会话已死的铁证**。这是零成本、确定性的机会主义检测,比重试/超时都快。凡做 BLE central 重连逻辑,都应检查"会话表否决重连"这类自我封锁。
3. **主机侧"已连接"状态是只增不减的缓存快照,不是链路实况**。本仓库三层状态(BLE ready 标志 → 协调器 connected_device_ids_ → UI)都源自同一个过期标志位,只有连接成功/断连事件/配对变更/休眠恢复四处刷新。展示态与链路实况脱节时,优先怀疑状态快照没有失效路径。
4. **固件进 deep sleep / 断电前,必须先 `ble_gap_terminate` 优雅断连**(AGENTS.md 已有"音频会话 stop 必须等 drain"的同类原则——凡会让对端干等的动作,本端都要主动收尾)。`Doc/Plan/固件待机省电策略.md:314` 设计时就假设了"S3 关机:BLE 断开,桌面端按现有断连处理",实现却一直没做这一步,设计与实现脱节近一年才爆雷。
5. **好样本是定位坏场景的钥匙**:桌面日志(`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`,非 Roaming)里恰好有一次断连事件送达→重连成功的完整记录(12:08),证明了下游链路正常,问题被锁定在"事件未送达"的分支上。排查偶发问题时,先在日志里找一次成功案例做对照。

## 五、环境/工具教训

- **Git Bash 下 `cmd /c xxx.bat` 会被 MSYS 路径转换吞掉 `/c` 参数**(cmd 只打印 banner 就退出,假象是"脚本没跑")。需 `MSYS_NO_PATHCONV=1 cmd /c xxx.bat` 或 `cmd //c`。同理 plain cmd 无 ctest,要经 `vcvars64.bat`。
- **esptool 报 `Cannot configure port` OSError 433(指定不存在的设备)**:多为设备已退出 BOOT 模式重新枚举、或端口被瞬时占用。先 `idf_cli.py --list-ports` 确认端口在(vid_pid=303A:1001 即 ESP32-S3 USB-Serial-JTAG),直接重试即可,不要急着改脚本。
- **`desktop/windows/` 被 .gitignore 整体忽略,`git add` 该目录文件必须 `-f`**,否则整次提交被拒绝(本次第一次提交就因此失败)。

## 六、遗留(未在本次修复范围)

- **固件 bonding 不持久**:`voice_ble.c` 设了 `sm_bonding=1` 但全仓库没调 `ble_store_config_init()`,bond 信息每次 deep sleep 重启即丢。Just Works 下通常可静默重配对,与本 Bug 无因果,但若将来启用加密校验需补上。
- **拿起不亮屏是预期行为**:deep sleep 唯一唤醒源是前键 GPIO11;IMU 拿起唤醒(路径 A)在 `main.c` 被 `#if 0` 有意禁用,注释注明待串口日志恢复后单独调试启用。
- Windows 侧可选加固（已于 2026-07-17 实施，`desktop/windows/src/ble_central_win.cc`）：① 订阅 `GattSession.SessionStatusChanged`，会话转 Closed 即按断连走 `HandleDeviceDisconnected` 拆除路径；② `WriteControlPayloadAsync` 检查 `GattCommunicationStatus` 与异常——`Unreachable`/写入抛 hr 异常时立即拆除会话走扫描重连，`ProtocolError`/`AccessDenied` 只记日志（链路仍活着，是 ATT 层拒绝）；③ 周期心跳探活：30s 向每个已连接会话写 `battery_status_request`（固件收到必回 `battery_status`，且不重启其待机断电计时器），90s 无任何入站 GATT 流量（`last_rx_ms`）或 `ConnectionStatus` 属性已翻 Disconnected 即判定僵尸会话拆除重连。
