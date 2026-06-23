#include "bmi270.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stick_s3_board.h"
#include "bmi270_config_file.h"

static const char *TAG = "bmi270";

#define BMI270_I2C_FREQ_HZ 400000

// BMI270 寄存器地址（据 Bosch BMI270-Sensor-API bmi2_defs.h 确认）。
#define BMI270_REG_CHIP_ID 0x00
#define BMI270_REG_ACC_X_LSB 0x0C  // ACC X/Y/Z 各 2 字节 little-endian
// MPU6886 寄存器（兼容 MPU6050 布局）。
#define MPU6886_REG_WHO_AM_I 0x75
#define MPU6886_REG_ACC_XOUT_H 0x3B  // ACC X/Y/Z 各 2 字节 big-endian
#define MPU6886_REG_PWR_MGMT_1 0x6B
#define MPU6886_REG_PWR_MGMT_2 0x6C
#define BMI270_REG_INTERNAL_STATUS 0x21
#define BMI270_REG_FEAT_PAGE 0x2F
#define BMI270_REG_FEATURES 0x30
#define BMI270_REG_INIT_ADDR_0 0x5B
#define BMI270_REG_INIT_ADDR_1 0x5C
#define BMI270_REG_INIT_DATA 0x5E
#define BMI270_REG_INIT_CTRL 0x59
#define BMI270_REG_INT1_IO_CTRL 0x53
#define BMI270_REG_INT_LATCH 0x55
#define BMI270_REG_INT1_MAP_FEAT 0x56
#define BMI270_REG_PWR_CTRL 0x7D
#define BMI270_REG_PWR_CONF 0x7C
#define BMI270_REG_CMD 0x7E

#define BMI270_CHIP_ID 0x24
#define BMI270_SOFT_RESET_CMD 0xB6
#define BMI270_PWR_CTRL_ACC_EN 0x04  // bit2
#define MPU6886_WHO_AM_I_VAL 0x70   // MPU6886 的 WHO_AM_I 返回值

// IMU 类型枚举
typedef enum {
    IMU_NONE,
    IMU_BMI270,
    IMU_MPU6886,
} imu_type_t;
#define BMI270_INIT_CTRL_LOAD_EN 0x01
#define BMI270_INTERNAL_STATUS_INIT_OK 0x01
// INT1_IO_CTRL：bit3=OUTPUT_EN(0x08)，bit1=LEVEL(0=active low)。push-pull+active low+out_en=0x08。
#define BMI270_INT1_IO_PUSH_PULL_ACTIVE_LOW 0x08
// INT1_MAP_FEAT bit6=any-motion(0x40)。
#define BMI270_INT1_MAP_ANY_MOTION 0x40
// any-motion feature 在 page 1，start_addr=0x0C，16 字节布局。
#define BMI270_ANY_MOT_PAGE 1
#define BMI270_ANY_MOT_OFFSET 0x0C
#define BMI270_FEAT_BURST_BYTES 16
// any-motion 字段：duration=0x01(20ms), xyz_sel=0xE0, threshold=0x68(50mg), enable bit7=0x80。
#define BMI270_ANY_MOT_DURATION_LSB 0x01
#define BMI270_ANY_MOT_DURATION_XYZ 0xE0
#define BMI270_ANY_MOT_THRESHOLD_LSB 0x68
#define BMI270_ANY_MOT_ENABLE 0x80

// 候选 I2C 地址：SDO=GND→0x68，SDO=VDD→0x69。
static const uint8_t kCandidateAddrs[] = {0x68, 0x69};

static i2c_master_dev_handle_t s_dev;
static bool s_present;
static imu_type_t s_imu_type = IMU_NONE;  // 探测到的 IMU 型号

// 轮询基线：上次采样的合加速度幅值（1g ≈ 4096 LSB @ 2g 量程，14-bit 左对齐到 16-bit）。
// 用 float 便于做平方和，避免 int 溢出。
static float s_last_acc_mag;
static bool s_has_baseline;

// 拿起判定阈值：合加速度幅值变化量（单位：LSB）。
// 降低到 800 LSB（~0.2g）增强灵敏度，并配合首次基线建立后小幅运动也触发。
#define BMI270_PICKUP_DELTA_THRESHOLD 800.0f

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

// burst 写：先写寄存器地址，再连续写多字节数据。
static esp_err_t bmi270_write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[1 + 32];
    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 100);
}

// 加载 BMI270 feature engine 配置固件（8192 字节）。
// 流程据 Bosch bmi2.c write_config_file：禁 APS → INIT_CTRL=0 → 分块 burst 写 INIT_DATA
// （每块先写 word 地址到 INIT_ADDR_0/1）→ INIT_CTRL=1 → 等 20ms → 校验 INTERNAL_STATUS=0x01。
static esp_err_t load_config_file(void)
{
    esp_err_t err = bmi270_write_reg(BMI270_REG_PWR_CONF, 0x00);  // 禁 APS
    if (err != ESP_OK) {
        return err;
    }
    err = bmi270_write_reg(BMI270_REG_INIT_CTRL, 0x00);  // CONF_LOAD_EN=0
    if (err != ESP_OK) {
        return err;
    }

    // 分块 burst 写 8192 字节，每块最多 32 字节（I2C buf 限制）。
    for (size_t i = 0; i < sizeof(kBmi270ConfigFile); i += 32) {
        const size_t chunk = sizeof(kBmi270ConfigFile) - i < 32 ? sizeof(kBmi270ConfigFile) - i : 32;
        const uint16_t word = (uint16_t)(i / 2);
        const uint8_t addr[2] = {(uint8_t)(word & 0x0F), (uint8_t)((word >> 4) & 0xFF)};
        err = bmi270_write_regs(BMI270_REG_INIT_ADDR_0, addr, 2);
        if (err != ESP_OK) {
            return err;
        }
        err = bmi270_write_regs(BMI270_REG_INIT_DATA, &kBmi270ConfigFile[i], chunk);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = bmi270_write_reg(BMI270_REG_INIT_CTRL, BMI270_INIT_CTRL_LOAD_EN);  // CONF_LOAD_EN=1
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t status = 0;
    err = bmi270_read_reg(BMI270_REG_INTERNAL_STATUS, &status);
    if (err != ESP_OK) {
        return err;
    }
    if (status != BMI270_INTERNAL_STATUS_INIT_OK) {
        ESP_LOGE(TAG, "config file load failed, internal_status=0x%02x", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 配置 any-motion feature（feature page 1，offset 0x0C，16 字节）。
static esp_err_t configure_any_motion(void)
{
    esp_err_t err = bmi270_write_reg(BMI270_REG_FEAT_PAGE, BMI270_ANY_MOT_PAGE);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t feat[BMI270_FEAT_BURST_BYTES] = {0};
    err = bmi270_read_regs(BMI270_REG_FEATURES, feat, sizeof(feat));
    if (err != ESP_OK) {
        return err;
    }

    feat[BMI270_ANY_MOT_OFFSET + 0] = BMI270_ANY_MOT_DURATION_LSB;        // duration 低字节
    feat[BMI270_ANY_MOT_OFFSET + 1] = BMI270_ANY_MOT_DURATION_XYZ;        // duration 高位 + xyz_sel
    feat[BMI270_ANY_MOT_OFFSET + 2] = BMI270_ANY_MOT_THRESHOLD_LSB;       // threshold 低字节
    // offset+3：threshold 高 3 位保留，bit7=feature enable。
    feat[BMI270_ANY_MOT_OFFSET + 3] = (feat[BMI270_ANY_MOT_OFFSET + 3] & 0x07) | BMI270_ANY_MOT_ENABLE;

    err = bmi270_write_regs(BMI270_REG_FEATURES, feat, sizeof(feat));
    if (err != ESP_OK) {
        return err;
    }

    // INT1 路由：any-motion → INT1（INT1_MAP_FEAT bit6）。
    err = bmi270_write_reg(BMI270_REG_INT1_MAP_FEAT, BMI270_INT1_MAP_ANY_MOTION);
    if (err != ESP_OK) {
        return err;
    }
    // INT1 IO：push-pull + active low + output enable。
    err = bmi270_write_reg(BMI270_REG_INT1_IO_CTRL, BMI270_INT1_IO_PUSH_PULL_ACTIVE_LOW);
    if (err != ESP_OK) {
        return err;
    }
    // non-latched：触发后 INT1 脉冲式，便于 M5PM1 PYG4 捕获边沿/电平唤醒。
    return bmi270_write_reg(BMI270_REG_INT_LATCH, 0x00);
}

// 探测 IMU：先按 BMI270 模式读 CHIP_ID(0x00)=0x24；失败则按 MPU6886 读 WHO_AM_I(0x75)=0x70。
// 命中后置 s_dev 和 s_imu_type。
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

        // 先按 BMI270 探测：CHIP_ID(0x00)=0x24
        uint8_t chip_id = 0;
        err = bmi270_read_reg(BMI270_REG_CHIP_ID, &chip_id);
        if (err == ESP_OK && chip_id == BMI270_CHIP_ID) {
            s_imu_type = IMU_BMI270;
            ESP_LOGI(TAG, "BMI270 found at 0x%02x chip_id=0x%02x", kCandidateAddrs[i], chip_id);
            return ESP_OK;
        }
        // 再按 MPU6886 探测：WHO_AM_I(0x75)=0x70（兼容老批次 StickS3）
        uint8_t who = 0;
        err = bmi270_read_reg(MPU6886_REG_WHO_AM_I, &who);
        if (err == ESP_OK && who == MPU6886_WHO_AM_I_VAL) {
            s_imu_type = IMU_MPU6886;
            ESP_LOGI(TAG, "MPU6886 found at 0x%02x who=0x%02x", kCandidateAddrs[i], who);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "probe 0x%02x: BMI270 id=0x%02x, MPU6886 who=0x%02x",
                 kCandidateAddrs[i], chip_id, who);
    }

    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    s_imu_type = IMU_NONE;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t bmi270_init(void)
{
    esp_err_t err = probe_device();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMU not found at 0x68/0x69, pickup wake disabled: %s", esp_err_to_name(err));
        s_present = false;
        s_imu_type = IMU_NONE;
        return ESP_OK;  // 优雅降级，非致命
    }

    s_present = true;

    if (s_imu_type == IMU_BMI270) {
        // softreset，等 2ms（BMI270 datasheet 要求）。
        (void)bmi270_write_reg(BMI270_REG_CMD, BMI270_SOFT_RESET_CMD);
        vTaskDelay(pdMS_TO_TICKS(2));

        // softreset 后设备地址可能需要重新添加。
        err = probe_device();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BMI270 lost after softreset: %s", esp_err_to_name(err));
            s_present = false;
            return ESP_OK;
        }

        // 关闭高级电源保存（ADV_POW_EN=0），确保 ACC 稳定输出。
        (void)bmi270_write_reg(BMI270_REG_PWR_CONF, 0x00);
        vTaskDelay(pdMS_TO_TICKS(5));

        // 开 ACC（PWR_CTRL bit2）。
        (void)bmi270_write_reg(BMI270_REG_PWR_CTRL, BMI270_PWR_CTRL_ACC_EN);
        vTaskDelay(pdMS_TO_TICKS(5));

        s_has_baseline = false;
        ESP_LOGI(TAG, "BMI270 initialized (acc on, polling mode)");
    } else if (s_imu_type == IMU_MPU6886) {
        // 唤醒 MPU6886：PWR_MGMT_1 写 0 退出 sleep。
        (void)bmi270_write_reg(MPU6886_REG_PWR_MGMT_1, 0x00);
        vTaskDelay(pdMS_TO_TICKS(10));
        // 禁用所有 gyro 和 temp 待机（省电），只留 ACC。
        (void)bmi270_write_reg(MPU6886_REG_PWR_MGMT_2, 0x07);  // 010_111: disable gyro axes + temp

        s_has_baseline = false;
        ESP_LOGI(TAG, "MPU6886 initialized (acc on, polling mode)");
    }
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
    int16_t x, y, z;

    if (s_imu_type == IMU_BMI270) {
        if (bmi270_read_regs(BMI270_REG_ACC_X_LSB, data, 6) != ESP_OK) {
            return false;
        }
        // BMI270 ACC 为 14-bit 左对齐到 16-bit little-endian，低 2 bit 为新数据标志等保留。
        x = (int16_t)(((uint16_t)data[1] << 8) | data[0]) >> 2;
        y = (int16_t)(((uint16_t)data[3] << 8) | data[2]) >> 2;
        z = (int16_t)(((uint16_t)data[5] << 8) | data[4]) >> 2;
    } else if (s_imu_type == IMU_MPU6886) {
        if (bmi270_read_regs(MPU6886_REG_ACC_XOUT_H, data, 6) != ESP_OK) {
            return false;
        }
        // MPU6886 ACC 为 16-bit big-endian，默认 ±2g 量程下 1g≈16384 LSB（需 4 倍缩放后比阈值）。
        x = (int16_t)((data[0] << 8) | data[1]) >> 2;  // 缩放到与 BMI270 同尺度（14-bit）
        y = (int16_t)((data[2] << 8) | data[3]) >> 2;
        z = (int16_t)((data[4] << 8) | data[5]) >> 2;
    } else {
        return false;
    }

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
    // 阶段 3：加载 8KB config file + any-motion feature + INT1 输出，
    // 使 IMU 在 M5PM1 关机后继续低功耗检测拿起，翻转经 INT1→PYG4 唤醒 M5PM1。
    // BMI270 不在线时安全跳过（关机后仅靠按键唤醒）。
    if (!s_present) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "enabling pickup wake: load config file + any-motion");

    // softreset 回到干净状态，再重新探测句柄。
    (void)bmi270_write_reg(BMI270_REG_CMD, BMI270_SOFT_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(2));
    if (probe_device() != ESP_OK) {
        ESP_LOGW(TAG, "BMI270 lost before config load");
        return ESP_FAIL;
    }

    esp_err_t err = load_config_file();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config file load failed: %s", esp_err_to_name(err));
        return err;
    }

    // 开 ACC（any-motion 依赖加速度数据）。
    (void)bmi270_write_reg(BMI270_REG_PWR_CONF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));
    (void)bmi270_write_reg(BMI270_REG_PWR_CTRL, BMI270_PWR_CTRL_ACC_EN);
    vTaskDelay(pdMS_TO_TICKS(5));

    err = configure_any_motion();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "any-motion config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "pickup wake enabled (any-motion 50mg/20ms, INT1 active low)");
    return ESP_OK;
}
