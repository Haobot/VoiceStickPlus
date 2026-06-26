# IMU 加速度 X 轴驱动的显示方向自动旋转

## 背景

VoiceStick 固件使用 M5Stack StickS3 的 BMI270 IMU，当前仅用于：
1. 拾取检测（`bmi270_pickup_detected()`，基于合加速度）。
2. 调试标签显示三轴加速度（`imu_poll_timer_cb()`）。

显示方向固定在 portrait（135×240），未根据设备握持方向自动调整。用户希望在设备旋转 180° 时，画面也自动旋转 180°，以便从另一侧阅读屏幕。

## 目标

- 当加速度 X 轴为 **正** 时，画面保持当前方向（0°）。
- 当加速度 X 轴为 **负** 时，画面旋转 **180°**。
- 避免在 0g 附近频繁抖动。
- 不改变 BLE 协议、桌面端或网站。

## 方案选择

推荐方案 B：在 `main.c` 维护方向状态，通过 `ui_status` 提供的 API 切换显示方向。

| 方案 | 描述 | 优点 | 缺点 |
|---|---|---|---|
| A | 在 `imu_poll_timer_cb()` 直接调用 `lv_display_set_rotation()` | 改动最小 | IMU 解释与显示旋转耦合 |
| **B** | `main.c` 做迟滞判断，`ui_status` 封装旋转细节 | 职责清晰、可扩展 | 略多代码 |
| C | 新增独立的方向轮询定时器 | 响应最快 | 对两种状态过度设计 |

## 设计细节

### 1. 方向状态与判定（`firmware/main/main.c`）

在 `main.c` 中新增：

```c
typedef enum {
    DISPLAY_ORIENTATION_NORMAL = 0,
    DISPLAY_ORIENTATION_UPSIDE_DOWN = 1,
} display_orientation_t;

static display_orientation_t s_display_orientation = DISPLAY_ORIENTATION_NORMAL;
```

在现有的 `imu_poll_timer_cb()` 中，读取 `bmi270_read_acc_g()` 后增加方向判断：

```c
#define ORIENTATION_THRESHOLD_G  0.5f
#define ORIENTATION_CONFIRM_COUNT 2

static int s_orientation_confirm_count = 0;

static void update_orientation(float x_g) {
    display_orientation_t desired = s_display_orientation;
    if (s_display_orientation == DISPLAY_ORIENTATION_NORMAL) {
        if (x_g < -ORIENTATION_THRESHOLD_G) {
            desired = DISPLAY_ORIENTATION_UPSIDE_DOWN;
        }
    } else {
        if (x_g > ORIENTATION_THRESHOLD_G) {
            desired = DISPLAY_ORIENTATION_NORMAL;
        }
    }

    if (desired == s_display_orientation) {
        s_orientation_confirm_count = 0;
        return;
    }

    s_orientation_confirm_count++;
    if (s_orientation_confirm_count >= ORIENTATION_CONFIRM_COUNT) {
        s_display_orientation = desired;
        s_orientation_confirm_count = 0;
        ui_status_set_orientation(s_display_orientation == DISPLAY_ORIENTATION_UPSIDE_DOWN);
    }
}
```

启动时读取一次加速度初始化方向：

```c
float x_g = 0.0f;
if (bmi270_read_acc_g(&x_g, NULL, NULL) == ESP_OK) {
    s_display_orientation = (x_g < 0.0f) ? DISPLAY_ORIENTATION_UPSIDE_DOWN : DISPLAY_ORIENTATION_NORMAL;
    ui_status_set_orientation(s_display_orientation == DISPLAY_ORIENTATION_UPSIDE_DOWN);
}
```

### 2. 显示方向切换 API（`firmware/components/ui_status/`）

在 `ui_status.h` 中新增：

```c
/**
 * @brief 设置屏幕方向。
 *
 * @param upside_down true 表示旋转 180°，false 表示正常方向。
 */
void ui_status_set_orientation(bool upside_down);
```

在 `ui_status.c` 中实现：

```c
void ui_status_set_orientation(bool upside_down)
{
    if (!s_display) return;

    lv_display_rotation_t rotation = upside_down
        ? LV_DISPLAY_ROTATION_180
        : LV_DISPLAY_ROTATION_0;

    // 当前 panel 使用 mirror(true, true)。旋转 180° 时，扫描方向需要反向补偿。
    bool mirror_x = !upside_down;
    bool mirror_y = !upside_down;

    lv_display_set_rotation(s_display, rotation);
    esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
}
```

> **说明**：当前初始化代码使用 `esp_lcd_panel_mirror(panel, true, true)`。LVGL 的 software rotation 会改变传递给 flush callback 的坐标方向，因此 180° 时需要将 mirror 取反，使物理扫描方向与 LVGL 渲染方向匹配。具体组合可能需要根据实际显示效果微调。

### 3. 数据流

```text
+-------------+      read_acc_g       +------------------+
|  BMI270 IMU | --------------------> | imu_poll_timer_cb |
+-------------+                       +------------------+
                                              |
                                              v
                                     update_orientation(x_g)
                                              |
                                              v
                                     ui_status_set_orientation()
                                              |
                                              v
                                     lv_display_set_rotation()
                                     esp_lcd_panel_mirror()
```

### 4. 状态机

```
[NORMAL]
  | x_g < -0.5g (连续 2 次)
  v
[UPSIDE_DOWN]
  | x_g > +0.5g (连续 2 次)
  v
[NORMAL]
```

### 5. 边界条件

- **接近水平**：X 轴接近 0g 时保持当前方向，不会抖动。
- **启动时**：立即根据当前握持方向设置初始方向。
- **屏幕休眠**：方向检测继续运行，点亮时方向已正确。
- **只有两种方向**：本设计只支持 0° 和 180°，不考虑 90°/270°。

## 测试与验证

1. **编译验证**：`idf.py build` 在 `firmware/` 目录通过。
2. **实机验证**：
   - 当前握持方向下，屏幕正常显示，X 轴为正。
   - 旋转设备 180°，屏幕内容倒置，X 轴为负。
   - 反复旋转，切换稳定无闪烁。
3. **边界验证**：将设备缓慢旋转经过水平位置，确认不会来回跳动。

## 风险与注意事项

1. **mirror/rotation 组合需实机确认**：LVGL software rotation 与 `esp_lcd_panel_mirror` 的交互可能需要一次实际调试才能确定最佳参数。
2. **功耗**：继续使用现有 200 ms 轮询，不新增定时器，功耗影响可忽略。
3. **UI 元素位置**：旋转 180° 后，电池图标、BLE 点、状态文字都会倒置，这是预期行为。
4. **不影响协议**：本改动纯固件内部，不修改 `control_rx`/`state_tx` 帧格式。

## 相关文件

- `firmware/main/main.c`
- `firmware/components/ui_status/ui_status.c`
- `firmware/components/ui_status/include/ui_status.h`
