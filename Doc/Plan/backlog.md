# 遗留项 Backlog（9-22 架构评审 + 10-07 跟进评审）

> 上游：`Doc/Plan/architecture-review-and-optimization-2026-09-22.md`（缺陷编号 P0/A/B/C/D/E 沿用其 §3 清单）、`Doc/Plan/architecture-review-followup-2026-10-07.md`（N 系列与阶段划分）。
> 建立：2026-10-07（跟进评审 N4 立项动作 0.4）。**状态变更直接改本表并注明日期/commit**；评审文档只读快照，进度以本文件为准。
> 状态：`open` 未开始 · `partial` 部分完成 · `blocked` 需产品决策/真机/跨环境 · `closed` 已关闭（见关闭记录）。
> 9-22 三批推进已关闭项不在此登记（P0-1/P0-3/P0-4、A5、A18、B4/B5/B6/B10、D2/D4/D5/D8、E1/E2/E5/E6/E11 等，见上游推进记录）。
> 标注「9-22 未复核」的条目沿用评审结论，认领前先复核；标「10-07 抽查」的有本次实测行号。

## 0. 阶段 0 在办（跟进评审 §5.1）

| ID | 事项 | 状态 | 备注 |
|---|---|---|---|
| 0.1 | 跨端契约 fixtures（golden-frame 多端对拍，进 CI） | **closed（三端）** | `tests/contract/` 48 黄金样本（state 14/power_mgmt 2/ota_state 5/binary 5/control 18/ota_control 4）；Windows `TestContractFixtures`/macOS `runContractFixtureTests` 随 ctest/swift run；**固件端** `control_cmd_contract_test.c`（解析/执行分层新抽的 `control_cmd` 纯模块 + vendored cJSON 同源拷贝）经 `run_tests.py` 目标 `firmware_control_cmd` 随 host-tests 运行——control_rx 18 样本 + 反向门用例 |
| 0.2 | release-guard 脚本 + CI job | **closed** | `scripts/release_guard.py`（7 项检查）+ `test_release_guard.py`（13 用例）+ `ci.yml` release-guard job |
| 0.3 | 网站 `npm run build` 进 CI | **closed** | `ci.yml` website job（npm ci + build，lock 缓存） |
| 0.4 | 本 backlog 建立 | **closed** | 本文件 |

## 1. P0

| 编号 | 事项 | 状态 | 验收 / 备注 |
|---|---|---|---|
| P0-2 | 固件 BLE 鉴权：`control_rx`/`ota_rx` 加 `WRITE_ENC`+MITM、Secure Boot v2 + flash encryption、`test_playback` 收进调试构建 | **blocked**（产品决策 + 真机 + 一次性 eFuse） | 10-07 抽查仍开放：`voice_ble.c:774` WRITE_NO_RSP、`:1191` sm_mitm=0、sdkconfig 无启用项；最低限度先做「README 威胁模型 + 发布阻断项」 |

## 2. P1 — 固件（A 类，9-22 编号）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| A1 | ATVV ERROR 终局：冷却重试 + 状态上报 | **closed（10-08）** | 实锤三重死端：ERROR 后 `control` 全丢弃、`start` 只认 IDLE、`tick` 无 ERROR 分支 → CAPS 2s 超时/8kHz/codec 不符任一命中即**本连接内永久静默**。修复：新增 `GATEWAY_ATVV_ERROR_COOLDOWN_MS`(5s) + `error_retry_at_ms`，两处进 ERROR 时记录冷却截止；`tick` 新增 ERROR 分支——冷却到期 `begin_caps_request()`（自 `start` 抽出的公共逻辑）**自动重发 GET_CAPS**，失败再冷却再重试（限速 7s/轮）；每次失败仍产 `ACTION_ERROR` 供 `consume_actions` 记日志。回归 `test_session_error_cooldown_retry`（超时→记冷却→冷却中无动作→到期重发 0x0A→CAPS 应答恢复 READY；8kHz 路径同样记冷却） |
| A2 | ATVV 发现链终局：CHRS 退避重试 + 「ATVV 不可用」上报 | **closed（10-08，代码层）** | 实锤：服务未发现/特征不全两处直接置 `DISC_STAGE_DONE`，而 **DONE 从不被任何逻辑读取 = 纯死端** → 本次连接内永不再发现；看门狗只轮询 SVCS/CHRS。修复：两处改为回置 `DISC_STAGE_SVCS` + 刷新 `s_disc_stage_started_ms` → **复用既有发现看门狗**按 `DISC_STAGE_TIMEOUT_MS`(4s) 整链 `start_discovery`（含句柄清零）重试；新增 `atvv_unavailable_report_due()` **60s 限频**上报「ATVV 不可用：…持续重试」（原每次尝试都打会4s刷屏）。`DISC_STAGE_DONE` 仅在握手完整成功路径设置。**真机观测项**：小米侧无 ATVV 服务时的持续重试行为待真机复核 |
| A3 | STREAMING 无音频看门狗（合成 PRESS_UP 收尾） | open（需真机判定门槛） | 10-08 勘察：`tick` 确无 STREAMING 分支、`SESSION_IDLE_REHANDSHAKE_MS=0` 是**有意停用**（真机实证：周期性 TX 写/会话重置疑似打断小米输入推送状态机）→ 不能靠重握手自愈。可行设计=「STREAMING 中无入站 N 秒 → 合成 PRESS_UP + finalize」，但**门槛取决于遥控器开麦后是否连续推流**：若 VAD 门控，长静默会误收尾（切断真实口述）——**需真机确认推流连续性后再定 N**，否则有误伤风险 |
| A4 | `voice_ble` 多连接表（按 conn_handle 维护 peer，替换单值） | **closed（10-08，核心）** | 新增纯 C `voice_ble/conn_table`（宿主单测 5/5 本地 `cc` 通过，含 stale 断连回归）；`voice_ble.c` CONNECT/DISCONNECT/SUBSCRIBE 按 handle 记账——断开只清本链路、**入站归零才发 peer/connection false**（stale 断连不再误清 `current_peer`）、SUBSCRIBE 表补录替代单值覆盖；镜像派生使全部发送/门控点零改动；CONN_UPDATE/MTU 按应用链路守卫；Hub 红线三条同文更新 |
| A4b | A4 余项：**双入站广播放开**（现仍首连即停播，OS-HID+app 并存与 sdkconfig 三链路设计意图未对齐——放开涉及功耗权衡需产品决策）+ 切换器动作携带对端身份（9-22 §366 后半）+ 真机回归（入侵者连接/断开不污染切换器、双机切换 100%） | open（产品决策 + 真机） | 10-08 立项：A4 只落连接表/按 handle 语义/回调转换，广播策略未动 |
| A6 | OTA 错误路径统一 `esp_ota_abort` + `ota_clear_state`；rollback 签到延后 | **closed（10-08）** | ① **错误不清理实锤**：`ota_write_data` 的 `bad_offset`/`write_failed` 与 `ota_finish` 的 `incomplete` 发完 error 后 `s_ota.active` 残留 → `voice_ble_ota_is_active()` 恒真 → **录音/关机被永久拒绝**；新增 `ota_fail_terminal()`（send_error + `esp_ota_abort` + `ota_clear_state`）统一三处，**明确排除** `not_active`/transfer 不匹配（D7：可能属另一条在飞传输）与 `end_failed`/`set_boot_failed`（`esp_ota_end` 已消费 handle，再 abort 反而 UB——原路径已正确清理）。② **rollback 签到延后**：boot 无条件 `mark_app_valid` 使「能启动但不健康」的固件立即被背书、坏固件永不回滚 → 改一次性 esp_timer **15s 稳定运行后签到**，窗口内复位保持 PENDING_VERIFY → bootloader 回滚；定时器创建/启动失败**回退旧行为**（宁可坏固件不回滚，也不让健康固件因超时被误回滚）；手动 `ota_commit` 仍即时签到作逃生门。③ 评审三合一中「录音/OTA 互斥非原子」未含本行 → **A6b 单列** |
| A7 | app_event 关键事件处理 | **partial** | 10-07 抽查：`main.c:764` 关键标记 + 20ms 等待 + 日志已在；重试/计数上报未见 |
| A6b | 录音/OTA 互斥非原子（录音中触发 OTA begin 的 cache-disable 窗口） | open（需跨模块 API + 真机复现） | 10-08 从 A6 核出：现有门是**单向**的（`voice_ble_ota_is_active()` 阻录音），反向无门——`voice_ble.h` 不导出任何 streaming 状态，main 无法在 OTA begin 前拒绝；需新增 `voice_ble_set_streaming(bool)`（main 在 start/stop_recording 调用）+ `ota_begin` 入口拒绝，并真机复现 cache-disable 崩溃窗口确认生效 |
| A8 | audio_task 错误分支热循环 → Task WDT（失败退避 + 主动收尾） | open | 9-22 未复核 |
| A9 | esp_timer 任务当工作队列：I2C 轮询与硬件初始化移出 timer | open | 9-22 未复核 |
| A10 | 编码器降级不可恢复 + I2C 总线泄漏 | open | 9-22 未复核 |
| A11 | BMI270 加载失败仍报 present | open | 9-22 未复核 |
| A12 | `tap_enabled` 默认值与 protocol.md 相反（实现/文档取一） | **closed（10-08，实现侧对齐文档）** | 取向论证：protocol.md:311 明写 Default false、桌面 `InteractionSettings::tap_to_arrow=false` 且在**连接变更/配置变更两处逐台下发**其有效值（coordinator:129/281）→ 固件默认是唯一异类，且只在「上电到桌面首次下发」的窗口生效（开箱可能注入 Down 键）。两处改 false：静态初值 `s_tap_enabled` + `load_tap_settings_from_nvs` 缺省 `enabled=0`（仅 NVS 缺 key 生效，用户已存值不受影响）。**文档无需改** |
| A13 | `tap_sensitivity` 无范围校验 + 缺字段静默落盘 | **closed（10-08）** | 复核 B14 重构后的执行分支仍缺两道闸：① **缺字段 → 静默按默认5 落盘**，覆盖用户 NVS 既有设置；② 数值无 1..10 校验即 `bmi270_set_tap_sensitivity` + 落盘（协议契约1..10）。修复：缺字段/未知 legacy 字符串 → 告警后**显式 break**（不动当前值、不落盘）；数值夹取 1..10 再下发+落盘。桌面侧本有 `TapSensitivityClamp`（配置解析 + UI trackbar 双重夹取），固件补齐**端点防御**（控制通道对任意已连接 central 开放，不可信任桌面已夹取） |
| A14 | `ui_state.text` 超 MTU 预算：发送端零校验、固件硬截断 | open | 9-22 未复核；与 state_tx MTU 红线相关 |
| A15 | `gateway_keymap` 回执 400B 缓冲发不出 | open | 9-22 未复核 |
| A16 | 双击收尾不清 owner / click_to_talk 忽略 remote up | open | 9-22 未复核 |
| A17 | 主机无响应看门狗是死代码（timer 从不 start） | **closed（10-08）** | 确认死代码：创建/init + stop + 回调 + `APP_EVENT_HOST_RESPONSE_TIMEOUT` 处理（回 ready）四件俱全，**全仓无 start**。修复：新增 `start_host_response_timer()`（30s one-shot，失败仅告警）；武装点= `apply_app_ui_state` **进入非 ready 态**（每次状态迁移先 stop 再按需 start，窗口随主机每次响应刷新）。**30s 取值有据**：桌面端自身 `kFinalizingWatchdogTimeout=15s` 先兜住正常收尾，30s 只在主机真静默时触发；cb 的 `!s_recording` 条件保证**录音中不误伤**；录音结束必然经 device-side audio_end 进入 thinking（非 ready）重新武装，链路自洽 |
| A19 | PMIC IRQ 先 enable 后清源 + ISR 内队列失败永久 disable | **closed（10-08）** | 两缺陷均实锤：① `APP_EVENT_POWER_IRQ` 原顺序 **enable 在前**（fall-through 到 `update_battery_status()`）——清源前线为低电平（`GPIO_INTR_LOW_LEVEL`）→ ISR 立即重入（disable→队列→enable→…风暴）；② `queue_app_event_from_isr` 为 void 且 `(void)xQueueSendFromISR` **吞掉失败** → 队列满时事件丢且线已 disable，**永久失效只剩 10s 电池兜底**。修复：① 调序为**先 `update_battery_status()`（内含 `stick_s3_board_clear_power_irqs` 清 IRQ_STATUS1/2/3）再 enable**、去 fall-through；② 队列函数改返回 `pdTRUE/pdFALSE`，ISR 失败置 `s_pmic_irq_dropped`（volatile），**周期电池刷新处先清源再补臂**——刻意不在 ISR 内 enable（源未清会无限自激）；队列未创建（启动早期）不计丢弃，防告警刷屏 |
| A20 | 错误路径吞掉（ATVV TX rc / HOGP rc / power_log IO） | **closed（10-08，四处全落）** | ① **ATVV TX 写 rc**：成功/失败同一条 INFO → 失败升级 `ESP_LOGE`（GET_CAPS/MIC_CLOSE 丢写即静默卡住，原先不可辨）；② **HOGP**：`gateway_hogp_send_keyboard/consumer` 两处 `(void)` 丢返回值 → 捕获 `rc !=0` 记 WARN（链路未就绪/mbuf 耗尽可见）；③ **power_log flush**：`fseek/fwrite/fclose` 全部检查——写失败 `ESP_LOGE` 并 **`memmove` 保留未落盘条目在 RAM**（原先无条件 `s_ram_count=0` 即静默丢）、`fclose` 失败补日志（缓冲错误到此才暴露）、`wrapped` 计数移到成功写后；④ **导出短读**：`fread!=1` 区分 `ferror`（`ESP_LOGE`）与真 EOF（`ESP_LOGW`）+ voice_ble dump 侧短读补 WARN（got/want/offset/total），原先一律 `break` 零日志=FS故障静默截断桌面误判 dump 完整。**验证边界**：power_log 不在 host 覆盖面（`run_tests.py` 0 引用）→ 验证=CI 固件编译 + 本地 host 7/7 卫生 |

## 3. P1 — Windows（B 类）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| B1 | 烧录工具关窗/析构 UAF（硬同步 + 取消令牌） | **closed（10-07，代码层）** | 采用评审 shared_ptr 方案：`FlashThreadCtx` 让 worker 自持 `shared_ptr<FlashTool>`+`shared_ptr<IFlashProcessRunner>`，`FlashThreadProc` 不再触碰 `this`；关窗/析构 5s 有界等待**超时也不再 UAF**（worker 跑完 Run() 对象才析构，无挂死风险）；`flash_tool_`/`runner_` 独占转共享。验证=CI 编译；**发布前**按 AGENTS 跑 `scripts/prepare_flash_payload.ps1` 冒烟 + `Doc/Plan/windows-com-flash-tool.md` §7.2 真机清单（本机无 Windows） |
| B2 | usage tap 管道缺 OVERLAPPED → 退出挂死 / Mutex 不释放 | **closed（10-08）** | 根因逐层拆开：`CreateNamedPipeW` 的 `dwOpenMode` 只有 `PIPE_ACCESS_INBOUND` **缺 `FILE_FLAG_OVERLAPPED`** → 后面两处 `OVERLAPPED` 结构与 `ERROR_IO_PENDING` 分支全是摆设（`lpOverlapped` 被忽略，`ConnectNamedPipe`/`ReadFile` 实为阻塞调用）→ `stop_event_` 永远观察不到 → `Stop()` 卡在 `join()` → 析构/进程退出挂死。修复：① 补 `FILE_FLAG_OVERLAPPED`；② 连接/读两条等待路径在 stop 分支补 **`CancelIoEx` + `GetOverlappedResult(..., TRUE)`**——否则句柄改对后反而暴露新问题：取消是异步的，栈上 `OVERLAPPED` 提前离开作用域而内核仍会写入（UB）。回归 `TestUsageTapManagerStopBounded`（Start 后不连客户端=阻塞点，异步 Stop 5s 有界；卡住则 assert→abort **快速失败而非拖死 CI**，并刻意泄漏 manager/线程避免二次挂死） |
| B3 | 微信模式启动失败不回滚默认录音设备 | **closed（10-08）** | 时序实锤：`auto_switch`（默认麦 eConsole→CABLE）在 `renderer.Start` **之前**完成，而后者失败直接 `return false`、调用方只 `SendUiState(ready)` 不 Stop → 默认麦长期停在 CABLE（静音）；更隐蔽的是**下次会话把 CABLE 当「原设备」存回（毒化）**。修复按评审「抽恢复函数」：新增 `RestoreDefaultCaptureDevice()`（幂等：切回 + 清 `saved_default_capture_id_` + `ClearDeviceSwitchState`），**启动失败路径与 Stop 共用同一出口**。回归 `TestWechatStartFailureRollsBackDefaultCapture`（注入假 switcher/renderer + 独立 state 路径，断言两次 SetDefaultCapture 顺序、默认设备回真实麦、状态文件已清） |
| B7 | 腾讯热词同步移出 `audio_mutex_` | **closed（10-08）** | 同步整体移出 `Start` 改后台线程：**构造点预热**（`UpdateConfig`/字幕周期创建均在锁外）+ `Start` 只做幂等 `KickHotwordVocabSync`（在飞不重复、成功不重跑）；结果经 `VocabSyncState` 互斥共享，`RunWebSocket` 锁外读 `CachedVocabId()`（顺带修掉原 `cached_vocab_id_` 无锁读写）；线程按值捕获 config/词表、**不持 this**，重建/销毁无需 join（避免销毁路径再引入网络阻塞）；新增静态测试缝 `SetVocabSyncTestSeam` + 回归 `TestTencentVocabSyncOffMainThread`（注入 1200ms 慢同步，断言构造与 Start 均 <600ms、在飞期间重复 kick 幂等、完成后 `CachedVocabId` 可见） |
| B8 | 协调器 `config_` 跨线程竞争 → `shared_ptr<const AppConfig>` 原子换入 | **closed（10-07）** | `std::atomic<shared_ptr<const AppConfig>>` 快照 + `config_write_mutex_` 写侧 copy-mutate-store；109 读点转 `ConfigSnapshot()`，4 写点（UpdateConfig/配对表×2/SavePairedDeviceInfo）入互斥；压测 `TestCoordinatorConcurrentUpdateConfigStress`（4 读线程 × 300 次换入） |
| B9 | 配置写盘非原子 + 合并保存丢 `[license]` | **closed（10-07）** | `WriteTo` 原子写（同目录 `.tmp` → `MoveFileExW` REPLACE_EXISTING\|WRITE_THROUGH）+ **两条合并保存路径**（Preserving/SettingsDialog）落盘前保留磁盘 `[license]`——「谁的副本谁重取」；plain Save 写本对象 license（LicenseRuntime 激活/锚点落盘语义所依赖，首版全局重取被既有回归测试拦下后修正）；单测 `TestSaveStaleCopyKeepsLicense` + `TestLicenseConfigRoundTrip` 回归 |
| B11 | `SetLocalRefiner` 持 `audio_mutex_` 析构旧 client | open | 9-22 未复核 |
| B12 | 精修线程对象只增不减 | open | 9-22 未复核 |
| B13 | 热词候选文件双写者 + 陈旧快照覆盖「忽略」 | **closed（10-08）** | 缺陷链实锤：coordinator `RecordAndNotifyHotwordCandidates` **load-once 缓存整存**（`hotword_candidates_loaded_` 置位后永不再读盘），设置页 Dismiss/加入写入的 `dismissed` 被下一次挖掘的陈旧快照覆盖 → **用户忽略的词反复弹回**；且设置页 UI 线程与后台挖掘线程同写一文件无互斥。按评审「单一持有者或 reload-merge-save」双管齐下：miner 模块提供**唯一写入口** `Record/Consume/DismissHotwordCandidateOnDisk`（进程级互斥 + 每次从磁盘重读后改写），coordinator 缓存三成员全删、设置页两处改走入口；另把 `SaveHotwordCandidates` 改**临时文件+改名**（原 trunc 直写会读到半截 JSON、列表闪空）。回归 `TestHotwordCandidatesSingleWriter`（达阈值→忽略→**后续挖掘不回弹**→消费路径→写后即读） |
| B14 | 热词合法性四套口径 → 统一 `IsValidHotword` | **partial（10-08）** | 四处口径实测：①selector/文档权威 `hotword_select.py::is_valid_word` = 仅「无空白 + ≤10 非 ASCII / ≤30 ASCII」（**允许点号**，旗舰热词 `CLAUDE.md`/`AGENTS.md` 依赖它）；②win32 加词只查重复+长度（空格可入配置）；④腾讯另有字符集（拒点号）→①④分歧即「加进去但不生效」。**已统一（本轮）**：`ValidateHotword` = 平台权威（严格对齐 python + `HotwordRejectReason` 拒绝原因）+ `ValidateHotwordForTencent` = 权威 **再收窄**（API 字符集，**唯一有意差异**）；① 改布尔包装、② 加词入口补校验并**给出提示**（新增 `kSelectionHotwordInvalidTitle/Body` + `kStringCount` 哨兵 + EN/ZH 双表）、④ 委托腾讯口径且**被拒热词带词记录进日志**。**两轮 CI 各抓一次方向错误**：首版把腾讯字符集并入权威（破坏文档对齐与既有排序测试）；次版把权威并入③提炼（破坏多词候选）——均改正，最终 ①②④ 统一。**③ 未并入 → B14b** |
| B14b | ③ LLM 提炼的**多词候选**（"Stack Chain"）是否纳入统一口径 | open（产品决策） | 10-08 从 B14 拆出：③ 允许 ≤3 词候选，而权威/腾讯/ASR 语料（`RankHotwords` 按 `IsValidHotword` 过滤）都不支持空格 → 多词候选「提出来但用不上」。但既有提取测试 8 处断言钉死多词，且 `AppearsInSourceText` 空白容忍是生产 **candidates=0** 根因的修复。路线 A=统一拒绝（改提取测试，容忍匹配退居幕后）；路线 B=全面支持多词（腾讯 API 拒空格，实际不可行）→ 倾向 A，**待拍板** |
| B15 | F5 抑制器低级钩子内 Sleep 忙等 + 文件日志 | open | 9-22 未复核 |
| B16 | 自愈路径调 `TryUnpairAsync` 与「绝不 unpair」红线冲突 | **待核查** | 10-07：红线注释已在（`:1845`），但 `TryUnpairAsync` 调用点仍在（`:595/:1827`）——需确认调用路径均非自愈 |
| B17 | 后台线程直接 `ShowNotification`（统一 DispatchToUi） | open | 9-22 未复核 |
| B18 | 云端精修/翻译取消不回调 + 断流当成功 | open | 9-22 未复核 |
| B19 | OTA 流控记账被 progress 回调存在性门控 | open | 9-22 未复核 |
| B20 | VS 控制写未拷贝局部句柄 / 扫描状态跨线程无锁 | open | 9-22 未复核 |

## 4. P1 — 商业化/授权（C 类）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| C1 | 试用锚点/防回拨冗余到注册表/DPAPI（删 config 不重置） | open | 9-22 未复核 |
| C2 | 授权绑定改「已配对 ∪ 已连接」持久集合（设备离线不断供） | **closed（10-08）** | 新增共享纯函数 `LicenseBindingDevices(connected, paired)`（`license.cc`，归一化+去重+连接侧在前）；**两处**独立构造点同改——`LicenseRuntime::NormalizedDeviceIds`（原只用 `ConnectedDeviceIds`）与 settings 对话框状态刷新/串码激活（原用本地 `NormalizedLicenseDevices`，已删除并统一）；语义：付费用户设备关机/休眠不再被判 `kWrongBinding` 跌回试用、本地麦不再被误闸；**无安全降级**（配对要求设备曾实际到场，仍须 `machine_guid` 匹配）；回归 `TestLicenseBindingDevicesUnion`（并集/归一化/去重 + 离线配对可验签 + 未连接未配对仍拒） |
| C3 | `DateToDays` uint32 下溢 → int64 | **closed（10-08，按钳位落地）** | 纪元（2026-01-01）前日期在 `days - kLicenseEpochDays` 处为负，转 uint32 下溢成 ~42.9 亿 → ①有效年卡被 `now >= expiry` 误判过期；②`last_seen > now` 恒假致回拨检测完全失效。按评审「显式处理负值」改为**钳到 0**（语义「不早于纪元」：now=0 时年卡不过期、last_seen(>0)>now 能识别回拨并交既有宽限机制按大幅回拨处理）——存储域本就是「自 2026-01-01 的 uint32」，钳位比改 int64 更小且不动配置 schema。回归 `TestLicenseDateToDaysPreEpoch` |
| C4 | 固件 OTA manifest detached 签名 + 下载超时/上限/https-only | open | 9-22 未复核 |
| C5 | 模型在位 sha256 复核（非只比大小） | **closed（10-08）** | 在位判定收敛为「存在 + 尺寸 + SHA-256」：新增 `VerifyFileSha256`（同 TU 包装匿名空间 `Sha256HexOfFile`，大小写不敏感）+ `ModelFilePresentAndVerified`；**判定下沉到会话工作线程**（`ModelDownloadSession::Run` 内，整文件哈希不占 UI/音频线程）；删除下载向导 UI 线程的「大小相符即跳过」预筛；同尺寸损坏/被替换 → 判不在位 → `DownloadFile` 重下 → `.part` 哈希 + 原子改名覆盖修复（`DownloadFile` 无尺寸捷径，已核）。设置页状态与 `ValidateSenseVoiceModelsDir` 保持**存在性**检查——它们在 UI 刷新与音频启动路径上，不得整文件哈希（已加代码注释说明分工）。回归 `TestModelFilePresentAndVerified` |
| C6 | 固件版本比较 fail-open + 预发布后缀字典序 | **closed（10-08）** | ① `IsOlderThan` 解析失败由 `return false`（fail-open=「已是最新」，坏版本串让升级提示与最低版本门永不触发）改为按「当前较旧」；② 后缀比较由字典序改「非数字前缀 + 尾部数字」（`rc10 > rc9`，原字典序因 `'1'<'9'` 颠倒），「无后缀 > 有后缀」与前缀字典序语义保留；回归 `TestFirmwareVersionC6Robustness`，本地 clang 复刻纯逻辑全断言通过 |
| C7 | 云试用凭据下发无设备证明 + 允许 ws://→http:// | **partial（10-08，明文已堵）** | TLS-only 策略收敛为 `voice_stick_cloud_api_win.h` 内联的 `SecureHttpUrlFromWebSocketUrl` / `IsHttpsUrl`（头文件内联 → core 单测可直测，无需链接外壳层 .cc）：① api_key 申请请求不再接受 `ws://`→`http://` 与 `http://`；② ASR 长连接 `WinHttpUrlFromWebSocketUrl` 同策略（原实现同样把 `ws://` 映射成 `http://`，api_key 随明文连接暴露）；③ 响应 `url` 只放行 `https://` 再交 `ShellExecute`（防 `file://` 等被篡改执行）；顺删 asr_client_win 中失去用途的 `StartsWithScheme`。**设备证明未做 → C7b** |
| C7b | 云试用凭据下发加**设备证明**（挑战-应答） | open（需后端/产品决策） | 10-08 从 C7 拆出：客户端侧明文与 scheme 治理已随 C7 完成；设备证明需服务端协议配合，客户端先行无意义 |
| C8b | C8 余项：`hotword_candidates` 越界耦合、i18n 完整性校验只查非空（应改键集合对拍）、`voice_f5_suppressor` 等 | **closed（10-08 逐子项核销）** | ① **越界耦合 = 文件名 4 处硬编码字面量**（协调器1 + 设置页3，改名静默脱钩）→ 收敛为 `HotwordCandidatesPath(config_path)` 单一出处，4 处全部改用，测试断言路径推导；② **i18n 键集合对拍早已就位**（`check_i18n` 递归展平 + 双向差集，`test_i18n_missing_key_detected` 在 `test_release_guard.py`）——复核确认无需再做；③ `voice_f5_suppressor` = **B15**（独立行仍 open）不在本行范围 |
| C8 | 日志轮转/热词与签名 URL 明文、精修 prompt 长度上限等分组治理 | **closed（10-08，余项 C8b 已核销）** | ① **日志轮转**：`RotateLogIfTooLarge(path, 8MB)` → 改名 `<path>.old`（只留一代），在 `Log()` 写锁内调用，轮转失败静默保留原文件；② **签名 URL 脱敏**：新增 `AsrClientTencent::UrlWithoutQuery`（去 query/fragment），TASR `connecting to` 原先 `substr(0,150)` 照样会带出 `authorization=` 片段，现只记 scheme/host/path；③ **精修 prompt 封顶** `ClampRefinePrompt` 4096 字节且按 UTF-8 字符边界回退（不发半截码点），override 与热词两路出口都过；④ **热词入日志**已核：现只记 `size()` 计数，无明文，无需改。回归 `TestC8LogUrlPromptHygiene`；余项 → C8b |

## 5. P1 — 跨端/ macOS（D 类）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| D1 | macOS 消费 device_info 以外能力帧（battery/encoder/gateway/power…） | **closed（10-08 复核，评审已过时）** | 实测 macOS `handleStateEvent` 现有 **14 个 case**（Windows 全部 11 类均有对应）：`battery_status`→`statusController.setDeviceBattery`、`encoder_status`→`setDeviceEncoderPresent`、`encoder_rotate`→`handleEncoderRotate`、`gateway_key`→`handleGatewayKey` 均为**真实接线**；`gateway_status` 仅日志且**已逐平台标注原因**（注释：macOS 无直连 ATVV 路径故无需抑制）——正是评审要求的「补实现**或**逐条标注平台」。9-22 的「只处理 5 类 / grep 只命中注释」已不成立 |
| D10b | `gateway_keymap` 回执**回显到键位映射对话框**（新增状态行/表格刷新 + i18n） | open | 10-08 从 D10 拆出：协议解析与日志已就绪（Windows `StateEvent::keymap_routes`、macOS `keymapRoutes`），剩 UI 消费——需在对话框加回显控件并决定是否与 `[xiaomi.keys]` 配置比对提示不一致 |
| D3 | macOS 订阅错误处理/活性兜底/重订阅 | **partial（10-08）** | 实锤：`didUpdateNotificationStateFor` 开头 `guard xiaomiContexts` 把 **StickS3 三特征（state/audio/otaState）的订阅结果整个吞掉**（代码注释甚至自认「沿用现状不检查」）→ 订阅失败/设备侧清 CCCD **零日志零重订阅**，「显示已连接但语音静默失效」不可观测。已落：StickS3 分支前置处理（成功复位/失败记日志）+ **逐特征有限退避重订阅**（3 次 ×0.5s）+ 超限断开重建（对齐 Windows fail 语义）+ 全局/逐外设清理点同步。**主动心跳活性兜底未做 → D3b**（需对齐 Windows 心跳/僵尸判定语义，防无心跳空闲态误判重连）。本地 `swift build` + `PASSED 650/650` |
| D3b | macOS 入站**心跳活性兜底**（僵尸连接判定） | open（需对齐 Windows 语义） | 10-08 从 D3 拆出：D3 已覆盖「订阅状态变化」可恢复场景；但**无入站数据**的静默失效（CCCD 仍 on）无法感知。实现须复用 Windows 口径（心跳 5s 周期 + `kHeartbeatStaleMs=15000` + 连接期活性证明），并防「空闲无心跳」误判重连——先读固件 state_tx 心跳语义再动 |
| D6 | OTA 分块下限 `max(20,…)` 突破帧头预算（MTU 20 必 bad_offset） | **closed（10-08）** | 两端同病：`max(20, min(maxWrite-帧头, 244))` 的**下限 20 覆盖了正确的小值**——MTU 未协商（macOS `maxWrite=20`/Windows `MaxPduSize=20`）时算出 20B chunk，包长 32B/35B 超可写上限 → 固件必 `bad_offset`。收敛为协议层纯函数：macOS `BleProtocol.otaChunkSize(maxWrite:)`（= maxWrite−12，放不下返回 0）、Windows `BleProtocol::OtaChunkSizeForPdu`（= max_pdu−15 含3B ATT，≤15 返回 0），调用方对 0 **报错终止**（macOS `attMtuTooSmall` 新错误 case、Windows `FinishFirmwareUpdate`）而非硬发。两端各带单测（macOS `runOtaFlowControlTests` 追加5断言+包长自检；Windows `TestOtaChunkSizeForPdu` 含预算自检） |
| D7 | 固件 `ota_abort` transfer_id 不匹配时清状态且不 abort | **closed（10-08）** | 缺陷实锤：id 不匹配时跳过 `esp_ota_abort` 却**照旧** `ota_clear_state()` + 发 `{event:aborted}` + ABORT 回调 → 进行中的传输被悄悄废掉、`esp_ota_handle` 泄漏、桌面端拿假 aborted 误判已中止（空闲时也发假 aborted）。判定抽成纯头文件策略 `voice_ble_ota_abort_allowed`（无活动→允许幂等收尾，桌面取消流程不挂；匹配→允许；**有活动且不匹配→拒绝**），`ota_abort` 拒绝时 `ota_send_error("transfer_mismatch")` + `BLE_ATT_ERR_UNLIKELY`、**不清理不发帧**——对齐 `ota_write_data` 的既有不匹配先例。host 测试 `voice_ble_ota_policy_test`（`run_tests.py` 第 7 目标，本地 7/7） |
| D9 | 三端 `version`=1 硬编码，无协议版本协商 | open | 9-22 未复核 |
| D10 | `gateway_keymap` 回执 Windows 无解析 / macOS 无事件 | **partial（10-08）** | 复核：**macOS 半边已过时**（`case "gateway_keymap"` 已解析 `keymapRoutes` 并日志）。**Windows 半边属实**：`ParseStateEvent` 此前对 `gateway_keymap` 静默丢弃 → 回执不可观测。已落：`StateEvent::KeyRoute` + `keymap_routes` 字段、按本文件既有字符串扫描风格逐对象提取（key/route 顺序无关）、协调器新分支落日志（`gateway keymap report: k=v, …`，与 macOS NSLog 对齐）。**回显 UI → D10b** |

## 6. P1 — 发布与测试设施（E 类）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| E3 | macOS 更新链 3 处静默降级（占位公钥/sign 失败/未公证）→ fail-hard | **closed（10-08）** | 三处全落：① `build-macos.sh` 占位 `SUPublicEDKey` 在 **release 构建直接 exit1**（debug 保留告警，本地迭代不被卡）；② `sign_update` 失败**绝不把错误文本写进 `.signature`**（原 else 分支正是如此）——改为删陈旧签名防「新 zip + 旧签名」组合、**格式校验**（base64 ≥40 字符）不合规按失败、捕获退出码（原实现吞掉失败码）→ release 无签名 exit1；③ `make-dmg.sh` 公证跳过（无 `AC_PASSWORD`）在 **release 意图下须 `VOICESTICK_ALLOW_UNNOTARIZED=1` 显式放行**，否则 exit1 并给 `notarytool store-credentials` 指引（非 release 行为不变；未公证=首次右键→Open 已在 `release.md` 记载）。**本地验证**：`bash -n` 双脚本 + 从真实脚本**抽取 sign 块**跑注入用例（有效写入 / sign 失败删陈旧签名 / 畸形按失败）3 例 + 公证闸门 3 例（非 release 跳过、release 无确认拒绝、显式放行）全过 |
| E4 | `prepare_flash_payload.ps1` 供应链哈希锚点（require-hashes） | open | 9-22 未复核 |
| E7 | Windows 测试目标显式 `-UNDEBUG` 或统一 CHECK 宏 | **partial** | CI 用 Debug 构建 + ctest 已实跑全绿（2026-10-07，断言真实生效）；本地 RelWithDebInfo 假绿面仍在 |
| E8 | 固件 gateway host 测试改跨平台 CTest 并进 CI | **closed（10-08）** | `run_tests.py` 重写为跨平台（POSIX `cc -std=c11 -Wall -Wextra -Werror`；Windows 保留 vcvars+cl `/W4 /WX`），目标表 5（gateway logic/atvv/targets/switcher + voice_ble conn_table），产物进临时目录不污染源码树；顺删 `test_gateway_atvv` 死函数 `feed_zero_audio`（gcc -Werror 拦下）；CI 新增 `host-tests` job；本地 5/5 全过 |
| E9 | E2E 脚本退出码反映判定（不假 PASS） | **partial** | 9-22 已修两项（gateway_switch/run_asr_bench），其余未复核 |
| E10 | appcast 单调性（✅已随 E2 修）+ COS `firmware/latest/manifest.json` 指向国内域名 | **partial** | manifest 指向未复核 |
| E12 | `test_playback` 收进调试构建 + 补 protocol.md 文档 | open | 与 P0-2 同批 |

## 7. N 系列（10-07 跟进评审）与 P2/P3 摘要

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| N1 | macOS App target 16.8k 行零测试（状态机/配置/Ogg 下沉 Core） | open | 阶段 2 首位；测试 runner 自带 NOTE 印证缺口 |
| N2 | 跨端契约零 fixtures（golden-frame 对拍） | **closed（随 0.1）** | `tests/contract/` 48 样本**三端**进 CI（Windows/macOS/host-tests） |
| N3 | CI 缺 website/release-guard | **closed（本批）** | 两 job 已入 `ci.yml`；契约 fixtures 归 0.1 |
| N4 | 遗留项无跟踪载体 | **closed（本文件）** | 状态变更规则见头部 |
| N5 | 三份 Hub 漂移（CLAUDE/CODEBUDDY 合并行 + 多余空行） | **closed（本批）** | 已修复；`release_guard` hub 检查守护 |
| N6 | 仓库卫生（根 9 个 .bat / m0·p1 跟踪矛盾 / skills 双份 / superpowers） | open | 阶段 3 |
| N7 | 上帝对象与接口膨胀（67 virtual / 最大单文件） | open | 阶段 2 |
| N8 | `core_tests.cc` 单文件 + main() 手工注册 | open | 阶段 2 |
| N9 | 本地化三套实现无奇偶守护（网站 zh/en 已由 guard 覆盖） | open | 桌面键清单后补 |
| N10 | Doc/Plan 89 篇状态标注 + 可观测性计数 | open | 阶段 3 |
| 既往 P2 | Ogg granule 1.5× 偏差 | **closed（本批）** | 两端改由帧采样常量推导（`kFrameSamples`/`frameSamples`），guard `frame-ms` 守护；Windows 侧由 CI ctest 复核 |
| 既往 P3 | protocol.md 帧时长 60ms 写错 | **closed（本批）** | 改 40ms；guard `frame-ms` 检查文档与固件 `AUDIO_FRAME_MS` 一致 |
| 既往 P2 其余 | PCM 单缓冲、HOGP 多键、mid-press 路由、NimBLE 任务内 NVS、音频路径不看 MTU、本地 ASR O(n²)、UI 同步 sleep、字幕兜底无锁、日志轮转 | open | 9-22 未复核 |
| 既往 P3 其余 | 可观测性计数无读取、低危安全（管道 DACL/开发公钥/明文 URL）、固件死代码、电池刻度 800 vs 850 | open | 9-22 未复核 |

## 8. 关闭记录

| 日期 | 项 | 验证 |
|---|---|---|
| 2026-10-07 | 0.2 release-guard（13 单测 + 真实仓库 7/7 PASS）、0.3 网站 CI、0.4 backlog、N3、N4、N5、granule、protocol 60ms | 本地：`test_release_guard.py` 13/13、`release_guard.py` 全绿、`npm run build` ✅、macOS `swift build` + 552/552 ✅；CI 已复核见下行 |
| 2026-10-08 | **A20 关闭**：错误路径吞 rc 四处全落（ATVV TX 升 ERROR、HOGP 捕获 rc、power_log flush 全链检查+RAM 保留、导出短读区分 ferror/EOF+dump 侧短读可见） | 本地 host 7/7（卫生）+ CI 七 job（固件编译 xiaomi_atvv_client/main/power_log/voice_ble 四文件；power_log 不在 host 面，编译即其验证） |
| 2026-10-08 | **A19 关闭**：PMIC IRQ 中断链两处根因修复——先清源再 enable（消 ISR 自激风暴）+ 队列满置位由周期刷新补臂（消永久失效） | 定义/使用顺序核验（flag@132 < 队列@830 < handler@1822 < ISR@2956）+ 本地 host 7/7 + CI 七 job（固件编译 main.c） |
| 2026-10-08 | **A12 + A13 关闭（tap 域双项）**：A12 固件默认对齐 protocol.md/桌面（两处 false，用户已存 NVS 不受影响，文档无需改）；A13 缺字段/未知字符串显式 break 不落盘 + 数值夹取 1..10 再落盘 | 本地 host 7/7 + CI 七 job（固件编译 main.c 改动） |
| 2026-10-08 | **A17 关闭**：主机无响应看门狗从死代码变为实装——`apply_app_ui_state` 进非 ready 态武装 30s one-shot（桌面15s finalize看门狗先兜底，cb 的 !s_recording 防误伤） | 定义/使用顺序核验（54<564<1655）+ 本地 host 7/7 + CI 七 job（固件编译） |
| 2026-10-08 | **A6 关闭**：OTA 三处终局错误（bad_offset/write_failed/incomplete）统一 `ota_fail_terminal` 清理（原 active 残留→录音/关机永久拒绝）；rollback 签到延后 15s（坏固件不再被 boot 即刻背书，失败回退旧行为）；互斥非原子拆出 A6b | 本地 host `run_tests.py` **7/7**；CI 七 job（固件编译 voice_ble.c + main.c 改动） |
| 2026-10-08 | **A1 关闭 + A2 关闭（代码层）**：A1 ERROR 死端 → 冷却5s 自动重发 GET_CAPS（抽 `begin_caps_request` 公共）；A2 发现死端 DONE → 回置 SVCS 复用看门狗4s 整链重试 + 60s 限频「ATVV 不可用」上报；A3 勘察后留真机（推流连续性未知，防误收尾） | A1 回归 `test_session_error_cooldown_retry`；本地 `run_tests.py` **7/7**；CI 七 job（固件编译覆盖 A2） |
| 2026-10-08 | **C8b 关闭（逐子项）**：`hotword_candidates.json` 路径 4 处字面量收敛为 `HotwordCandidatesPath()` 单一出处；i18n 键集合对拍复核确认早已就位；f5 子项归 B15 | `TestHotwordCandidatesSingleWriter` 追加路径推导断言；CI 七 job（Windows ctest） |
| 2026-10-08 | **E3 关闭**：macOS 更新链三处静默降级全部 fail-hard（占位公钥/签名失败/公证跳过），签名永不落错误文本、格式校验、公证跳过需显式放行 | 本地 `bash -n` + 抽取真实脚本块注入用例（sign 3 例 + 公证 3 例）全过；bash 脚本不经 CI，本地验证为唯一证据 |
| 2026-10-08 | **D1 复核关闭 + D10（partial）**：D1 实测已由后续工作完成（macOS 14 case、真实接线、gateway_status 逐平台标注）；D10 Windows 半边补 `StateEvent::keymap_routes` 解析 + 协调器落日志（macOS 半边本已解析，评审陈旧），UI 回显拆 D10b | 新增 `TestGatewayKeymapReceiptParsing`（回执帧 + 非回执事件负例）；CI 七 job（Windows ctest） |
| 2026-10-08 | **D3（partial）**：macOS StickS3 订阅结果不再被 guard 吞掉——失败记日志 + 逐特征 3×0.5s 退避重订阅 + 超限断开重建；主动心跳留 D3b | 本地 `swift build` + `PASSED 650/650`；CI 七 job |
| 2026-10-08 | **D7 关闭**：固件 ota_abort 不匹配拒绝（不清理/不发假 aborted/不漏 handle），判定抽纯头文件策略供宿主单测 | 本地 `run_tests.py` **7/7**（新增 voice_ble_ota_policy 目标）+ CI 七 job（固件编译） |
| 2026-10-08 | **D6 关闭**：OTA 分块两端收敛为协议层纯函数（去 `max(20,…)` 下限，放不下报错不硬发），修 MTU 未协商必 `bad_offset` | macOS 本地 `swift run VoiceStickTests` + Windows CI ctest 双端单测；两端包长 ≤ 预算自检 |
| 2026-10-08 | **阶段 1 B13 关闭**：热词候选文件收敛为 miner 单一写入口（进程级互斥 + reload-merge-save），coordinator load-once 缓存删除、设置页两处改走入口、Save 原子化 | 本地断言（忽略不回弹/消费/写后即读）+ CI 七 job（Windows ctest 含新用例） |
| 2026-10-08 | **阶段 1 B14（partial）**：①②④ 统一为 `ValidateHotword` 平台权威 + `ValidateHotwordForTencent` 唯一 API 收窄（腾讯被拒热词带词入日志，加词入口补用户提示）；③多词候选拆 B14b 待产品决策 | 两轮 CI 各抓一次方向错误并改正（腾讯字符集误并权威 → 破坏排序测试；权威误并③ → 破坏多词候选与生产修复）；回归 `TestHotwordValidationUnified` 含双口径与超集对拍 |
| 2026-10-08 | **阶段 1 B3 关闭**：微信模式启动失败路径抽 `RestoreDefaultCaptureDevice()` 与 Stop 共用（默认麦不再卡 CABLE 静音、不再毒化下次会话的「原设备」） | 事件驱动回归（FakeBleCentral `on_state_event` + 注入假 switcher/renderer）；CI 七 job（Windows ctest） |
| 2026-10-08 | **阶段 1 B2 关闭**：usage tap 管道补 `FILE_FLAG_OVERLAPPED`（原 OVERLAPPED 分支形同虚设、Stop/退出挂死）+ 连接/读两路取消后等内核用完栈上 OVERLAPPED（防修复后暴露的 UB）；顺带核实 C8b 的 i18n 子项——`check_i18n` **已是键集合双向对拍**，评审「只查非空」已过时 | 新增 `TestUsageTapManagerStopBounded`（有界 5s、卡住快速失败）；CI 七 job（Windows ctest） |
| 2026-10-08 | **安全组 C8 分组治理（partial）**：日志轮转（8MB→.old 一代）+ 签名 URL 去 query 脱敏 + 精修 prompt 4096B UTF-8 边界封顶；核实热词日志只记计数 | 本地复刻三段算法全过；CI 七 job（Windows ctest 含新用例） |
| 2026-10-08 | **安全组 C7 明文堵漏（partial）**：云/ASR 链路 TLS-only——api_key 申请请求与 ASR 长连接都不再接受 `ws://`/`http://`，响应链接只放行 `https://`；策略收敛为头文件内联函数供单测直测；设备证明拆为 C7b 待后端 | 本地用**真实头文件**编译运行 13 项策略断言全过；CI 七 job（Windows ctest 含新用例） |
| 2026-10-08 | **安全组 C5 关闭**：模型在位判定由「仅比大小」改「存在 + 尺寸 + SHA-256」，判定下沉会话工作线程（UI/音频热路径不哈希）；同尺寸损坏文件不再被静默跳过，改走重下修复 | 本地核验 `sha256("hello")` 常量与 `DownloadFile` 无尺寸捷径；CI 七 job（Windows ctest 含新用例） |
| 2026-10-08 | **安全组 C2 关闭**：授权绑定候选改「已连接 ∪ 已配对」持久集合，共享 `LicenseBindingDevices` 统一 runtime 与 settings 两条构造路径——设备关机不再断供 | 本地 clang 复刻并集/归一化逻辑通过；CI 七 job（Windows ctest 含新用例） |
| 2026-10-08 | **安全组 C3/C6 关闭**：`DateToDays` 纪元前钳位（修年卡误判过期 + 回拨检测失效）；`IsOlderThan` 解析失败 fail-open→按较旧 + 后缀数字序（`rc10>rc9`） | 本地 clang 复刻纯逻辑全断言通过；CI 七 job（Windows ctest 含 2 个新用例） |
| 2026-10-08 | **阶段 1 B7 关闭**：`AsrClientTencent` 热词表同步改后台线程（构造预热 + Start 幂等 Kick），修掉「持 `audio_mutex_` 内联 HTTP 冻结状态机/按键/音频帧至超时」；测试缝与回归用例齐 | CI 七 job（Windows ctest 含新用例） |
| 2026-10-08 | **0.1 固件端 reader 落地（三端齐）**：解析/执行分层——新抽纯模块 `voice_ble/control_cmd`（control_rx 全命令族解析，零 ESP 依赖，宿主编译）+ main.c `ble_control_cb` 改 parse→execute（244 行分支体逐条保真迁移，告警文案同）+ voice_ble power_log 族共用同一解析；`control_cmd_contract_test` 消费 manifest（18 样本 + 4 反向门用例）；vendored cJSON 与 Windows third_party 同 MD5 同源拷贝；run_tests.py 第 6 目标（注入 REPO_ROOT） | 本地 6/6 host 全过；CI 七 job（host-tests 含 reader + firmware job 编译 main.c/voice_ble.c 改动） |
| 2026-10-08 | **阶段 1 E8 关闭**：`run_tests.py` 跨平台化（POSIX cc / Windows MSVC 双路线，5 目标含 voice_ble conn_table，临时目录产物）+ CI `host-tests` job 每推送运行；删 gateway 测试死函数；Hub 固件测试行 ×3 同步更新 | 本地 5/5 全过（ATVV 112/112）；CI 七 job 验证（见下行） |
| 2026-10-08 | **阶段 1 A4 深修落地（核心）**：新增纯 C `voice_ble/conn_table`（宿主单测 5/5 本地 cc -Wall -Wextra -Werror 通过，含 stale 断连回归用例）；`voice_ble.c` 三段 GAP 事件按 handle 记账 + 镜像派生（发送/门控点零改动）+ CONN_UPDATE/MTU 应用链路守卫；Hub 红线三条同文更新；余项立 A4b（广播放开产品决策 + 切换器身份 + 真机双机/入侵者回归） | 本机仅 IDF6.1 无 xtensa 工具链（项目要 v5.5.1）→ **CI firmware job 编译验证**；真机项挂 A4b |
| 2026-10-07 | **阶段 1 B1 关闭（代码层）**：`FlashThreadCtx` 生命周期解耦（worker 自持共享引用、与对话框彻底解绑），5s 有界等待从「超时即 UAF」变为「超时也安全」 | CI 六 job 编译/测试通过；真机验收与 payload 冒烟留发布前（AGENTS 约定） |
| 2026-10-07 | **阶段 1 B8/B9 关闭**：B8 配置快照化（`atomic<shared_ptr>` 原子换入 + 写互斥，109 读点/4 写点，压测 4 读线程 × 300 换入）；B9 原子写 + 合并路径保留 `[license]`。**落地三折**：①首版 Save 全局重取 license 被既有 `TestLicenseConfigRoundTrip` 拦下（LicenseRuntime 经 Save 落盘语义）→ 改「谁的副本谁重取」；②原子替换被 `Load` **自持读句柄迁移回写**顶死（MoveFileEx err=5，旧原地写共享兼容故此前不炸）→ Load 解析后即关句柄 + WriteTo 原地降级兜底；③未捕获异常静默终止靠 main 围栏 + SEH 广谱探针二分定位 | CI run `37647304192` 六 job 全绿（Windows ctest 全量含压测/契约/B9 单测通过） |
| 2026-10-07 | **0.1 契约 fixtures 两端落地**：`tests/contract/`（生成器独立手搓字节 + manifest.json 48 样本）+ Windows `TestContractFixtures`（ctest）+ macOS `runContractFixtureTests`（swift run）；键序不构成契约、expect 只取公共字段（单端缺口清单见 `tests/contract/README.md`） | 本地 macOS `swift run VoiceStickTests` **644/644**（契约 48 样本对拍行输出）；Windows 侧由 CI ctest 复核 |
| 2026-10-07 | **CI 六 job 首次全绿**（此前连续 15+ 次恒红，E6 门禁从「形同虚设」变真闸）：① macOS `BatteryMonitorWindowController.swift` `/` 跨工具链二义显式化（CI Xcode16 实锤、本地新 SDK 恰好可解）；② WinSparkle 下载源 vslavnik→vslavik（旧 fork 已 404）+ SHA256 钉死 + zip 顶层目录布局自适应；③ `settings_dialog`/`win32_app` 补 `VOICESTICK_LOCAL_ASR/REFINE_ENABLED` 守卫（ASR-OFF/REFINE-OFF 瘦构建从未被编译过，链接即失败）；④ CI 注入 sherpa-onnx 官方包（缓存 + SHA256），本地 ASR 保持默认 ON、LocalAsrClient 测试全量覆盖；⑤ **WasapiMicCapture COM 释放顺序修复**（ComPtr 在 `CoUninitialize` 之后析构属 UB——无麦克风失败路径 CI 首次执行即 ctest SEGFAULT，经进度标记 + SEH 探针二分定位） | CI run `37634784237`：macOS/Windows(build+ctest)/firmware/website/release-guard/script-tests 六 job 全部 ✓ |
