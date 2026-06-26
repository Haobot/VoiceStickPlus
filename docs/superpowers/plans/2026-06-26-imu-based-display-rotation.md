# IMU 加速度 X 轴驱动的显示方向自动旋转 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 M5Stack StickS3 固件中，根据 BMI270 加速度计 X 轴正负自动切换 LVGL 显示方向（0° 或 180°）。

**Architecture:** 方向判断逻辑放在 `firmware/main/main.c` 的现有 IMU 轮询回调中，使用迟滞避免抖动；显示旋转细节封装在 `firmware/components/ui_status/ui_status.c` 的新 API `ui_status_set_orientation()` 中，内部切换 LVGL rotation 与 panel mirror。

**Tech Stack:** ESP-IDF v5.5.1, C, LVGL v9, BMI270/MPU6886 驱动, ST7789 LCD 驱动

---

## 文件变更清单

| 文件 | 操作 | 职责 |
|---|---|---|
| `firmware/components/ui_status/include/ui_status.h` | 修改 | 声明 `ui_status_set_orientation(bool upside_down)` |
| `firmware/components/ui_status/ui_status.c` | 修改 | 实现方向切换：管理 `s_display` rotation 与 `panel` mirror |
| `firmware/main/main.c` | 修改 | 添加方向状态、阈值、初始化、在 IMU 轮询中调用方向更新 |

---

## Task 1: 在 `ui_status.h` 中声明方向 API

**Files:**
- Modify: `firmware/components/ui_status/include/ui_status.h:23`

- [ ] **Step 1: 在 `ui_status_set_imu_text` 下方添加声明**

```c
// 根据设备方向设置屏幕旋转：false 为正常方向（0°），true 为倒置（180°）。
void ui_status_set_orientation(bool upside_down);
```

- [ ] **Step 2: 检查头文件编译**

Run: `cd firmware && idf.py reconfigure`
Expected: 配置成功，无新错误。

---

## Task 2: 在 `ui_status.c` 中实现 `ui_status_set_orientation()`

**Files:**
- Modify: `firmware/components/ui_status/ui_status.c`

背景：当前 `ui_status_init()` 中 panel 初始化为 `esp_lcd_panel_mirror(panel, true, true)`。LVGL 的软件旋转会改变传给 flush callback 的坐标方向，因此 180° 时需要把 mirror 取反，使物理扫描方向与 LVGL 渲染方向匹配。

- [ ] **Step 1: 在 `lvgl_flush_cb` 与 `lvgl_tick_cb` 之间（或文件顶部静态变量区）添加静态变量保存 panel 句柄**

当前 `panel` 是 `ui_status_init()` 的局部变量，需要提升为静态全局变量，以便在 `ui_status_set_orientation()` 中使用。

修改位置：`firmware/components/ui_status/ui_status.c:55-68` 静态变量区，添加：

```c
static esp_lcd_panel_handle_t s_panel;
```

- [ ] **Step 2: 在 `ui_status_init()` 中把局部 `panel` 赋值给 `s_panel`**

修改位置：`firmware/components/ui_status/ui_status.c:332-339`

原代码：
```c
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = STICK_S3_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &panel),
                        TAG, "create st7789 panel");
```

改为：
```c
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = STICK_S3_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel),
                        TAG, "create st7789 panel");
```

并将后续所有 `panel` 替换为 `s_panel`：
- `esp_lcd_panel_reset(panel)` → `esp_lcd_panel_reset(s_panel)`
- `esp_lcd_panel_init(panel)` → `esp_lcd_panel_init(s_panel)`
- `esp_lcd_panel_invert_color(panel, true)` → `esp_lcd_panel_invert_color(s_panel, true)`
- `esp_lcd_panel_mirror(panel, true, true)` → `esp_lcd_panel_mirror(s_panel, true, true)`
- `esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP)` → `esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, LCD_Y_GAP)`
- `esp_lcd_panel_disp_on_off(panel, true)` → `esp_lcd_panel_disp_on_off(s_panel, true)`
- `lv_display_set_user_data(s_display, panel)` → `lv_display_set_user_data(s_display, s_panel)`

- [ ] **Step 3: 在 `ui_status_set_imu_text()` 之后实现 `ui_status_set_orientation()`**

新增函数：

```c
void ui_status_set_orientation(bool upside_down)
{
    if (!s_display || !s_panel) {
        return;
    }

    _lock_acquire(&s_lvgl_lock);

    lv_display_rotation_t rotation = upside_down
        ? LV_DISPLAY_ROTATION_180
        : LV_DISPLAY_ROTATION_0;

    // 当前初始化使用 mirror(true,true)。LVGL 软件旋转 180° 会翻转坐标方向，
    // 因此把 mirror 取反，使物理扫描方向与渲染方向一致。
    bool mirror_x = !upside_down;
    bool mirror_y = !upside_down;

    lv_display_set_rotation(s_display, rotation);
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y));

    _lock_release(&s_lvgl_lock);
}
```

> 如果实际烧录后发现 0°/180° 方向与预期相反，可尝试把 `mirror_x`/`mirror_y` 同时改为 `upside_down` 或其他组合。

- [ ] **Step 4: 编译验证 ui_status 组件**

Run: `cd firmware && idf.py build`
Expected: 编译通过，无新增 warning。

---

## Task 3: 在 `main.c` 中添加方向状态与初始化

**Files:**
- Modify: `firmware/main/main.c`

- [ ] **Step 1: 在文件顶部添加宏定义与静态变量**

添加位置：`firmware/main/main.c:50-77`（在 `IMU_POLL_INTERVAL_US` 之后、其他静态变量之前）

```c
// 基于 IMU X 轴的显示方向自动旋转。
// 当前握持方向 X 为正时画面不变；旋转 180° 后 X 为负，画面也旋转 180°。
#define ORIENTATION_THRESHOLD_G      0.5f
#define ORIENTATION_CONFIRM_COUNT    2
```

添加位置：`firmware/main/main.c:77-78`（在其他静态变量之后，函数声明之前）

```c
typedef enum {
    DISPLAY_ORIENTATION_NORMAL = 0,
    DISPLAY_ORIENTATION_UPSIDE_DOWN = 1,
} display_orientation_t;

static display_orientation_t s_display_orientation = DISPLAY_ORIENTATION_NORMAL;
static int s_orientation_confirm_count = 0;
```

- [ ] **Step 2: 在 IMU 初始化后读取一次加速度并设置初始方向**

修改位置：`firmware/main/main.c:1401-1404`

原代码：
```c
    // BMI270 初始化在 I2C 总线就绪后；探测失败时优雅降级，不影响主流程。
    (void)bmi270_init();
    // IMU X 轴加速度常驻上屏：定时器在 ui_status 与 IMU 就绪后启动，常驻运行。
    ESP_ERROR_CHECK(init_imu_poll_timer());
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_imu_poll_timer, IMU_POLL_INTERVAL_US));
```

改为：
```c
    // BMI270 初始化在 I2C 总线就绪后；探测失败时优雅降级，不影响主流程。
    (void)bmi270_init();
    // 根据初始握持方向设置屏幕方向，避免启动后方向与实际相反。
    float initial_x_g = 0.0f;
    if (bmi270_present() && bmi270_read_acc_g(&initial_x_g, NULL, NULL) == ESP_OK) {
        s_display_orientation = (initial_x_g < 0.0f)
            ? DISPLAY_ORIENTATION_UPSIDE_DOWN
            : DISPLAY_ORIENTATION_NORMAL;
        ui_status_set_orientation(s_display_orientation == DISPLAY_ORIENTATION_UPSIDE_DOWN);
    }
    // IMU X 轴加速度常驻上屏：定时器在 ui_status 与 IMU 就绪后启动，常驻运行。
    ESP_ERROR_CHECK(init_imu_poll_timer());
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_imu_poll_timer, IMU_POLL_INTERVAL_US));
```

- [ ] **Step 3: 编译验证**

Run: `cd firmware && idf.py build`
Expected: 编译通过。

---

## Task 4: 在 IMU 轮询回调中更新方向

**Files:**
- Modify: `firmware/main/main.c:1252-1273`

- [ ] **Step 1: 在 `imu_poll_timer_cb()` 前添加方向更新辅助函数**

添加位置：`firmware/main/main.c:1248-1252`（在 `init_pickup_poll_timer` 之前或 `imu_poll_timer_cb` 之前）

```c
static void update_display_orientation(float x_g)
{
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

- [ ] **Step 2: 在 `imu_poll_timer_cb()` 中调用方向更新**

修改位置：`firmware/main/main.c:1269-1272`

原代码：
```c
    char buf[48];
    snprintf(buf, sizeof(buf), "X:%+.2f g\nY:%+.2f g\nZ:%+.2f g", x_g, y_g, z_g);
    ui_status_set_imu_text(buf);
    ESP_LOGI(TAG, "IMU acc X=%+.2f Y=%+.2f Z=%+.2f g", x_g, y_g, z_g);
}
```

改为：
```c
    update_display_orientation(x_g);

    char buf[48];
    snprintf(buf, sizeof(buf), "X:%+.2f g\nY:%+.2f g\nZ:%+.2f g", x_g, y_g, z_g);
    ui_status_set_imu_text(buf);
    ESP_LOGI(TAG, "IMU acc X=%+.2f Y=%+.2f Z=%+.2f g", x_g, y_g, z_g);
}
```

- [ ] **Step 3: 编译验证**

Run: `cd firmware && idf.py build`
Expected: 编译通过。

---

## Task 5: 实机验证

**Files:**
- 无文件变更

- [ ] **Step 1: 使用 ESP-IDF 烧录固件**

Run（将 `PORT` 替换为实际串口）：
```sh
cd firmware
idf.py -p PORT flash monitor
```

- [ ] **Step 2: 观察当前方向**

握住设备，使屏幕正常可读，确认屏幕显示正常，串口日志中 `IMU acc X=+... g` 为正。

- [ ] **Step 3: 旋转 180° 验证**

将设备旋转 180°，确认屏幕内容也旋转 180°，串口日志中 `IMU acc X=-... g` 为负。

- [ ] **Step 4: 测试边界稳定性**

缓慢旋转设备经过水平位置（X 接近 0g），确认屏幕不会来回快速跳动。

---

## Task 6: 提交变更

**Files:**
- `firmware/components/ui_status/include/ui_status.h`
- `firmware/components/ui_status/ui_status.c`
- `firmware/main/main.c`
- `docs/superpowers/specs/2026-06-26-imu-based-display-rotation-design.md`
- `docs/superpowers/plans/2026-06-26-imu-based-display-rotation.md`

- [ ] **Step 1: 添加文件**

```bash
git add firmware/components/ui_status/include/ui_status.h
git add firmware/components/ui_status/ui_status.c
git add firmware/main/main.c
git add docs/superpowers/specs/2026-06-26-imu-based-display-rotation-design.md
git add docs/superpowers/plans/2026-06-26-imu-based-display-rotation.md
```

- [ ] **Step 2: 提交**

```bash
git commit -m "feat(firmware): auto-rotate display based on IMU X-axis acceleration

- Add ui_status_set_orientation() to switch LVGL rotation and panel mirror.
- In main.c, track display orientation with hysteresis using BMI270 X-axis.
- X positive keeps normal orientation; X negative rotates 180 degrees.
- Initialize orientation at boot to match current device hold."
```

---

## 自检

- **Spec coverage:**
  - 方向判定逻辑 → Task 3 + Task 4
  - 180° 旋转实现 → Task 2
  - 迟滞避免抖动 → Task 4 的 `ORIENTATION_THRESHOLD_G` 与 `ORIENTATION_CONFIRM_COUNT`
  - 启动初始化方向 → Task 3 Step 2
  - 不改变协议 → 无 BLE 相关文件变更
- **Placeholder scan:** 无 TBD/TODO/"implement later"。
- **类型一致性：**
  - `ui_status_set_orientation(bool upside_down)` 在 Task 1 声明、Task 2 实现、Task 3/4 调用，签名一致。
  - `display_orientation_t` 与 `s_display_orientation` 在 Task 3 定义、Task 4 使用，一致。
