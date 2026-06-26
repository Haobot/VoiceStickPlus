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
#define BMI270_REG_ACC_RANGE 0x41  // ACC 量程：低 2 位 0=±2g/1=±4g/2=±8g/3=±16g
#define BMI270_REG_ACC_CONF 0x40   // ACC 配置：ODR/带宽/性能模式
#define BMI270_REG_STATUS 0x03     // 状态：bit7=acc 数据就绪
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
#define BMI270_ACC_RANGE_2G 0x00     // ACC_RANGE 写 ±2g，固定 4096 LSB/g 换算尺度
#define BMI270_ACC_CONF_NORMAL_100HZ 0xA8  // 性能滤波 + 100Hz ODR（Bosch 复位默认值）
#define MPU6886_WHO_AM_I_VAL 0x70   // MPU6886 的 WHO_AM_I 返回值

// 加速度换算：BMI270 14-bit 左对齐与 MPU6886 16-bit 均经 >>2 归一到 14-bit 同尺度，
// ±2g 量程下 1g ≈ 4096 LSB。
#define BMI270_LSB_PER_G 4096.0f

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
static float s_pickup_threshold = BMI270_PICKUP_THRESHOLD_DEFAULT_LSB;

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
        // softreset 后等 10ms 让 POR 完成（datasheet 最少 2ms，但 esp_restart 软重启
        // 时 IMU 不断电，偏短的延时偶发导致后续配置写入丢失、ACC 不输出，故加大）。
        (void)bmi270_write_reg(BMI270_REG_CMD, BMI270_SOFT_RESET_CMD);
        vTaskDelay(pdMS_TO_TICKS(10));

        // softreset 后设备地址可能需要重新添加。
        err = probe_device();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BMI270 lost after softreset: %s", esp_err_to_name(err));
            s_present = false;
            return ESP_OK;
        }

        // 关键：BMI270 上电后必须加载 8KB config file 并等 INTERNAL_STATUS=0x01，
        // feature engine 就绪后 ACC 才会输出有效数据。缺这步时 STATUS data-ready 不置位、
        // ACC 读数恒为 0——基础 ACC 同样依赖 config file，不只是 any-motion feature。
        esp_err_t cfg_err = load_config_file();
        if (cfg_err != ESP_OK) {
            ESP_LOGE(TAG, "BMI270 config load failed: %s (acc data will be invalid)",
                     esp_err_to_name(cfg_err));
        }

        // config 就绪后配 ACC：性能模式 + 100Hz + ±2g，最后使能 ACC。
        (void)bmi270_write_reg(BMI270_REG_ACC_CONF, BMI270_ACC_CONF_NORMAL_100HZ);
        vTaskDelay(pdMS_TO_TICKS(5));
        (void)bmi270_write_reg(BMI270_REG_ACC_RANGE, BMI270_ACC_RANGE_2G);
        vTaskDelay(pdMS_TO_TICKS(5));
        (void)bmi270_write_reg(BMI270_REG_PWR_CTRL, BMI270_PWR_CTRL_ACC_EN);
        vTaskDelay(pdMS_TO_TICKS(10));

        // 读回诊断：正常应为 int_status=0x01（config ok）、status bit7=0x80（acc 数据就绪）。
        uint8_t pwr_ctrl = 0, acc_conf = 0, acc_range = 0, int_status = 0, status = 0;
        (void)bmi270_read_reg(BMI270_REG_PWR_CTRL, &pwr_ctrl);
        (void)bmi270_read_reg(BMI270_REG_ACC_CONF, &acc_conf);
        (void)bmi270_read_reg(BMI270_REG_ACC_RANGE, &acc_range);
        (void)bmi270_read_reg(BMI270_REG_INTERNAL_STATUS, &int_status);
        (void)bmi270_read_reg(BMI270_REG_STATUS, &status);

        s_has_baseline = false;
        ESP_LOGI(TAG,
                 "BMI270 initialized: pwr_ctrl=0x%02x acc_conf=0x%02x acc_range=0x%02x "
                 "int_status=0x%02x status=0x%02x",
                 pwr_ctrl, acc_conf, acc_range, int_status, status);
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

// 读取三轴原始加速度（归一到 14-bit 同尺度）。BMI270 与 MPU6886 寄存器布局/字节序不同，
// 在此集中处理，供拿起检测与 g 值换算复用。不在线或读失败返回非 ESP_OK。
static esp_err_t read_acc_raw(int16_t *x, int16_t *y, int16_t *z)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[6] = {0};

    if (s_imu_type == IMU_BMI270) {
        esp_err_t err = bmi270_read_regs(BMI270_REG_ACC_X_LSB, data, 6);
        if (err != ESP_OK) {
            return err;
        }
        // BMI270 ACC 为 14-bit 左对齐到 16-bit little-endian，低 2 bit 为新数据标志等保留。
        *x = (int16_t)(((uint16_t)data[1] << 8) | data[0]) >> 2;
        *y = (int16_t)(((uint16_t)data[3] << 8) | data[2]) >> 2;
        *z = (int16_t)(((uint16_t)data[5] << 8) | data[4]) >> 2;
        return ESP_OK;
    }
    if (s_imu_type == IMU_MPU6886) {
        esp_err_t err = bmi270_read_regs(MPU6886_REG_ACC_XOUT_H, data, 6);
        if (err != ESP_OK) {
            return err;
        }
        // MPU6886 ACC 为 16-bit big-endian，默认 ±2g 量程下 1g≈16384 LSB，>>2 缩放到 BMI270 同尺度。
        *x = (int16_t)((data[0] << 8) | data[1]) >> 2;
        *y = (int16_t)((data[2] << 8) | data[3]) >> 2;
        *z = (int16_t)((data[4] << 8) | data[5]) >> 2;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t bmi270_read_acc_g(float *x_g, float *y_g, float *z_g)
{
    int16_t x, y, z;
    esp_err_t err = read_acc_raw(&x, &y, &z);
    if (err != ESP_OK) {
        return err;
    }
    if (x_g) {
        *x_g = (float)x / BMI270_LSB_PER_G;
    }
    if (y_g) {
        *y_g = (float)y / BMI270_LSB_PER_G;
    }
    if (z_g) {
        *z_g = (float)z / BMI270_LSB_PER_G;
    }
    return ESP_OK;
}

bool bmi270_pickup_detected(void)
{
    int16_t x, y, z;
    if (read_acc_raw(&x, &y, &z) != ESP_OK) {
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

    if (delta >= s_pickup_threshold) {
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

void bmi270_set_pickup_threshold(float threshold_lsb)
{
    if (threshold_lsb < 0.0f) {
        ESP_LOGW(TAG, "negative pickup threshold %.1f ignored, using 0", threshold_lsb);
        threshold_lsb = 0.0f;
    }
    s_pickup_threshold = threshold_lsb;
    ESP_LOGI(TAG, "pickup threshold set to %.0f LSB", s_pickup_threshold);
}

float bmi270_get_pickup_threshold(void)
{
    return s_pickup_threshold;
}
