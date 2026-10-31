> 状态：已实施（一期 r92 + 二期 r96；真机验收并 A 组）

# 传感轮询任务（sensor_poll_task）设计 — A9b

把 5 个 esp_timer 轮询回调里的 I²C 读从 esp_timer 共享任务迁到专用低优先级任务，
消除「单次 I²C 最坏 30-100ms 超时连坐所有定时器」（双击窗、OTA 看门狗、息屏、
poweroff 等同任务排队）的结构性风险。A9 已除主犯（double_click 内联重活），A9b 为余留。

## 一、现状与风险（读码实录）

| 回调 | 周期/触发 | I²C 面 | 出口 |
|---|---|---|---|
| pickup_poll_timer_cb@2408 | 定时 | bmi270_pickup_detected | queue_app_event |
| tap_poll_timer_cb@2431 | 10ms | bmi270_tap_poll | queue_app_event |
| encoder_poll_timer_cb@2935 | 定时 | read_button + read_delta（2 笔） | queue_* + encoder_status |
| air_mouse_poll_timer_cb@2989 | 定时 | bmi270_air_mouse_poll | voice_ble_send_motion（直发） |
| imu_poll_timer_cb | 定时 | bmi270 读 | motion（代码注释同先例） |

- 现有代码注释（@2987）只论证了「负载轻、不违反 timer cb 栈约束」——未覆盖最坏情形：
  I²C 总线异常时驱动超时上界 30-100ms 会卡住整个 esp_timer 任务（共享任务），同任务的
  双击定时/OTA 看门狗/息屏/关机延时全部顺延 = A9b 要解决的本质。
- 快路径守卫（s_recording / s_ota_updating / 抑制窗等）全部在回调内、先于 I²C 调用。

## 二、方案：标记-通知 + 专用任务执行

    esp_timer cb（保持原周期与守卫，但只做两件事）：
        guard 早退（原逻辑不动）
        atomic 置 pending 位（每 poller 一位）
        xTaskNotify(s_sensor_task, POLL_BIT_x, eSetBits)   ← 最坏开销 = 标记+通知 ≈ µs

    sensor_poll_task（新建，3KB 栈，优先级略高于 idle、与 app_event 同档）：
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);        // 睡到被踢
            临界区摘取 pending 位图并清零                    // 合并：一次唤醒处理全部置位
            逐位执行原 I²C + 出口体（函数体原样迁移，行为零变化）
        }

关键性质：

1. esp_timer 任务彻底轻量：不再有任何 I²C/BLE 直调，最坏连坐面消失。
2. 周期语义不变：rate 仍由 esp_timer 定；任务只决定何时执行，合并唤醒只会减少调度次数。
3. 时延：唤醒到执行 = 任务调度延迟（µs 级，优先级保证）；tap/air_mouse 可感路径不变。
4. I²C 最坏超时只卡本任务：双击窗/看门狗/息屏不再顺延。
5. 关停：Shutdown = stop 全部 poll timer → notify → 任务常驻空转（便于重启复用）。

## 三、实现前必核两点

1. voice_ble_send_motion 跨线程：现于 esp_timer 任务直发；迁任务 = 换生产者线程。
   核 voice_ble 侧 notify 是否有互斥/单生产者假设（motion 与 audio/state 多源并发点）。
   有假设 → 任务内改为入队由 voice_ble 侧发送（追加出口，改动 +1）。
2. 共享状态竞态：s_encoder_button_pressed / s_encoder_count_rem / s_tap_suppress_until_us
   的读者分布（按键路径在 app_event 任务写 suppress）——迁移后写者由 timer 任务变
   sensor 任务；suppress 为 64 位时间戳，新增任务-任务对需按 64 位原子或临界区收口。

## 四、power_log 同步 I2C——r139 判定：已被一期环覆盖，二期撤销

复核（r139，以调用方全量 grep 为准）：`power_log_note_mode` 仅 2 个调用点——
1. `power_log_refresh_mode`（全部非 S3 调用方的唯一汇聚点）已改经 4 槽环异步执行（r96 一期）；
2. `enter_power_off` 的 S3 关机条目**刻意保持同步**（组件内唯一同步 flush 点，断电前必须落盘）。

即「控制处理线程上的同步 I2C 残余 = 零」，原设想的 `sensor_i2c_sync(fn)` 请求-响应 helper
**无需实现**（避免了为一个不存在的问题加锁路）。结论：一期设计已完整覆盖，本节由候选转闭环记录。

## 五、验收

- CI 固件编译（本机无 xtensa 工具链）+ host 7/7 回归（纯逻辑面未动）。
- 真机（A 组并案）：tap/air_mouse 延迟无感变化、双击窗不抖、OTA 看门狗不因 I²C 顺延、
  拔线/掉电路径（encoder 离线补发 up）行为不变。

## 六、影响面

- 单文件：firmware/main/main.c（5 回调体迁移 + 任务创建/通知位 + Shutdown 挂接）。
- 无协议 / 无桌面端 / 无配置变更。

## 七、实施记录（与设计对照）

- 一期（r92，fd209f4/e547374/13e39e6）：5 回调改守卫+kick、原体改名 sensor_poll_X 迁执行、
  sensor_poll_task（3KB/prio6）合并唤醒摘位图；两必核结论：send_motion=audio 任务同假设、
  suppress 64 位改 __atomic×4；自审五项过 + 卫生修（(void)arg×5、缩进归位）。
- 二期（r96，604b611）：§4 power_log 承诺兑现——refresh 纯推导化、main 侧 4 槽环（portMUX 多生产者）
  + POWER_LOG 位入同任务消费、组件零改动（组件内去重兜底）；S3 直呼同步保留（唯一 flush 点@power_log.c:456）；
  附带修 r92 粘行（mux 声明缺换行，合法 C 故 CI 未报、p2 锚点断言暴露）。
- 走查（r97）：环 push/pop 均在临界区、kick 在临界区外、双路径分工=exec@371 与 S3@758 恰两处（refresh 零直呼）。
- 全量审计（r98）：main.c 全部 16 个已注册 esp_timer 回调逐一定性——去注释后 HEAVY 残留=无，
  16/16 全部轻量（kicks/queue_app_event/log 级）；首版审计曾把 double_click 注释里的 start_recording 字样
  误判为 HEAVY（假阳性），去注释复审推翻 → A9/A9b 覆盖完整性得到证明（timer 任务无 I2C/IO 残留）。
- 余：真机验收四项 + power_log 条目连续性（power_log_dump 对拍）并 A 组。