# hold_to_talk 连接就绪过渡期录音启动增加重试

## 背景

设备串口重启 / stale bond 触发断开重连后,Windows 客户端要重新走完整 GATT 服务发现 + 特征值订阅,才能让设备侧 `ble_ready` 置位(实测约 1.5–2 秒)。`voice_ble_is_ready()` 定义为 `s_connected && s_audio_subscribed && s_state_subscribed`(`firmware/components/voice_ble/voice_ble.c:821-824`)。

hold_to_talk 物理按键路径里,按下即启动 300ms 按住阈值定时器(`main.c:864-872`),到点在 `double_click_timer_cb` 里**只调一次** `start_recording()`(`main.c:1282-1300`)。若此刻 `ble_ready=0`,`start_recording` 返回 0 被拒(`main.c:494-498`),`s_hold_threshold_pending` 已清 false,timer 不再 fire。用户即便继续按住、之后 `ble_ready` 转 1,设备也不会补启动录音——必须松开重按。

实测复现:连接链路层 connected 后 126ms 按下按钮,hold threshold(300ms)到点时 Windows 还在订阅,`start recording denied: ble_ready=0`,本次按键作废。在 fully ready 之后按键则一切正常。

## 目标

hold_to_talk 在 hold threshold 到点因 `ble_ready=0` 被拒时,只要按钮仍按下就按短间隔重试,覆盖 Windows 订阅完成的过渡期;超时或松开则干净放弃。只对 `ble_ready=0` 这一可恢复原因重试,`ota`/`ui_state`/已在录音等不可恢复原因不重试。

## 改动范围

单文件:`firmware/main/main.c`。`voice_ble_is_ready()` 已在 `voice_ble.h:41` 声明,可直接调用,无需改组件层。

## 设计

### 新增静态状态与常量

```c
#define RECORDING_RETRY_INTERVAL_MS 100   // 重试间隔
#define RECORDING_RETRY_WINDOW_MS   2000  // 重试总窗口(覆盖订阅过渡期 ~1.5s + 余量)

static bool     s_recording_retry_pending;       // ble_ready 未就绪,按住等待重试中
static int64_t  s_recording_retry_deadline_us;   // 重试放弃时刻
```

复用现有 `s_double_click_timer`:重试期间 `s_hold_threshold_pending=false`、`s_double_click_pending=false`,该 timer 空闲,可安全复用,无需新建定时器。

### `double_click_timer_cb`(`main.c:1278-1312`)

- 在 `s_hold_threshold_pending` 分支之前插入重试分支:松开则放弃;超时则放弃;仍 `!ble_ready` 则续重试;ble_ready 则启动录音(复用现有成功路径)。
- `s_hold_threshold_pending` 分支里 `start_recording()` 返回 0 时,判 `!voice_ble_is_ready() && 按钮仍按下` → 进入重试,否则原放弃逻辑。

### `handle_primary_up`(`main.c:899-967`)

在 `s_hold_threshold_pending` 分支之后插入重试期间松开处理:清状态、停 timer、return(不发 button_up,因从未发过 button_down)。

### 断连清理(`main.c:1082-1103`)

`APP_EVENT_BLE_DISCONNECTED` 加 `s_recording_retry_pending = false;`。

## 边界与不变量

- 只对 `ble_ready=0` 重试;不可恢复原因走原放弃逻辑。
- 重试期间松开:干净退出,不发 button_up。
- 重试超时(2 秒):放弃,用户需松开重按。
- 重试期间 BLE 断开:清 flag + 停 timer;即便误 fire,`voice_ble_is_ready()` 因 `s_connected=false` 返回 false,等超时,不误启动录音。
- 双击/短按逻辑不受影响:`s_recording_retry_pending` 与 `s_double_click_pending`/`s_hold_threshold_pending` 互斥。
- click_to_talk 不受影响:不经 hold threshold 定时器。

## 验证

固件无自动化单测,靠编译 + 真机:

1. `idf.py build`(ESP-IDF v5.5.1)通过,无新警告。
2. COM17 串口烧录(WiFi 已禁用,不走 HTTP OTA;长按进 Boot、短按重启)。
3. 真机回归:
   - 目标场景:重连过渡期内按住按钮 → `ble not ready, deferring recording start (retrying)` → `recording start retry: ble ready, starting` → 正常录音 + ASR,无需松开重按。
   - 超时场景:按住 2 秒 → `recording start retry timed out`,放弃。
   - 松开场景:重试期间松开 → `button front up during recording start retry, aborting`,无异常 button_up。
   - 正常场景:fully ready 后按住,直接录音,无重试日志。
   - 双击/短按:ready 后短按仍进双击窗口,双击仍发 button_double_click。

## 实施步骤

1. 本 RFC。
2. 改 `firmware/main/main.c`(四处)。
3. `idf.py build` 验证编译。
4. COM17 串口烧录 + 真机回归。
5. 通过后提交(中文约定式提交)。
