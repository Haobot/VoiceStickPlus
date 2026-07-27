# MiniEncoderC 编码器接入 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 M5Stack MiniEncoderC（I2C @0x42，G9/G10 自行接线）接入 StickS3：编码器按钮等价物理主键（双击 Enter、hold_to_talk/click_to_talk 零改动复用），旋转经 BLE 上报原始方向+步数、由 Windows 端映射为上/下方向键（可翻转、可开关），录音期间编码器 SK6812 亮红灯。

**Architecture:** 固件新增轮询式 `mini_encoder_c` 组件（第二路 I2C 总线，无线程无回调），main.c 用 10ms esp_timer 轮询：按钮边沿走现有 `queue_primary_down/up_event`（新 `APP_INPUT_SOURCE_ENCODER`，语义与 PHYSICAL 相同），旋转增量走新 `APP_EVENT_ENCODER_ROTATE` + `voice_ble_send_encoder_rotate`；Windows 端 `BleProtocol::ParseStateEvent` 解析新字段，`VoiceStickCoordinator::HandleEncoderRotate` 仿 `HandleTapEvent` 门控后经 `InputInjector::SendArrowUp/Down` 每 step 注入一次。

**Tech Stack:** ESP-IDF v5.5.1 (C), Windows C++20 (CMake+Ninja+MSVC 2022), assert-based core_tests

**Spec:** `docs/superpowers/specs/2026-07-27-mini-encoder-c-design.md`

**通用注意事项（每个 Task 都适用）：**

- 执行前若本计划文件尚未提交，先提交：`git add docs/superpowers/plans/2026-07-27-mini-encoder-c.md` → `docs: MiniEncoderC 编码器接入实施计划`。
- `.gitignore` 整体忽略 `desktop/windows/`，提交 Windows 端源码/测试/模板改动必须 `git add -f`（计划中每个 Windows commit 步骤已写明）。
- 固件编译验证命令（Git Bash，仓库根目录）：
  ```bash
  python scripts/idf_cli.py -c
  ```
  （若已 export ESP-IDF v5.5.1 环境，也可 `cd firmware && idf.py build`。）预期输出包含 `Project build complete`。固件没有单元测试框架，编译通过即固件 Task 的验证。
- Windows 增量构建命令（Git Bash，仓库根目录）：
  ```bash
  cmd //c '@echo off && call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build desktop\windows\build-x64'
  ```
  （若 `build-x64` 不存在，先加 `cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja`。）
- Windows 单测命令：
  ```bash
  cmd //c 'ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests'
  ```
- TDD 仅适用于 Windows core_tests：先写失败测试 → 跑 ctest 确认失败（编译错误或断言失败均可）→ 实现 → 确认通过 → commit。固件 Task 严禁发明测试框架。
- 行号均为阅读时点参考值，以实际代码为准；代码片段给出足够上下文，插入时按锚点文本定位。
- 真机验证（烧录、按编码器、看 LED）只出现在末尾 Task 11 的人工验证检查点，不进入任何 TDD 流程。

---

### Task 1: stick_s3_board 基础设施（Grove 引脚宏 + 内部 I2C 端口暴露）

**Files:**
- Modify: `firmware/components/stick_s3_board/include/stick_s3_board.h`
- Modify: `firmware/components/stick_s3_board/stick_s3_board.c`

背景：`init_i2c()`（stick_s3_board.c:136-173）按 NUM_1→NUM_0、正线序→反线序四组候选探测内部总线（ES8311/BMI270/M5PM1 共用 G47/G48），但不记录最终生效的端口号。编码器组件需要知道内部总线占了哪个端口，才能在另一个端口建第二路总线。

- [ ] **Step 1: stick_s3_board.h 加 Grove 引脚宏**

在 `#define STICK_S3_PIN_I2C_SDA 47`（约第 12 行）之后插入：

```c
// Grove 口第二路 I2C（MiniEncoderC 等外设，用户自行接线）。与内部 G47/G48 总线
// 分属不同 I2C 端口。Grove 口 5V 保持不启用（不动 PMIC BOOST_EN），外设供电由外部接线负责。
#define STICK_S3_PIN_GROVE_SDA 9
#define STICK_S3_PIN_GROVE_SCL 10
```

- [ ] **Step 2: stick_s3_board.h 声明 `stick_s3_board_i2c_port()`**

在 `i2c_master_bus_handle_t stick_s3_board_i2c_bus(void);`（约第 31 行）之后插入：

```c
// 返回内部总线（ES8311/BMI270/M5PM1）实际生效的 I2C 端口号。
// init_i2c 有 NUM_1→NUM_0 探测回退，第二路总线（如 MiniEncoderC）据此选用另一个端口。
i2c_port_t stick_s3_board_i2c_port(void);
```

- [ ] **Step 3: stick_s3_board.c 记录生效端口**

在 `static i2c_master_bus_handle_t s_i2c_bus;`（第 10 行）之后加一行：

```c
static i2c_port_t s_i2c_port = I2C_NUM_1;
```

在 `init_i2c()` 的成功分支（约第 161-165 行）中：

before:

```c
        uint8_t device_id = 0;
        last_err = pmic_read_reg(M5PM1_REG_DEVICE_ID, &device_id);
        if (last_err == ESP_OK) {
            ESP_LOGI(TAG, "I2C probe port %d sda=%d scl=%d -> ok id=0x%02x",
                     candidates[i].port, candidates[i].sda, candidates[i].scl, device_id);
            return ESP_OK;
        }
```

after:

```c
        uint8_t device_id = 0;
        last_err = pmic_read_reg(M5PM1_REG_DEVICE_ID, &device_id);
        if (last_err == ESP_OK) {
            s_i2c_port = candidates[i].port;
            ESP_LOGI(TAG, "I2C probe port %d sda=%d scl=%d -> ok id=0x%02x",
                     candidates[i].port, candidates[i].sda, candidates[i].scl, device_id);
            return ESP_OK;
        }
```

- [ ] **Step 4: stick_s3_board.c 实现 `stick_s3_board_i2c_port()`**

在 `stick_s3_board_i2c_bus()` 实现（约第 255-258 行）之后插入：

```c
i2c_port_t stick_s3_board_i2c_port(void)
{
    return s_i2c_port;
}
```

- [ ] **Step 5: 编译验证**

`python scripts/idf_cli.py -c`，预期 `Project build complete`。

- [ ] **Step 6: Commit**

```bash
git add firmware/components/stick_s3_board/include/stick_s3_board.h firmware/components/stick_s3_board/stick_s3_board.c
git commit -m "feat(firmware): stick_s3_board 暴露内部 I2C 端口并新增 Grove 引脚宏"
```

---

### Task 2: 新组件 `mini_encoder_c`

**Files:**
- Create: `firmware/components/mini_encoder_c/CMakeLists.txt`
- Create: `firmware/components/mini_encoder_c/include/mini_encoder_c.h`
- Create: `firmware/components/mini_encoder_c/mini_encoder_c.c`

不建 `idf_component.yml`：无外部托管依赖（bmi270 组件同样没有）。`firmware/components/` 下新目录会被 idf 构建系统自动发现并编译，本 Task 不需要 main 引用即可编过。

- [ ] **Step 1: 创建 CMakeLists.txt**

内容（仿 bmi270/CMakeLists.txt）：

```cmake
idf_component_register(
    SRCS "mini_encoder_c.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c stick_s3_board
)
```

- [ ] **Step 2: 创建 include/mini_encoder_c.h**

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// M5Stack MiniEncoderC（SKU U157）驱动：I2C @0x42，旋转编码器 + 按钮 + SK6812 LED。
//
// 硬件事实：
//   - 与 StickS3 HAT 口结构不兼容，用户自行接线到 Grove 口 G9/G10。
//   - 走第二路 I2C 总线（不占内部 G47/G48 总线），100 kHz。
//   - Grove 口 5V 保持不启用，编码器供电由外部接线负责。
//   - I2C 外设，不能作为深睡唤醒源。
//
// 用法与 bmi270 一致：轮询式，组件内无线程无回调，timer 由 main.c 持有。
// 探测失败（含交换 SDA/SCL 线序重试一次后仍失败）时置 absent，后续接口安全降级。

// 建第二路 I2C 总线并探测 0x42。先按 G9=SDA/G10=SCL 探测，失败交换线序重试一次。
// 必须在 stick_s3_board_init() 之后调用（依赖其内部总线端口号）。
esp_err_t mini_encoder_c_init(void);

// 编码器是否在线（init 探测成功且未因连续 I2C 失败降级）。
bool mini_encoder_c_present(void);

// 读按钮状态：寄存器 0x20，1 字节，0x01=按下。
esp_err_t mini_encoder_c_read_button(bool *pressed);

// 读旋转增量：寄存器 0x10，int32 LE，读后清零语义（真机验证；
// 若不行则改读 0x00 计数器做软件差分，见 .c 中注释）。正值=顺时针（cw）。
esp_err_t mini_encoder_c_read_delta(int32_t *delta);

// 写 SK6812 LED 颜色：寄存器 0x30，写 3 字节 R,G,B。写失败静默忽略（不影响录音主链路）。
esp_err_t mini_encoder_c_set_led(uint8_t r, uint8_t g, uint8_t b);
```

- [ ] **Step 3: 创建 mini_encoder_c.c**

```c
#include "mini_encoder_c.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "stick_s3_board.h"

static const char *TAG = "mini_encoder_c";

#define MINI_ENCODER_C_ADDR 0x42
#define MINI_ENCODER_C_I2C_FREQ_HZ 100000
#define MINI_ENCODER_C_I2C_TIMEOUT_MS 100

// 寄存器（M5Stack MiniEncoderC STM32 固件）：
//   0x00 旋转计数器（int32 LE，累计值）
//   0x10 旋转增量（int32 LE，读后清零——真机验证）
//   0x20 按钮状态（1 字节，0x01=按下）
//   0x30 SK6812 LED 颜色（写 3 字节 R,G,B）
#define MINI_ENCODER_C_REG_COUNTER 0x00
#define MINI_ENCODER_C_REG_DELTA   0x10
#define MINI_ENCODER_C_REG_BUTTON  0x20
#define MINI_ENCODER_C_REG_LED     0x30

// 连续 I2C 失败达到此次数后标记 absent、由调用方停止轮询，避免日志刷屏。
#define MINI_ENCODER_C_MAX_FAIL_STREAK 10

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int s_fail_streak;

static void note_i2c_result(esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        s_fail_streak = 0;
        return;
    }
    if (s_present) {
        ++s_fail_streak;
        ESP_LOGW(TAG, "%s failed (%d/%d): %s", what, s_fail_streak,
                 MINI_ENCODER_C_MAX_FAIL_STREAK, esp_err_to_name(err));
        if (s_fail_streak >= MINI_ENCODER_C_MAX_FAIL_STREAK) {
            s_present = false;
            ESP_LOGW(TAG, "too many I2C failures, mark encoder absent");
        }
    }
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len,
                                       MINI_ENCODER_C_I2C_TIMEOUT_MS);
}

// 第二路 I2C 总线用内部总线之外的另一个端口（ESP32-S3 只有 NUM_0/NUM_1 两个）。
static esp_err_t init_bus_on(gpio_num_t sda, gpio_num_t scl)
{
    if (s_bus) {
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
        s_dev = NULL;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = (stick_s3_board_i2c_port() == I2C_NUM_1) ? I2C_NUM_0 : I2C_NUM_1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MINI_ENCODER_C_ADDR,
        .scl_speed_hz = MINI_ENCODER_C_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev_config, &s_dev);
}

static esp_err_t probe(void)
{
    uint8_t counter[4] = {0};
    return read_regs(MINI_ENCODER_C_REG_COUNTER, counter, sizeof(counter));
}

esp_err_t mini_encoder_c_init(void)
{
    // 用户自行接线，SDA/SCL 线序可能接反：先按宏定义线序探测 0x42，
    // 失败则交换线序重试一次，仍失败标记 absent（优雅降级，行为与无编码器一致）。
    const struct {
        gpio_num_t sda;
        gpio_num_t scl;
    } candidates[] = {
        {STICK_S3_PIN_GROVE_SDA, STICK_S3_PIN_GROVE_SCL},
        {STICK_S3_PIN_GROVE_SCL, STICK_S3_PIN_GROVE_SDA},
    };
    esp_err_t last_err = ESP_FAIL;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        last_err = init_bus_on(candidates[i].sda, candidates[i].scl);
        if (last_err == ESP_OK) {
            last_err = probe();
        }
        if (last_err == ESP_OK) {
            s_present = true;
            ESP_LOGI(TAG, "MiniEncoderC found at 0x%02x (sda=%d scl=%d)",
                     MINI_ENCODER_C_ADDR, candidates[i].sda, candidates[i].scl);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "probe sda=%d scl=%d -> %s",
                 candidates[i].sda, candidates[i].scl, esp_err_to_name(last_err));
    }

    s_present = false;
    ESP_LOGW(TAG, "MiniEncoderC absent: %s", esp_err_to_name(last_err));
    return last_err;
}

bool mini_encoder_c_present(void)
{
    return s_present;
}

esp_err_t mini_encoder_c_read_button(bool *pressed)
{
    if (!pressed) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t value = 0;
    esp_err_t err = read_regs(MINI_ENCODER_C_REG_BUTTON, &value, 1);
    note_i2c_result(err, "read button");
    if (err == ESP_OK) {
        *pressed = (value == 0x01);
    }
    return err;
}

esp_err_t mini_encoder_c_read_delta(int32_t *delta)
{
    if (!delta) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    // 首选 0x10 增量寄存器（int32 LE，读后清零语义，真机验证）。
    // 编译期可见的差分回退：若真机证实 0x10 无读后清零语义，改读 0x00 计数器
    // （int32 LE），组件内保存上次读数做软件差分：
    //   static int32_t s_last_counter;
    //   int32_t counter = ...read 0x00...;
    //   *delta = counter - s_last_counter;
    //   s_last_counter = counter;
    uint8_t raw[4] = {0};
    esp_err_t err = read_regs(MINI_ENCODER_C_REG_DELTA, raw, sizeof(raw));
    note_i2c_result(err, "read delta");
    if (err == ESP_OK) {
        *delta = (int32_t)((uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
                           ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24));
    }
    return err;
}

esp_err_t mini_encoder_c_set_led(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t data[] = {MINI_ENCODER_C_REG_LED, r, g, b};
    esp_err_t err = i2c_master_transmit(s_dev, data, sizeof(data),
                                        MINI_ENCODER_C_I2C_TIMEOUT_MS);
    // LED 写失败静默忽略（不影响录音主链路），但仍计入连续失败统计以便降级。
    if (err != ESP_OK) {
        note_i2c_result(err, "set led");
    }
    return err;
}
```

- [ ] **Step 4: 编译验证**

`python scripts/idf_cli.py -c`，预期 `Project build complete`（组件被构建系统自动发现，虽尚无调用方）。

- [ ] **Step 5: Commit**

```bash
git add firmware/components/mini_encoder_c/
git commit -m "feat(firmware): 新增 mini_encoder_c 组件（第二路 I2C 探测/按钮/增量/LED）"
```

---

### Task 3: main.c 按钮接入（编码器按钮 = 物理主键）

**Files:**
- Modify: `firmware/main/main.c`
- Modify: `firmware/main/CMakeLists.txt`

关键事实（阅读源码确认）：`handle_primary_down`/`handle_primary_up` 中双击检测、300ms hold 阈值、click_to_talk、体感鼠标映射、owner 仲裁等全部分支都门控在 `source == APP_INPUT_SOURCE_PHYSICAL` 上（约 main.c:885、892、925、957、983、998、1028、1031、1054、1100、1117 共 11 处比较）。若直接把编码器事件以新 source 入队却不扩展这些比较，编码器按钮会落入「远程热键」路径：无双击检测、无 hold 阈值、按下立即录音——与规格「与现有主键完全相同的行为」矛盾。因此本 Task 必须引入 `is_local_primary_source()` 并替换全部 11 处比较（只改函数体内的 `source == APP_INPUT_SOURCE_PHYSICAL` 比较表达式，不动 `.source = APP_INPUT_SOURCE_PHYSICAL` 赋值，后者在 587/601/667/674 行，保持不变）。

- [ ] **Step 1: main.c 加 include 与输入源枚举**

在 `#include "bmi270.h"`（第 23 行）之后插入：

```c
#include "mini_encoder_c.h"
```

`app_input_source_t`（约第 138-141 行）：

before:

```c
typedef enum {
    APP_INPUT_SOURCE_PHYSICAL,
    APP_INPUT_SOURCE_REMOTE,
} app_input_source_t;
```

after:

```c
typedef enum {
    APP_INPUT_SOURCE_PHYSICAL,
    APP_INPUT_SOURCE_REMOTE,
    // MiniEncoderC 编码器按钮：交互语义与 PHYSICAL 完全相同（双击、hold 阈值、
    // click_to_talk、体感映射、owner 仲裁），仅日志里用 source 值区分来源。
    APP_INPUT_SOURCE_ENCODER,
} app_input_source_t;
```

- [ ] **Step 2: main.c 加 `is_local_primary_source()` 帮助函数**

在 `elapsed_button_ms()` 定义（约第 817-827 行）之后插入：

```c
// 编码器按钮与正面物理键在交互语义上完全等价：双击检测、hold 阈值、click_to_talk、
// 体感鼠标映射与 owner 仲裁对两者一视同仁（见 app_input_source_t 注释）。
static bool is_local_primary_source(app_input_source_t source)
{
    return source == APP_INPUT_SOURCE_PHYSICAL || source == APP_INPUT_SOURCE_ENCODER;
}
```

- [ ] **Step 3: 替换 handle_primary_down / handle_primary_up 中 11 处 source 比较**

在 `handle_primary_down`（约 874-1043）与 `handle_primary_up`（约 1045-1136）函数体内，把所有 `source == APP_INPUT_SOURCE_PHYSICAL` 比较替换为 `is_local_primary_source(source)`。共 11 处，逐一对应（以实际代码为准）：

1. `if (s_air_mouse_enabled && source == APP_INPUT_SOURCE_PHYSICAL) {`（handle_primary_down 体感分支，~885）→ `if (s_air_mouse_enabled && is_local_primary_source(source)) {`
2. `if (source == APP_INPUT_SOURCE_PHYSICAL) {`（双击窗口第二击分支，~892）→ `if (is_local_primary_source(source)) {`
3. `const primary_owner_t owner_from_source = (source == APP_INPUT_SOURCE_PHYSICAL) ? PRIMARY_OWNER_PHYSICAL : PRIMARY_OWNER_REMOTE;`（~925-926）→ `(is_local_primary_source(source)) ? ...`
4. `s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK_INSTANT && source == APP_INPUT_SOURCE_PHYSICAL`（~956-957）→ `... && is_local_primary_source(source)`
5. `s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK && source == APP_INPUT_SOURCE_PHYSICAL`（~982-983）→ 同上
6. `s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK && source == APP_INPUT_SOURCE_PHYSICAL`（~997-998）→ 同上
7. `s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK && source == APP_INPUT_SOURCE_PHYSICAL`（记录首次点击时刻，~1027-1028）→ 同上
8. `s_primary_owner = (source == APP_INPUT_SOURCE_PHYSICAL) ? PRIMARY_OWNER_PHYSICAL : PRIMARY_OWNER_REMOTE;`（~1031-1032）→ `(is_local_primary_source(source)) ? ...`
9. `if (s_air_mouse_enabled && source == APP_INPUT_SOURCE_PHYSICAL) {`（handle_primary_up 体感分支，~1054）→ `if (s_air_mouse_enabled && is_local_primary_source(source)) {`
10. `const primary_owner_t owner_from_source = (source == APP_INPUT_SOURCE_PHYSICAL) ? ...`（~1100-1101）→ `(is_local_primary_source(source)) ? ...`
11. `if (source == APP_INPUT_SOURCE_PHYSICAL && primary_duration_ms > 0 && ...`（短按进双击窗口，~1117）→ `if (is_local_primary_source(source) && primary_duration_ms > 0 && ...`

- [ ] **Step 4: 加 `primary_button_held()` 并替换三处 `gpio_get_level(STICK_S3_PIN_BUTTON_FRONT)`**

在 `poweroff_allowed_now()`（约第 246-250 行）之后插入：

```c
// 主键当前是否处于按住态：正面物理键（GPIO 低电平）或编码器按钮任一按下即视为按住。
// 双击/hold 阈值定时器与关机前按住检查共用此判定；编码器 absent 时退化为纯 GPIO 判定。
static bool primary_button_held(void)
{
    if (gpio_get_level(STICK_S3_PIN_BUTTON_FRONT) == 0) {
        return true;
    }
    if (mini_encoder_c_present()) {
        bool pressed = false;
        if (mini_encoder_c_read_button(&pressed) == ESP_OK && pressed) {
            return true;
        }
    }
    return false;
}
```

替换点 1——`enter_power_off`（约第 496-505 行）：

before:

```c
    int wait_ms = 0;
    while (gpio_get_level(wake_gpio) == 0 && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (gpio_get_level(wake_gpio) == 0) {
```

after:

```c
    int wait_ms = 0;
    while (primary_button_held() && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (primary_button_held()) {
```

（说明：编码器键不能唤醒深睡，但若进深睡时任一主键输入仍处于按住态，同样应中止关机，行为对齐物理键。第 507-508 行的日志 `gpio_get_level(wake_gpio)` 仅打前键电平，保持不变。）

替换点 2——`double_click_timer_cb` 录音重试分支（约第 1490 行）：

before: `if (gpio_get_level(STICK_S3_PIN_BUTTON_FRONT) != 0) {`
after:  `if (!primary_button_held()) {`

替换点 3——`double_click_timer_cb` hold 阈值分支（约第 1531 行）：

before: `if (gpio_get_level(STICK_S3_PIN_BUTTON_FRONT) == 0) {`
after:  `if (primary_button_held()) {`

- [ ] **Step 5: 加编码器轮询定时器（仅按钮部分；旋转在 Task 4 加入）**

常量区（在 `AIR_MOUSE_POLL_INTERVAL_US` 定义，约第 70 行之后）插入：

```c
// 编码器轮询周期。10ms=100Hz，与敲击轮询一致；按钮边沿与旋转增量都经此轮询采集。
// 仅 MiniEncoderC 在线时运行；连续 I2C 失败后组件标记 absent，回调内停表。
#define ENCODER_POLL_INTERVAL_US (10 * 1000ULL)
```

静态变量区（在 `static esp_timer_handle_t s_air_mouse_poll_timer;`，约第 106 行之后）插入：

```c
static esp_timer_handle_t s_encoder_poll_timer;
static bool s_encoder_button_pressed;
```

定时器回调与初始化（在 `set_tap_polling_enabled()` 定义结束之后、`air_mouse_poll_timer_cb` 之前，约第 1757 行处）插入：

```c
// 编码器轮询：按钮边沿 → 主键 down/up 事件（APP_INPUT_SOURCE_ENCODER，语义等价物理键）。
// 在 timer 任务上下文做 I2C 读，与 air_mouse_poll_timer_cb 同一先例（负载轻）。
// 组件连续 I2C 失败标记 absent 后停表，避免空转与日志刷屏。
static void encoder_poll_timer_cb(void *arg)
{
    (void)arg;
    if (!mini_encoder_c_present()) {
        (void)esp_timer_stop(s_encoder_poll_timer);
        return;
    }

    bool pressed = false;
    if (mini_encoder_c_read_button(&pressed) == ESP_OK &&
        pressed != s_encoder_button_pressed) {
        s_encoder_button_pressed = pressed;
        if (pressed) {
            queue_primary_down_event(APP_INPUT_SOURCE_ENCODER, 0);
        } else {
            queue_primary_up_event(APP_INPUT_SOURCE_ENCODER, 0);
        }
    }
}

static esp_err_t init_encoder_poll_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = encoder_poll_timer_cb,
        .name = "encoder_poll",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_encoder_poll_timer);
}
```

- [ ] **Step 6: app_main 初始化与启动轮询**

在 `ESP_ERROR_CHECK(init_buttons());`（约第 2170 行）前后改造为：

before:

```c
    note_activity();
    ESP_ERROR_CHECK(init_buttons());
```

after:

```c
    note_activity();
    // MiniEncoderC 编码器：探测失败优雅降级（absent），不影响主流程。
    (void)mini_encoder_c_init();
    ESP_ERROR_CHECK(init_encoder_poll_timer());
    ESP_ERROR_CHECK(init_buttons());
    // 仅在线时启动 10ms 轮询；必须在 init_buttons 之后（事件队列已创建）。
    if (mini_encoder_c_present()) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_encoder_poll_timer,
                                                 ENCODER_POLL_INTERVAL_US));
    }
```

- [ ] **Step 7: main/CMakeLists.txt 加组件依赖**

before:

```cmake
    REQUIRES stick_s3_board voice_ble audio_pipeline ui_status button esp_pm esp_timer json bmi270
```

after:

```cmake
    REQUIRES stick_s3_board voice_ble audio_pipeline ui_status button esp_pm esp_timer json bmi270 mini_encoder_c
```

- [ ] **Step 8: 编译验证**

`python scripts/idf_cli.py -c`，预期 `Project build complete`。

- [ ] **Step 9: Commit**

```bash
git add firmware/main/main.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): MiniEncoderC 按钮接入主键状态机（语义等价物理主键）"
```

---

### Task 4: main.c 旋转事件 + voice_ble 上报

**Files:**
- Modify: `firmware/components/voice_ble/include/voice_ble.h`
- Modify: `firmware/components/voice_ble/voice_ble.c`
- Modify: `firmware/main/main.c`

- [ ] **Step 1: voice_ble.h 声明 `voice_ble_send_encoder_rotate()`**

在 `esp_err_t voice_ble_send_tap(const char *kind);`（约第 59 行）之后插入：

```c
// 发送编码器旋转事件。direction 为 "cw"/"ccw"（原始物理方向，固件不做语义映射），
// steps 为该轮询窗口内同向累计格数（>=1）。
esp_err_t voice_ble_send_encoder_rotate(const char *direction, uint8_t steps);
```

- [ ] **Step 2: voice_ble.c 实现 `voice_ble_send_encoder_rotate()`**

在 `voice_ble_send_tap()` 实现（约第 1100-1107 行）之后插入：

```c
esp_err_t voice_ble_send_encoder_rotate(const char *direction, uint8_t steps)
{
    if (steps == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char json[96];
    snprintf(json, sizeof(json),
             "{\"event\":\"encoder_rotate\",\"direction\":\"%s\",\"steps\":%u}",
             direction ? direction : "cw", (unsigned)steps);
    ESP_LOGI(TAG, "encoder rotate direction=%s steps=%u",
             direction ? direction : "cw", (unsigned)steps);
    return send_state_json(json);
}
```

- [ ] **Step 3: main.c 加事件类型、payload 字段与入队函数**

`app_event_type_t`（约第 191-210 行）在 `APP_EVENT_TAP,` 之后插入：

```c
    APP_EVENT_ENCODER_ROTATE,
```

`app_event_t`（约第 212-220 行）在 `uint32_t size;` 之后插入：

```c
    // 编码器旋转事件 payload：direction 0=cw / 1=ccw（原始物理方向）；
    // steps 为该轮询窗口内同向累计格数（>=1）。仅 APP_EVENT_ENCODER_ROTATE 使用。
    uint8_t encoder_direction;
    uint8_t encoder_steps;
```

前向声明区（在 `static void queue_primary_up_event(...)` 声明，约第 229 行之后）插入：

```c
static void queue_encoder_rotate_event(int32_t delta);
```

入队函数实现（在 `queue_primary_up_event()` 实现之后、`queue_ui_state_event()` 之前，约第 632 行处）插入：

```c
// 旋转增量入队：10ms 轮询窗口内同向增量已由增量寄存器（读后清零）天然合帧，
// 一次非零读数即一帧。方向在窗口内反转的极端情况按净值方向处理（真机罕见，可接受）。
// steps 截断到 uint8_t 上限 255。
static void queue_encoder_rotate_event(int32_t delta)
{
    if (s_app_event_queue && delta != 0) {
        app_event_t event = {
            .type = APP_EVENT_ENCODER_ROTATE,
            .source = APP_INPUT_SOURCE_ENCODER,
            .encoder_direction = (delta > 0) ? 0 : 1,
            .encoder_steps = (delta > 0)
                ? (delta > 255 ? 255 : (uint8_t)delta)
                : (delta < -255 ? 255 : (uint8_t)(-delta)),
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}
```

- [ ] **Step 4: encoder_poll_timer_cb 加旋转增量读取**

Task 3 创建的 `encoder_poll_timer_cb` 整体替换为：

```c
// 编码器轮询：按钮边沿 → 主键 down/up 事件；旋转增量非零 → 旋转事件。
// 在 timer 任务上下文做 I2C 读，与 air_mouse_poll_timer_cb 同一先例（负载轻）。
// 组件连续 I2C 失败标记 absent 后停表，避免空转与日志刷屏。
static void encoder_poll_timer_cb(void *arg)
{
    (void)arg;
    if (!mini_encoder_c_present()) {
        (void)esp_timer_stop(s_encoder_poll_timer);
        return;
    }

    bool pressed = false;
    if (mini_encoder_c_read_button(&pressed) == ESP_OK &&
        pressed != s_encoder_button_pressed) {
        s_encoder_button_pressed = pressed;
        if (pressed) {
            queue_primary_down_event(APP_INPUT_SOURCE_ENCODER, 0);
        } else {
            queue_primary_up_event(APP_INPUT_SOURCE_ENCODER, 0);
        }
    }

    int32_t delta = 0;
    if (mini_encoder_c_read_delta(&delta) == ESP_OK && delta != 0) {
        queue_encoder_rotate_event(delta);
    }
}
```

- [ ] **Step 5: app_event_task 加 APP_EVENT_ENCODER_ROTATE case**

在 `case APP_EVENT_TAP:` 分支（约第 1335-1344 行）之后插入：

```c
        case APP_EVENT_ENCODER_ROTATE:
            // 编码器旋转：发送门控仿 APP_EVENT_TAP，仅空闲态上报，
            // 避免干扰语音周期与体感鼠标。方向映射在桌面端完成，固件只报原始物理事实。
            if (voice_ble_is_connected() && !s_recording && !s_ota_updating &&
                !s_air_mouse_enabled &&
                (s_app_ui_state == APP_UI_STATE_READY ||
                 s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION)) {
                const char *direction = (event.encoder_direction == 0) ? "cw" : "ccw";
                ESP_LOGI(TAG, "encoder rotate %s steps=%u, sending to host",
                         direction, (unsigned)event.encoder_steps);
                voice_ble_send_encoder_rotate(direction, event.encoder_steps);
                note_activity();
            }
            break;
```

- [ ] **Step 6: 确认 device_info 不变**

打开 `voice_ble.c` 的 `voice_ble_send_device_info()`（约第 1022-1036 行），确认 `buttons` 数组仍为 `["primary","secondary"]`——编码器行为上就是 primary，不暴露新按键角色。**本步只做核对，不改代码。**

- [ ] **Step 7: 编译验证**

`python scripts/idf_cli.py -c`，预期 `Project build complete`。

- [ ] **Step 8: Commit**

```bash
git add firmware/components/voice_ble/include/voice_ble.h firmware/components/voice_ble/voice_ble.c firmware/main/main.c
git commit -m "feat(firmware): 编码器旋转事件经 BLE 上报（encoder_rotate）"
```

---

### Task 5: 录音 LED 红灯

**Files:**
- Modify: `firmware/main/main.c`

关键事实（阅读源码确认）：`audio_pipeline_stop()` 同步等待 audio_task + tx_task 退出并排空 drain 帧后才返回（audio_pipeline.c:889-891 注释：「必须等两者退出后再返回，否则 button_up 抢跑丢尾音」），因此 `stop_recording()` 中 `audio_pipeline_stop()` 返回点即 drain 完成点。BLE 断连路径（`APP_EVENT_BLE_DISCONNECTED`）直接调 `audio_pipeline_stop()` 而不走 `stop_recording()`，也需灭灯，否则断连时 LED 会残留红色。

- [ ] **Step 1: start_recording 成功路径亮红灯**

在 `start_recording()` 中 `s_recording = true;`（约第 554 行）之后插入：

```c
    // 录音期间编码器 LED 亮红灯；LED 写失败静默忽略，不影响录音主链路。
    if (mini_encoder_c_present()) {
        (void)mini_encoder_c_set_led(255, 0, 0);
    }
```

- [ ] **Step 2: stop_recording 灭灯（drain 完成点）**

在 `stop_recording()` 中 `audio_pipeline_stop();`（约第 570 行）之后插入：

```c
    // audio_pipeline_stop 同步等 drain 完成才返回，此处即录音会话真正结束点，灭灯。
    if (mini_encoder_c_present()) {
        (void)mini_encoder_c_set_led(0, 0, 0);
    }
```

- [ ] **Step 3: BLE 断连路径灭灯**

在 `app_event_task` 的 `case APP_EVENT_BLE_DISCONNECTED:` 分支中 `audio_pipeline_stop();`（约第 1269 行）之后插入：

```c
            // 断连直接停 pipeline（不走 stop_recording），同样灭编码器 LED。
            if (mini_encoder_c_present()) {
                (void)mini_encoder_c_set_led(0, 0, 0);
            }
```

- [ ] **Step 4: 编译验证**

`python scripts/idf_cli.py -c`，预期 `Project build complete`。

- [ ] **Step 5: Commit**

```bash
git add firmware/main/main.c
git commit -m "feat(firmware): 录音期间编码器 LED 红灯指示"
```

---

### Task 6: Windows 协议解析（TDD）

**Files:**
- Modify: `desktop/windows/src/ble_protocol.h`（StateEvent 结构，约第 28-38 行）
- Modify: `desktop/windows/src/ble_protocol.cc`（ParseStateEvent，约第 147-165 行）
- Test: `desktop/windows/tests/core_tests.cc`（TestStateParsing 在约第 743 行，新测试紧随其后；main() 注册点找 `TestStateParsing();` 调用处）

- [ ] **Step 1: 写失败测试**

在 `core_tests.cc` 的 `TestStateParsing()` 函数（约第 743-753 行）之后新增：

```cpp
void TestEncoderRotateStateParsing() {
    const std::string json = "{\"event\":\"encoder_rotate\",\"direction\":\"ccw\",\"steps\":3}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "encoder_rotate");
    assert(event->direction == "ccw");
    assert(event->steps.has_value());
    assert(event->steps.value() == 3);

    // 缺字段容错：direction 为空串、steps 为 nullopt，不影响整体解析。
    const std::string sparse = "{\"event\":\"encoder_rotate\"}";
    ByteVector sparse_frame = {1, 0x10};
    AppendLe16(sparse_frame, static_cast<std::uint16_t>(sparse.size()));
    sparse_frame.insert(sparse_frame.end(), sparse.begin(), sparse.end());
    auto sparse_event = BleProtocol::ParseStateEvent(sparse_frame);
    assert(sparse_event.has_value());
    assert(sparse_event->event == "encoder_rotate");
    assert(sparse_event->direction.empty());
    assert(!sparse_event->steps.has_value());
}
```

在 `main()` 中 `TestStateParsing();` 调用之后注册：

```cpp
    TestEncoderRotateStateParsing();
```

- [ ] **Step 2: 构建并运行测试，确认失败**

跑通用注意事项中的增量构建命令。预期：编译失败，`StateEvent` 没有 `direction` / `steps` 成员。

- [ ] **Step 3: ble_protocol.h 加字段**

在 `StateEvent` 结构（约第 28-38 行）的 `std::optional<bool> battery_usb_powered;` 之后插入：

```cpp
    // 编码器旋转事件字段：direction 为 "cw"/"ccw"（固件上报的原始物理方向，
    // 语义映射在桌面端完成）；steps 为该帧内同向累计格数（>=1）。非旋转事件为空。
    std::string direction;
    std::optional<std::uint32_t> steps;
```

- [ ] **Step 4: ble_protocol.cc 解析字段**

在 `ParseStateEvent` 中 `event.battery_usb_powered = JsonBoolValue(json, "usb_powered");`（约第 162 行）之后插入：

```cpp
    event.direction = JsonStringValue(json, "direction");
    event.steps = JsonU32Value(json, "steps");
```

- [ ] **Step 5: 构建并运行测试，确认通过**

增量构建 + `ctest -R voicestick_windows_tests`，预期全部通过。

- [ ] **Step 6: Commit（注意 -f）**

```bash
git add -f desktop/windows/src/ble_protocol.h desktop/windows/src/ble_protocol.cc desktop/windows/tests/core_tests.cc
git commit -m "feat(desktop/windows): 解析 encoder_rotate 状态事件"
```

---

### Task 7: Windows InputInjector SendArrowUp（TDD）

**Files:**
- Modify: `desktop/windows/src/voice_stick_coordinator.h`（InputInjector 接口，约第 163-172 行）
- Modify: `desktop/windows/src/input_injector_win.h`（约第 15 行 SendArrowDown 声明处）
- Modify: `desktop/windows/src/input_injector_win.cc`（约第 65-68 行 SendArrowDown 实现处）
- Test: `desktop/windows/tests/core_tests.cc`（FakeInputInjector 在约第 285-308 行）

说明：`SendArrowDown` 本身没有独立单测（其覆盖来自 HandleTapEvent 链路测试），此处 TDD 的失败测试用 Fake 直连验证方式：测试调用 `input.SendArrowUp()` 断言计数，编译期失败（接口与 Fake 都没有该方法）即「失败测试」。

- [ ] **Step 1: 写失败测试**

在 `core_tests.cc` 的 `TestTapThrottleRecoversAfter500ms()` 函数（约第 1795-1816 行）之后新增：

```cpp
void TestInputInjectorArrowUpFakeWiring() {
    // Fake 直连验证 SendArrowUp 接线：协调器映射测试（Task 8）依赖此计数。
    FakeInputInjector input;
    input.SendArrowUp();
    assert(input.arrow_up_count == 1);
    assert(input.arrow_down_count == 0);
}
```

在 `main()` 中 `TestTapThrottleRecoversAfter500ms();` 调用之后注册：

```cpp
    TestInputInjectorArrowUpFakeWiring();
```

- [ ] **Step 2: 构建并运行测试，确认失败**

增量构建。预期：编译失败，`FakeInputInjector` 没有 `SendArrowUp` / `arrow_up_count` 成员。

- [ ] **Step 3: InputInjector 接口加纯虚方法**

`voice_stick_coordinator.h` 的 `InputInjector` 类中，在 `virtual void SendArrowDown() = 0;`（约第 167 行）之后插入：

```cpp
    // 注入一次上方向键，用于编码器逆时针旋转在候选/选项间向上切换。
    virtual void SendArrowUp() = 0;
```

- [ ] **Step 4: FakeInputInjector 实现**

`core_tests.cc` 的 `FakeInputInjector`（约第 285-308 行）中，在 `void SendArrowDown() override { ++arrow_down_count; }` 之后插入：

```cpp
    void SendArrowUp() override { ++arrow_up_count; }
```

在 `int arrow_down_count = 0;` 之后插入：

```cpp
    int arrow_up_count = 0;
```

- [ ] **Step 5: InputInjectorWin 声明与实现**

`input_injector_win.h` 中 `void SendArrowDown() override;`（第 15 行）之后插入：

```cpp
    void SendArrowUp() override;
```

`input_injector_win.cc` 中 `SendArrowDown()` 实现（约第 65-68 行）之后插入：

```cpp
void InputInjectorWin::SendArrowUp() {
    SendKey(VK_UP, true);
    SendKey(VK_UP, false);
}
```

- [ ] **Step 6: 构建并运行测试，确认通过**

增量构建 + `ctest -R voicestick_windows_tests`，预期全部通过（FakeInputInjector 补上纯虚实现后既有测试恢复编译）。

- [ ] **Step 7: Commit（注意 -f）**

```bash
git add -f desktop/windows/src/voice_stick_coordinator.h desktop/windows/src/input_injector_win.h desktop/windows/src/input_injector_win.cc desktop/windows/tests/core_tests.cc
git commit -m "feat(desktop/windows): InputInjector 新增 SendArrowUp"
```

---

### Task 8: Windows HandleEncoderRotate + 配置（TDD）

**Files:**
- Modify: `desktop/windows/src/app_config.h`（tap_to_arrow 字段在约第 174 行）
- Modify: `desktop/windows/src/app_config.cc`（KV 解析约第 403 行、TOML 表解析约第 596 行、序列化约第 697 行，共三处）
- Modify: `desktop/windows/src/voice_stick_coordinator.h`（HandleTapEvent 声明在约第 288 行）
- Modify: `desktop/windows/src/voice_stick_coordinator.cc`（HandleStateEvent 约第 396-417 行、HandleTapEvent 约第 937-959 行）
- Test: `desktop/windows/tests/core_tests.cc`（TapEvent helper 在约第 549-554 行；tap 链路测试在约第 1712-1816 行；main() 注册）

- [ ] **Step 1: 写失败测试——事件构造 helper 与五个测试函数**

在 `core_tests.cc` 的 `TapEvent()` helper（约第 549-554 行）之后新增：

```cpp
// 构造编码器旋转事件（固件上报的 {"event":"encoder_rotate","direction":"cw","steps":2}）。
StateEvent EncoderRotateEvent(const std::string& direction, std::uint32_t steps) {
    StateEvent state_event;
    state_event.event = "encoder_rotate";
    state_event.direction = direction;
    state_event.steps = steps;
    return state_event;
}
```

在 `TestInputInjectorArrowUpFakeWiring()`（Task 7 新增）之后新增五个测试函数：

```cpp
void TestAppConfigEncoderRoundTrip() {
    // 默认值：旋转注入开、不翻转。
    assert(AppConfig::Defaults().encoder_to_arrow == true);
    assert(AppConfig::Defaults().encoder_rotation_invert == false);

    // TOML 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_encoder_config_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.encoder_to_arrow = false;
    config.encoder_rotation_invert = true;
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.encoder_to_arrow == false);
    assert(loaded.encoder_rotation_invert == true);
    std::filesystem::remove(temp);
}

void TestEncoderRotateMapsDirectionToArrows() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 默认映射：cw→Down、ccw→Up，每个 step 注入一次。
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));

    assert(input.arrow_down_count == 2);
    assert(input.arrow_up_count == 1);
}

void TestEncoderRotateInvertFlipsDirection() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_rotation_invert = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 翻转后：cw→Up、ccw→Down。
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));

    assert(input.arrow_up_count == 2);
    assert(input.arrow_down_count == 1);
}

void TestEncoderRotateDisabledWhenConfigOff() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_to_arrow = false;  // 总开关关闭
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));

    assert(input.arrow_down_count == 0);
    assert(input.arrow_up_count == 0);
}

void TestEncoderRotateIgnoredDuringRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入录音态（hold_to_talk 默认，主键按下即录音）。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));

    // 录音中旋转应被忽略，不注入方向键，也不取消当前录音。
    assert(input.arrow_down_count == 0);
    assert(input.arrow_up_count == 0);
}
```

在 `main()` 中 `TestInputInjectorArrowUpFakeWiring();` 调用之后注册：

```cpp
    TestAppConfigEncoderRoundTrip();
    TestEncoderRotateMapsDirectionToArrows();
    TestEncoderRotateInvertFlipsDirection();
    TestEncoderRotateDisabledWhenConfigOff();
    TestEncoderRotateIgnoredDuringRecording();
```

- [ ] **Step 2: 构建并运行测试，确认失败**

增量构建。预期：编译失败，`AppConfig` 没有 `encoder_to_arrow` / `encoder_rotation_invert` 成员。

- [ ] **Step 3: app_config.h 加字段**

在 `bool tap_to_arrow = false;`（约第 174 行）及其后 `tap_sensitivity` 声明之前/之后均可，紧跟 `tap_to_arrow` 声明之后插入：

```cpp
    // MiniEncoderC 编码器：旋转注入方向键开关（顺时针→Down、逆时针→Up，每格一次）。默认开启。
    bool encoder_to_arrow = true;
    // 编码器旋转方向翻转：true 时顺时针→Up、逆时针→Down。默认关闭。
    bool encoder_rotation_invert = false;
```

- [ ] **Step 4: app_config.cc 三处接入（仿 tap_to_arrow）**

KV 解析（在 `if (key == "tap_to_arrow") ...` 行，约第 403 行之后）插入：

```cpp
    if (key == "encoder_to_arrow") config.encoder_to_arrow = BoolValue(value, config.encoder_to_arrow);
    if (key == "encoder_rotation_invert") config.encoder_rotation_invert = BoolValue(value, config.encoder_rotation_invert);
```

TOML 表解析（在 `if (auto value = TomlBool(table, "tap_to_arrow")) config.tap_to_arrow = *value;`，约第 596 行之后）插入：

```cpp
        if (auto value = TomlBool(table, "encoder_to_arrow")) config.encoder_to_arrow = *value;
        if (auto value = TomlBool(table, "encoder_rotation_invert")) config.encoder_rotation_invert = *value;
```

序列化（在 `output << "tap_to_arrow = " << ...` 行，约第 697 行之后）插入：

```cpp
    output << "encoder_to_arrow = " << (encoder_to_arrow ? "true" : "false") << "\n";
    output << "encoder_rotation_invert = " << (encoder_rotation_invert ? "true" : "false") << "\n";
```

（布尔字段无范围校验需求，默认值即校验后行为，与 tap_to_arrow 一致。）

- [ ] **Step 5: coordinator.h 声明 HandleEncoderRotate**

在 `void HandleTapEvent(const StateEvent& event, const std::string& device_id);`（约第 288 行）之后插入：

```cpp
    void HandleEncoderRotate(const StateEvent& event, const std::string& device_id);
```

- [ ] **Step 6: HandleStateEvent 加分支**

在 `HandleStateEvent`（约第 396-417 行）的 tap 分支之后：

before:

```cpp
    } else if (event.event == "tap") {
        HandleTapEvent(event, device_id);
    }
}
```

after:

```cpp
    } else if (event.event == "tap") {
        HandleTapEvent(event, device_id);
    } else if (event.event == "encoder_rotate") {
        HandleEncoderRotate(event, device_id);
    }
}
```

- [ ] **Step 7: HandleEncoderRotate 实现**

在 `HandleTapEvent` 实现（约第 937-959 行）之后插入：

```cpp
void VoiceStickCoordinator::HandleEncoderRotate(const StateEvent& event, const std::string& device_id) {
    // 总开关关闭则忽略。
    if (!config_.encoder_to_arrow) return;
    // 体感态忽略旋转，避免与体感移动/点击冲突（固件侧也有体感门控，双保险）。
    if (IsAirMouseActive(device_id)) return;
    // 录音中或识别中忽略旋转，避免干扰当前语音周期（门控与 HandleTapEvent 一致）。
    if (session_state_ == SessionState::kRecording ||
        session_state_ == SessionState::kFinalizing) {
        return;
    }
    const std::uint32_t steps = event.steps.value_or(0);
    if (steps == 0) return;
    // 方向映射：默认 cw→Down / ccw→Up；encoder_rotation_invert=true 时翻转。
    // direction 非 "ccw"（含空串/未知值）按 cw 处理，与固件只发 cw|ccw 的约定一致。
    const bool send_up = (event.direction == "ccw") != config_.encoder_rotation_invert;
    LogCoordinatorLine("encoder rotate on VS-" + device_id + " direction=" + event.direction +
                       " steps=" + std::to_string(steps) + (send_up ? " -> ArrowUp" : " -> ArrowDown"));
    for (std::uint32_t i = 0; i < steps; ++i) {
        if (send_up) {
            input_injector_->SendArrowUp();
        } else {
            input_injector_->SendArrowDown();
        }
    }
}
```

- [ ] **Step 8: 构建并运行测试，确认通过**

增量构建 + `ctest -R voicestick_windows_tests`，预期全部通过。

- [ ] **Step 9: Commit（注意 -f）**

```bash
git add -f desktop/windows/src/app_config.h desktop/windows/src/app_config.cc desktop/windows/src/voice_stick_coordinator.h desktop/windows/src/voice_stick_coordinator.cc desktop/windows/tests/core_tests.cc
git commit -m "feat(desktop/windows): 编码器旋转映射方向键与 encoder_to_arrow/encoder_rotation_invert 配置"
```

---

### Task 9: 配置示例与协议文档

**Files:**
- Modify: `desktop/windows/resources/config.template.toml`
- Modify: `desktop/macos/Config/config.example.toml`
- Modify: `Doc/Ref/protocol.md`

- [ ] **Step 1: Windows 配置模板**

`desktop/windows/resources/config.template.toml` 中，在 `tap_sensitivity = 5`（第 71 行）之后插入：

```toml
# MiniEncoderC 编码器：旋转注入方向键（顺时针→Down、逆时针→Up，每格一次）
encoder_to_arrow = true
# 旋转方向翻转（true 时顺时针→Up）
encoder_rotation_invert = false
```

- [ ] **Step 2: macOS 示例配置（仅字段示例，macOS 代码不消费）**

`desktop/macos/Config/config.example.toml` 中，在 `air_mouse_invert_y = false   # 反转 Y 轴（适配用户习惯）`（第 36 行）之后插入：

```toml
# MiniEncoderC 编码器旋转映射（仅 Windows 端消费，macOS 暂不支持）：
# encoder_to_arrow = true          # 旋转注入方向键开关（顺时针→Down、逆时针→Up）
# encoder_rotation_invert = false  # 旋转方向翻转（true 时顺时针→Up）
```

- [ ] **Step 3: protocol.md 状态事件清单加 encoder_rotate**

`Doc/Ref/protocol.md` 的状态事件清单（约第 74-82 行）中，在 `{"event":"tap","button":"double"}` 行之后插入：

```json
{"event":"encoder_rotate","direction":"cw","steps":2}
```

- [ ] **Step 4: protocol.md 补语义段**

在 `tap` 语义段（约第 100-103 行，"See `Doc/Plan/imu-tap-detection.md`." 结尾）之后插入新段落：

```markdown
`encoder_rotate` is emitted when the firmware's MiniEncoderC rotary encoder
(I2C @0x42, wired to G9/G10) accumulates rotation within one 10 ms poll window.
`direction` carries the raw physical direction (`"cw"` | `"ccw"`) and `steps`
the accumulated detents in that direction (>= 1). The firmware performs no
semantic mapping; the desktop maps rotation to arrow keys (default cw → Down,
ccw → Up, one key press per step, flippable via `encoder_rotation_invert`) and
only injects when idle — gated off while recording, recognizing, or in
air-mouse mode, mirroring the `tap_to_arrow` gating. The master switch is
`encoder_to_arrow`.
```

- [ ] **Step 5: 验证**

无需编译；用 `git diff` 核对三个文件改动。可选：跑一次 `ctest -R voicestick_windows_tests` 确认 `TestConfigTemplateSeeding` 等模板相关测试不受新增配置行影响（预期通过）。

- [ ] **Step 6: Commit（模板注意 -f）**

```bash
git add -f desktop/windows/resources/config.template.toml
git add desktop/macos/Config/config.example.toml Doc/Ref/protocol.md
git commit -m "docs: 配置示例与协议文档补充 encoder_rotate 事件与 encoder_* 配置"
```

---

### Task 10: AGENTS.md / CLAUDE.md 同步

**Files:**
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

两文件为同源副本（AGENTS.md 声明以 CLAUDE.md 为权威源、内容一致），下列每处改动在两个文件中同步执行；锚点文本两文件相同处用同一 before/after，不同处分别给出。

- [ ] **Step 1: 组件清单 5 → 6（两文件相同锚点）**

before:

```markdown
固件只负责硬件 I/O、音频编码、BLE 通信、电源管理和显示主机下发的 UI 状态，不持有桌面交互状态机。关键组件（`firmware/components/` 下共 5 个）：
```

after:

```markdown
固件只负责硬件 I/O、音频编码、BLE 通信、电源管理和显示主机下发的 UI 状态，不持有桌面交互状态机。关键组件（`firmware/components/` 下共 6 个）：
```

并在 `- `components/bmi270/`：BMI270 IMU 驱动。` 行之后插入：

```markdown
- `components/mini_encoder_c/`：MiniEncoderC 编码器驱动（I2C @0x42，G9/G10 第二路总线，按钮/旋转增量/SK6812 LED，轮询式；探测失败优雅降级）。
```

- [ ] **Step 2: 硬件映射表加行（两文件相同锚点）**

在 `| IMU | BMI270 | I2C，体感鼠标与敲击检测 |` 行之后插入：

```markdown
| MiniEncoderC 编码器 | I2C @0x42，SDA=G9 / SCL=G10（自行接线，第二路 I2C 总线） | 按钮等价主键，旋转映射方向键，录音时亮红灯；不能作为深睡唤醒源 |
```

- [ ] **Step 3: 配置节加配置项（两文件锚点不同）**

AGENTS.md——在 `- `tap_to_arrow`：IMU 敲击映射方向键开关。` 行之后插入：

```markdown
- `encoder_to_arrow` / `encoder_rotation_invert`：MiniEncoderC 编码器旋转映射方向键开关（默认 `true`）与方向翻转（默认 `false`，true 时顺时针→Up）；仅 Windows 端消费。
```

CLAUDE.md——该文件配置列表无 `tap_to_arrow` 条目（既有漂移，不在本次修复范围），在 `- `[wechat_input_method]`：...` 整行之后插入同一条目：

```markdown
- `encoder_to_arrow` / `encoder_rotation_invert`：MiniEncoderC 编码器旋转映射方向键开关（默认 `true`）与方向翻转（默认 `false`，true 时顺时针→Up）；仅 Windows 端消费。
```

- [ ] **Step 4: 注意事项加两条（两文件相同锚点，「给 Agent 的提示」列表）**

在 `- `scripts/` 下除各平台构建脚本外...` 行（两文件「给 Agent 的提示」节倒数第二条）之后、最后一条之前插入：

```markdown
- MiniEncoderC 编码器键是 I2C 外设，不能作为深睡唤醒源；主键（GPIO11）仍是唯一唤醒键。
- Grove 口 5V 保持不启用（固件不动 PMIC BOOST_EN），MiniEncoderC 供电由外部接线负责。
```

- [ ] **Step 5: 核对两文件一致性**

`git diff AGENTS.md CLAUDE.md` 人工核对，除两文件自身既有差异（如 CLAUDE.md 缺 tap_to_arrow 条目、AGENTS.md 头部的同源声明）外，本次新增内容应一致。

- [ ] **Step 6: Commit**

```bash
git add AGENTS.md CLAUDE.md
git commit -m "docs(AGENTS.md): 同步 MiniEncoderC 硬件映射/配置项/组件清单与接线注意事项"
```

---

### Task 11: 整体验证检查点（非代码 Task，不 commit）

- [ ] **Step 1: 固件全量编译**

```bash
python scripts/idf_cli.py -c
```

预期 `Project build complete`。

- [ ] **Step 2: Windows 全量构建 + 全部测试**

```bash
cmd //c '@echo off && call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build desktop\windows\build-x64'
cmd //c 'ctest --test-dir desktop\windows\build-x64 --output-on-failure'
```

预期 `voicestick_windows_tests` 全绿（`voicestick_integration_tests` 无 key 时 SKIP 属正常）。

- [ ] **Step 3: 人工真机验证检查点——无编码器回归（烧录 Task 1-5 构建的固件）**

烧录方式参照 `.agents/skills/sticks3-flash-ota`（优先 BLE OTA，回退串口）。不接编码器时逐项确认：

- 启动日志出现 `MiniEncoderC absent`，无连续错误刷屏。
- 主键：按住录音 / 松开结束、双击注入 Enter、click_to_talk 模式——与当前版本行为一致。
- 侧键单击进/出体感鼠标、双击恢复上一次输入确认。
- 敲击映射方向键（tap_to_arrow 开启时）、体感鼠标移动与左键——不变。
- 空闲自动熄屏、关机与主键唤醒——不变。

- [ ] **Step 4: 人工真机验证检查点——有编码器功能**

接好 MiniEncoderC（G9/G10，外部供电）后逐项确认：

- 启动日志出现 `MiniEncoderC found at 0x42`；若接线反了，日志显示交换线序后成功。
- 编码器按钮按住录音 / 松开结束，与主键一致；双击注入 Enter；click_to_talk 模式首击启动、再击停止。
- 空闲态旋转：Windows 焦点应用收到方向键（默认顺时针 Down、逆时针 Up），每格一次；录音中、识别中、体感鼠标态下旋转不注入。
- `encoder_rotation_invert = true`（改 `%APPDATA%\VoiceStick\config.toml` 重启应用）后方向翻转。
- `encoder_to_arrow = false` 后旋转完全不注入。
- 录音期间编码器 LED 亮红灯，松开 drain 完成后熄灭；录音中 BLE 断连也熄灭。
