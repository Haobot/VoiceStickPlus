#include "bmi270.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stick_s3_board.h"
#include "bmi270_config_file.h"

static const char *TAG = "bmi270";

#define BMI270_I2C_FREQ_HZ 400000

// BMI270 寄存器地址（据 Bosch BMI270-Sensor-API bmi2_defs.h 确认）。
#define BMI270_REG_CHIP_ID 0x00
#define BMI270_REG_ACC_X_LSB 0x0C  // ACC X/Y/Z 各 2 字节 little-endian
#define BMI270_REG_GYR_X_LSB 0x12  // GYR X/Y/Z 各 2 字节 little-endian
#define BMI270_REG_ACC_RANGE 0x41  // ACC 量程：低 2 位 0=±2g/1=±4g/2=±8g/3=±16g
#define BMI270_REG_ACC_CONF 0x40   // ACC 配置：ODR/带宽/性能模式
#define BMI270_REG_GYR_RANGE 0x43  // GYR 量程：低 2 位 0=±2000/1=±1000/2=±500/3=±250 dps
#define BMI270_REG_GYR_CONF 0x42   // GYR 配置：ODR/带宽/性能模式
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
#define BMI270_REG_INT_STATUS_0 0x1C
#define BMI270_REG_PWR_CTRL 0x7D
#define BMI270_REG_PWR_CONF 0x7C
#define BMI270_REG_CMD 0x7E

#define BMI270_CHIP_ID 0x24
#define BMI270_SOFT_RESET_CMD 0xB6
#define BMI270_PWR_CTRL_ACC_EN 0x04  // bit2
#define BMI270_PWR_CTRL_GYR_EN 0x02  // bit1（Bosch bmi2_defs.h: BMI2_GYR_EN_MASK=0x02）
#define BMI270_ACC_RANGE_2G 0x00     // ACC_RANGE 写 ±2g，固定 4096 LSB/g 换算尺度
#define BMI270_ACC_CONF_NORMAL_100HZ 0xA8  // 性能滤波 + 100Hz ODR（Bosch 复位默认值）
#define BMI270_GYR_RANGE_500DPS 0x02       // GYR_RANGE 写 ±500dps，灵敏度 ≈65.5 LSB/dps
// GYR_CONF (0x42)：bits[7:4]=ODR, bits[3:2]=BW, bits[1:0]=性能模式。
// ODR 编码与 ACC 不同：100Hz=0x08，默认性能模式 bit0=1。
#define BMI270_GYR_CONF_NORMAL_100HZ 0x83  // 100Hz + 性能模式（ODR=0x08<<4=0x80, 性能bit0=0x01, BW=0x0）
#define MPU6886_WHO_AM_I_VAL 0x70   // MPU6886 的 WHO_AM_I 返回值

// 加速度换算：BMI270 14-bit 左对齐与 MPU6886 16-bit 均经 >>2 归一到 14-bit 同尺度，
// ±2g 量程下 1g ≈ 4096 LSB。
#define BMI270_LSB_PER_G 4096.0f
// 陀螺仪换算：BMI270 ±500dps 量程下灵敏度 ≈ 65.5 LSB/dps。
#define BMI270_DPS_PER_LSB_500DPS (1.0f / 65.5f)
#define BMI270_GYR_LSB_PER_DPS 65.5f

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

// 敲击检测状态机参数与状态。
// 灵敏度档位 1..10：1=最不灵敏（需大力敲，高 ACC 阈值），10=最灵敏（轻触即发，低 ACC 阈值）。
// 用户面向 level 取值 1..10，内部存 level-1 作为 kTapParams 数组索引。
typedef enum {
    TAP_SENSITIVITY_COUNT = 10,         // 档位总数
    TAP_SENSITIVITY_DEFAULT_LEVEL = 5,  // 用户面向默认档（1..10），对应原 medium 体验
} tap_sensitivity_t;

typedef enum {
    TAP_STATE_IDLE,
    TAP_STATE_FIRST,
} tap_state_t;

// 灵敏度参数表：ACC 突变阈值（g，相对于慢跟随基线）、GYR 平静阈值（dps）、
// 主轴集中度阈值（主轴 delta 占三轴 delta 之和的最小比例）。
// 手持敲击设备本体时，ACC 能量集中在单一法向轴；桌面震动/整体平移则三轴均匀扰动。
// 集中度判据用于拒绝后者，降低误触。GYR 仍用于排除挥动/旋转。
// 注意：read_acc_g 返回 g 值，不是 raw LSB。
typedef struct {
    float acc_thr_g;
    float gyr_calm_thr_dps;
    float axis_concentration_ratio;  // 主轴 delta 占比下限，[0,1]
} tap_params_t;

// 10 档参数表（索引 0..9 对应用户面向档 1..10）：
// ACC 阈值随档位升高线性递减（更易触发），GYR 平静门线性递增（更容忍旋转扰动）。
// 集中度阈值各档统一 0.6：敲击主轴能量占比通常 >0.7，三轴均匀扰动 <0.5。
// 档 1 = 旧版最灵敏档（0.50g/50dps）作为新基线；档 10 = 极灵敏（0.15g/90dps）。
// 旧版 1..10 档（1.60→0.50g）整体偏迟钝，用户反馈"10 档仍不够灵敏"，故整体下移。
static const tap_params_t kTapParams[TAP_SENSITIVITY_COUNT] = {
    { .acc_thr_g = 0.50f, .gyr_calm_thr_dps = 50.0f, .axis_concentration_ratio = 0.6f },  // 档1
    { .acc_thr_g = 0.46f, .gyr_calm_thr_dps = 54.4f, .axis_concentration_ratio = 0.6f },  // 档2
    { .acc_thr_g = 0.42f, .gyr_calm_thr_dps = 58.9f, .axis_concentration_ratio = 0.6f },  // 档3
    { .acc_thr_g = 0.38f, .gyr_calm_thr_dps = 63.3f, .axis_concentration_ratio = 0.6f },  // 档4
    { .acc_thr_g = 0.34f, .gyr_calm_thr_dps = 67.8f, .axis_concentration_ratio = 0.6f },  // 档5（默认）
    { .acc_thr_g = 0.31f, .gyr_calm_thr_dps = 72.2f, .axis_concentration_ratio = 0.6f },  // 档6
    { .acc_thr_g = 0.27f, .gyr_calm_thr_dps = 76.7f, .axis_concentration_ratio = 0.6f },  // 档7
    { .acc_thr_g = 0.23f, .gyr_calm_thr_dps = 81.1f, .axis_concentration_ratio = 0.6f },  // 档8
    { .acc_thr_g = 0.19f, .gyr_calm_thr_dps = 85.6f, .axis_concentration_ratio = 0.6f },  // 档9
    { .acc_thr_g = 0.15f, .gyr_calm_thr_dps = 90.0f, .axis_concentration_ratio = 0.6f },  // 档10
};

#define TAP_MIN_GAP_MS   80
#define TAP_MAX_GAP_MS   400
#define TAP_DEBOUNCE_MS  50

static bool s_tap_enabled = false;
static tap_sensitivity_t s_tap_sensitivity = TAP_SENSITIVITY_DEFAULT_LEVEL - 1;
static tap_state_t s_tap_state = TAP_STATE_IDLE;
static int64_t s_tap_first_us = 0;
// 三轴各自基线（g）：平静期慢速 EMA 跟随，冲击/衰减期极慢跟随（不冻结，避免姿态变化后基线卡死）。
static float s_tap_baseline_g[3] = {0.0f, 0.0f, 0.0f};
static bool s_tap_has_baseline = false;
static int64_t s_tap_debounce_until_us = 0;
// 上一帧主轴 delta（g），用于脉冲瞬时性判据（敲击是上升沿，持续偏移不是）。
static float s_tap_last_delta_dom = 0.0f;

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

        // config 就绪后配 ACC：性能模式 + 100Hz + ±2g。
        (void)bmi270_write_reg(BMI270_REG_ACC_CONF, BMI270_ACC_CONF_NORMAL_100HZ);
        vTaskDelay(pdMS_TO_TICKS(5));
        (void)bmi270_write_reg(BMI270_REG_ACC_RANGE, BMI270_ACC_RANGE_2G);
        vTaskDelay(pdMS_TO_TICKS(5));
        // 同步配 GYR：性能模式 + 100Hz + ±500dps，供敲击检测做挥动排除。
        (void)bmi270_write_reg(BMI270_REG_GYR_CONF, BMI270_GYR_CONF_NORMAL_100HZ);
        vTaskDelay(pdMS_TO_TICKS(5));
        (void)bmi270_write_reg(BMI270_REG_GYR_RANGE, BMI270_GYR_RANGE_500DPS);
        vTaskDelay(pdMS_TO_TICKS(5));
        // 同时使能 ACC 与 GYR。
        (void)bmi270_write_reg(BMI270_REG_PWR_CTRL, BMI270_PWR_CTRL_ACC_EN | BMI270_PWR_CTRL_GYR_EN);
        vTaskDelay(pdMS_TO_TICKS(10));

        // 读回诊断：正常应为 int_status=0x01（config ok）、status bit7=0x80（acc 数据就绪）。
        uint8_t pwr_ctrl = 0, acc_conf = 0, acc_range = 0, gyr_conf = 0, gyr_range = 0, int_status = 0, status = 0;
        (void)bmi270_read_reg(BMI270_REG_PWR_CTRL, &pwr_ctrl);
        (void)bmi270_read_reg(BMI270_REG_ACC_CONF, &acc_conf);
        (void)bmi270_read_reg(BMI270_REG_ACC_RANGE, &acc_range);
        (void)bmi270_read_reg(BMI270_REG_GYR_CONF, &gyr_conf);
        (void)bmi270_read_reg(BMI270_REG_GYR_RANGE, &gyr_range);
        (void)bmi270_read_reg(BMI270_REG_INTERNAL_STATUS, &int_status);
        (void)bmi270_read_reg(BMI270_REG_STATUS, &status);

        s_has_baseline = false;
        ESP_LOGI(TAG,
                 "BMI270 initialized: pwr_ctrl=0x%02x acc_conf=0x%02x acc_range=0x%02x "
                 "gyr_conf=0x%02x gyr_range=0x%02x int_status=0x%02x status=0x%02x",
                 pwr_ctrl, acc_conf, acc_range, gyr_conf, gyr_range, int_status, status);
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

esp_err_t bmi270_read_gyr_dps(float *x_dps, float *y_dps, float *z_dps)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    // 软件敲击检测仅 BMI270 支持；MPU6886 未启用陀螺仪，直接返回不支持。
    if (s_imu_type != IMU_BMI270) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t data[6] = {0};
    esp_err_t err = bmi270_read_regs(BMI270_REG_GYR_X_LSB, data, 6);
    if (err != ESP_OK) {
        return err;
    }

    // BMI270 GYR 为 16-bit signed little-endian，±500dps 量程下灵敏度 65.5 LSB/dps。
    const float x = (float)(int16_t)(((uint16_t)data[1] << 8) | data[0]) / BMI270_GYR_LSB_PER_DPS;
    const float y = (float)(int16_t)(((uint16_t)data[3] << 8) | data[2]) / BMI270_GYR_LSB_PER_DPS;
    const float z = (float)(int16_t)(((uint16_t)data[5] << 8) | data[4]) / BMI270_GYR_LSB_PER_DPS;

    if (x_dps) {
        *x_dps = x;
    }
    if (y_dps) {
        *y_dps = y;
    }
    if (z_dps) {
        *z_dps = z;
    }
    return ESP_OK;
}

// 读取当前 ACC 三轴（g）与 GYR 合角速度幅值（dps），供敲击状态机使用。
// 保留 ACC 三轴分量以做主轴集中度判据。返回 true 表示成功读到有效数据。
static bool read_acc_axes_and_gyr_mag(float acc_g[3], float *gyr_mag)
{
    float x_dps, y_dps, z_dps;
    if (bmi270_read_acc_g(&acc_g[0], &acc_g[1], &acc_g[2]) != ESP_OK) {
        return false;
    }
    if (bmi270_read_gyr_dps(&x_dps, &y_dps, &z_dps) != ESP_OK) {
        return false;
    }
    *gyr_mag = sqrtf(x_dps * x_dps + y_dps * y_dps + z_dps * z_dps);
    return true;
}

// 检测一次 ACC 敲击脉冲。
// 命中条件（同时满足）：
//   (1) 主轴 delta >= acc_thr_g       （单一轴上有足够大的突变）
//   (2) 主轴集中度 >= axis_concentration_ratio（能量集中在单一轴，拒绝三轴均匀扰动）
//   (3) gyr_mag <= gyr_calm_thr_dps   （未在挥动/旋转）
//   (4) 脉冲瞬时性：当前 delta 明显大于上一帧 delta（上升沿）。持续偏移（姿态变化后基线
//       未追上）前后帧 delta 都大，不满足上升沿 → 拒绝。这是防止"举起设备持续误触发"的关键。
// 命中返回 true，同时更新去抖时间。
// 基线策略：平静期慢 EMA(0.2) 跟随；冲击/偏移期极慢 EMA(0.02) 跟随——不冻结，让基线最终
// 能追上姿态变化，避免 dom 持续超阈值导致永久误判。冲击是瞬时（几 ms），极慢 EMA 不会在
// 双击窗口（<400ms）内明显抬高基线导致第二击漏检。
static bool detect_tap_impulse(const tap_params_t *params, int64_t now_us)
{
    if (now_us < s_tap_debounce_until_us) {
        return false;
    }

    float acc_g[3];
    float gyr_mag;
    if (!read_acc_axes_and_gyr_mag(acc_g, &gyr_mag)) {
        return false;
    }

    if (!s_tap_has_baseline) {
        s_tap_baseline_g[0] = acc_g[0];
        s_tap_baseline_g[1] = acc_g[1];
        s_tap_baseline_g[2] = acc_g[2];
        s_tap_has_baseline = true;
        ESP_LOGI(TAG, "tap baseline=%.4f/%.4f/%.4f g", acc_g[0], acc_g[1], acc_g[2]);
        return false;
    }

    // 三轴各自 delta 与总和，找主轴。
    const float dx = fabsf(acc_g[0] - s_tap_baseline_g[0]);
    const float dy = fabsf(acc_g[1] - s_tap_baseline_g[1]);
    const float dz = fabsf(acc_g[2] - s_tap_baseline_g[2]);
    const float delta_sum = dx + dy + dz;
    const float delta_dom = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);

    // 基线更新：平静期慢跟随（alpha=0.2），偏移/冲击期极慢跟随（alpha=0.02）。
    // 不冻结 → 姿态变化后基线能在 ~1s 内追上，避免持续误判。
    const float quiet_thr = params->acc_thr_g * 0.3f;
    const float baseline_alpha = (delta_dom < quiet_thr) ? 0.2f : 0.02f;
    s_tap_baseline_g[0] = s_tap_baseline_g[0] * (1.0f - baseline_alpha) + acc_g[0] * baseline_alpha;
    s_tap_baseline_g[1] = s_tap_baseline_g[1] * (1.0f - baseline_alpha) + acc_g[1] * baseline_alpha;
    s_tap_baseline_g[2] = s_tap_baseline_g[2] * (1.0f - baseline_alpha) + acc_g[2] * baseline_alpha;

    // 每 100 次打印一次状态，确认状态机在跑。
    static int dbg_cnt = 0;
    if (++dbg_cnt >= 100) {
        dbg_cnt = 0;
        const float concentration = delta_sum > 0.0f ? delta_dom / delta_sum : 0.0f;
        ESP_LOGI(TAG, "tap poll: dom=%.4f last=%.4f conc=%.2f gyr=%.2f thr=%.2f/%.1f/%.2f",
                 delta_dom, s_tap_last_delta_dom, concentration, gyr_mag,
                 params->acc_thr_g, params->gyr_calm_thr_dps, params->axis_concentration_ratio);
    }

    const bool impulse = delta_dom >= params->acc_thr_g &&
                         gyr_mag <= params->gyr_calm_thr_dps;
    if (!impulse) {
        s_tap_last_delta_dom = delta_dom;
        return false;
    }
    // 集中度判据。
    const float concentration = delta_dom / delta_sum;
    if (concentration < params->axis_concentration_ratio) {
        ESP_LOGD(TAG, "tap rejected: low concentration %.2f (dom=%.4f sum=%.4f)",
                 concentration, delta_dom, delta_sum);
        s_tap_last_delta_dom = delta_dom;
        return false;
    }
    // 脉冲瞬时性判据：当前 delta 必须明显大于上一帧（上升沿）。
    // 阈值取 acc_thr_g 的 0.5 倍：敲击第一帧 delta 远大于静止帧；持续偏移则前后帧接近。
    const float rising_edge = s_tap_last_delta_dom + params->acc_thr_g * 0.5f;
    if (delta_dom < rising_edge) {
        ESP_LOGD(TAG, "tap rejected: not rising edge dom=%.4f last=%.4f",
                 delta_dom, s_tap_last_delta_dom);
        s_tap_last_delta_dom = delta_dom;
        return false;
    }
    ESP_LOGI(TAG, "tap impulse: dom=%.4f conc=%.2f gyr=%.2f", delta_dom, concentration, gyr_mag);
    s_tap_last_delta_dom = 0.0f;  // 命中后重置，去抖期结束后允许下一次上升沿
    s_tap_debounce_until_us = now_us + (TAP_DEBOUNCE_MS * 1000LL);
    return true;
}

bool bmi270_tap_poll(void)
{
    if (!s_tap_enabled || s_imu_type != IMU_BMI270) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    const tap_params_t *params = &kTapParams[s_tap_sensitivity];

    switch (s_tap_state) {
        case TAP_STATE_IDLE:
            if (detect_tap_impulse(params, now_us)) {
                s_tap_state = TAP_STATE_FIRST;
                s_tap_first_us = now_us;
            }
            return false;

        case TAP_STATE_FIRST: {
            const int64_t elapsed_ms = (now_us - s_tap_first_us) / 1000LL;
            if (elapsed_ms > TAP_MAX_GAP_MS) {
                // 超窗口，视为单击，回到 IDLE。
                s_tap_state = TAP_STATE_IDLE;
                return false;
            }
            if (elapsed_ms < TAP_MIN_GAP_MS) {
                return false;
            }
            if (detect_tap_impulse(params, now_us)) {
                s_tap_state = TAP_STATE_IDLE;
                ESP_LOGD(TAG, "double tap detected");
                return true;
            }
            return false;
        }

        default:
            s_tap_state = TAP_STATE_IDLE;
            return false;
    }
}

void bmi270_set_tap_enabled(bool enable)
{
    s_tap_enabled = enable;
    if (!enable) {
        s_tap_state = TAP_STATE_IDLE;
        s_tap_has_baseline = false;
    }
    ESP_LOGI(TAG, "tap detection %s", enable ? "enabled" : "disabled");
}

void bmi270_set_tap_sensitivity(int level)
{
    // 用户面向 level 取值 1..10，内部存 level-1 作为数组索引。
    if (level < 1 || level > TAP_SENSITIVITY_COUNT) {
        level = TAP_SENSITIVITY_DEFAULT_LEVEL;
    }
    s_tap_sensitivity = (tap_sensitivity_t)(level - 1);
    ESP_LOGI(TAG, "tap sensitivity set to %d", level);
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
