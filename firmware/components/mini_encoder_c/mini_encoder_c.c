#include "mini_encoder_c.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "stick_s3_board.h"

static const char *TAG = "mini_encoder_c";

#define MINI_ENCODER_C_ADDR 0x42
#define MINI_ENCODER_C_I2C_FREQ_HZ 100000
// 100 kHz 下 4 字节交易不足 1ms；轮询发生在 esp_timer 共享任务上下文，
// 超时取 30ms 避免阻塞拖累双击检测等其他定时器。
#define MINI_ENCODER_C_I2C_TIMEOUT_MS 30

// 寄存器（M5Stack MiniEncoderC STM32 固件）：
//   0x00 旋转计数器（int32 LE，累计值）
//   0x10 旋转增量（int32 LE，读后清零——真机验证）
//   0x20 按钮状态（1 字节，0x00=按下，0x01=释放——真机验证，低有效）
//   0x30 SK6812 LED 颜色（写 3 字节 R,G,B）
#define MINI_ENCODER_C_REG_COUNTER 0x00
#define MINI_ENCODER_C_REG_DELTA   0x10
#define MINI_ENCODER_C_REG_BUTTON  0x20
#define MINI_ENCODER_C_REG_LED     0x30

// 连续 I2C 失败达到此次数后标记 absent、由调用方停止轮询，避免日志刷屏。
#define MINI_ENCODER_C_MAX_FAIL_STREAK 10

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
// 会被 esp_timer 回调上下文（read_*）与 app_event_task 上下文（set_led）同时访问。
static _Atomic bool s_present;
static _Atomic int s_fail_streak;

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
    if (s_dev) {
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
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
    // MiniEncoderC Hat 插顶部 8pin 排针，按接线图 SDA=G8/SCL=G0；交换线序探测一次
    // 作为容错，仍失败标记 absent（优雅降级，行为与无编码器一致）。
    const struct {
        gpio_num_t sda;
        gpio_num_t scl;
    } candidates[] = {
        {STICK_S3_PIN_HAT_SDA, STICK_S3_PIN_HAT_SCL},
        {STICK_S3_PIN_HAT_SCL, STICK_S3_PIN_HAT_SDA},
    };
    esp_err_t last_err = ESP_FAIL;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        last_err = init_bus_on(candidates[i].sda, candidates[i].scl);
        if (last_err == ESP_OK) {
            last_err = probe();
        }
        if (last_err == ESP_OK) {
            s_present = true;
            // 兜底清灯：固件在录音中崩溃/看门狗复位后，编码器独立 MCU 的 LED 会残留红色。
            (void)mini_encoder_c_set_led(0, 0, 0);
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
        *pressed = (value == 0x00);
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
    // LED 写失败静默忽略（不影响录音主链路），但成败都计入统计以便降级（与读路径对称）。
    note_i2c_result(err, "set led");
    return err;
}
