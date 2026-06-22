#include "bmi270.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stick_s3_board.h"

static const char *TAG = "bmi270";

#define BMI270_I2C_FREQ_HZ 400000

// BMI270 寄存器地址（据 Bosch BMI270-Sensor-API bmi2_defs.h 确认）。
#define BMI270_REG_CHIP_ID 0x00
#define BMI270_REG_ACC_X_LSB 0x0C  // ACC X/Y/Z 各 2 字节 little-endian，共 6 字节
#define BMI270_REG_PWR_CTRL 0x7D
#define BMI270_REG_PWR_CONF 0x7C
#define BMI270_REG_CMD 0x7E

#define BMI270_CHIP_ID 0x24
#define BMI270_SOFT_RESET_CMD 0xB6
#define BMI270_PWR_CTRL_ACC_EN 0x04  // bit2

// 候选 I2C 地址：SDO=GND→0x68，SDO=VDD→0x69。
static const uint8_t kCandidateAddrs[] = {0x68, 0x69};

static i2c_master_dev_handle_t s_dev;
static bool s_present;

// 轮询基线：上次采样的合加速度幅值（1g ≈ 4096 LSB @ 2g 量程，14-bit 左对齐到 16-bit）。
// 用 float 便于做平方和，避免 int 溢出。
static float s_last_acc_mag;
static bool s_has_baseline;

// 拿起判定阈值：合加速度幅值变化量（单位：LSB）。
// 2g 量程下 1g≈4096 LSB；0.3g≈1228 LSB。拿起动作瞬时加速度通常 >0.5g，
// 取 1500 LSB 兼顾灵敏度与抗桌面振动误触。待实测标定。
#define BMI270_PICKUP_DELTA_THRESHOLD 1500.0f

static esp_err_t bmi270_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 100);
}

static esp_err_t bmi270_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

static esp_err_t bmi270_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_dev, data, sizeof(data), 100);
}

// 探测 BMI270：尝试候选地址，读到 CHIP_ID=0x24 即命中。
static esp_err_t probe_device(void)
{
    i2c_master_bus_handle_t bus = stick_s3_board_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < sizeof(kCandidateAddrs) / sizeof(kCandidateAddrs[0]); ++i) {
        if (s_dev) {
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
        }

        const i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = kCandidateAddrs[i],
            .scl_speed_hz = BMI270_I2C_FREQ_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &dev_config, &s_dev);
        if (err != ESP_OK) {
            continue;
        }

        uint8_t chip_id = 0;
        err = bmi270_read_reg(BMI270_REG_CHIP_ID, &chip_id);
        if (err == ESP_OK && chip_id == BMI270_CHIP_ID) {
            ESP_LOGI(TAG, "BMI270 found at 0x%02x chip_id=0x%02x", kCandidateAddrs[i], chip_id);
            return ESP_OK;
        }
        ESP_LOGD(TAG, "probe 0x%02x -> id=0x%02x err=%s", kCandidateAddrs[i], chip_id,
                 esp_err_to_name(err));
    }

    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t bmi270_init(void)
{
    esp_err_t err = probe_device();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BMI270 not found, pickup wake disabled: %s", esp_err_to_name(err));
        s_present = false;
        return ESP_OK;  // 优雅降级，非致命
    }

    s_present = true;

    // softreset，等 2ms（BMI270 datasheet 要求）。
    (void)bmi270_write_reg(BMI270_REG_CMD, BMI270_SOFT_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(2));

    // softreset 后设备地址可能需要重新添加（部分 BMI270 复位会断开 I2C 设备层状态），
    // 重新探测一次以确保句柄有效。
    err = probe_device();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BMI270 lost after softreset: %s", esp_err_to_name(err));
        s_present = false;
        return ESP_OK;
    }

    // 关闭高级电源保存（ADV_POW_EN=0），确保 ACC 稳定输出。
    (void)bmi270_write_reg(BMI270_REG_PWR_CONF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));

    // 开 ACC（PWR_CTRL bit2）。陀螺仪不需要，省电。
    // ACC_RANGE(0x41) softreset 默认 0x00=±2g，1g≈4096 LSB @14-bit，与阈值换算一致，无需改。
    // ACC_CONF(0x40) softreset 默认 0x28=100Hz+normal BWP，足够轮询，无需改。
    (void)bmi270_write_reg(BMI270_REG_PWR_CTRL, BMI270_PWR_CTRL_ACC_EN);
    vTaskDelay(pdMS_TO_TICKS(5));

    s_has_baseline = false;
    ESP_LOGI(TAG, "BMI270 initialized (acc on, polling mode)");
    return ESP_OK;
}

bool bmi270_present(void)
{
    return s_present;
}

bool bmi270_pickup_detected(void)
{
    if (!s_present) {
        return false;
    }

    uint8_t data[6] = {0};
    esp_err_t err = bmi270_read_regs(BMI270_REG_ACC_X_LSB, data, sizeof(data));
    if (err != ESP_OK) {
        return false;
    }

    // BMI270 ACC 为 14-bit 左对齐到 16-bit little-endian，低 2 bit 为新数据标志等保留。
    // 右移 2 位取 14-bit 有符号值，再转 int16。
    int16_t x = (int16_t)(((uint16_t)data[1] << 8) | data[0]) >> 2;
    int16_t y = (int16_t)(((uint16_t)data[3] << 8) | data[2]) >> 2;
    int16_t z = (int16_t)(((uint16_t)data[5] << 8) | data[4]) >> 2;

    const float mag = sqrtf((float)x * x + (float)y * y + (float)z * z);

    if (!s_has_baseline) {
        s_last_acc_mag = mag;
        s_has_baseline = true;
        return false;
    }

    const float delta = fabsf(mag - s_last_acc_mag);
    // 更新基线（慢跟随，避免单次大动作后基线停留在峰值导致后续检测失灵）。
    s_last_acc_mag = mag;

    if (delta >= BMI270_PICKUP_DELTA_THRESHOLD) {
        ESP_LOGD(TAG, "pickup detected delta=%.0f mag=%.0f", delta, mag);
        return true;
    }
    return false;
}

esp_err_t bmi270_enable_pickup_wake(void)
{
    // 阶段 3 实现：加载 8KB config file + any-motion feature + INT1 输出。
    // 阶段 1 暂占位，BMI270 不在线时安全跳过。
    if (!s_present) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "bmi270_enable_pickup_wake not implemented yet (phase 3)");
    return ESP_OK;
}
