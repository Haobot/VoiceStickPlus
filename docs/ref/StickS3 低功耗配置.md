---
created: 2026-06-23T05:14:32 (UTC +08:00)
tags: []
source: https://docs.m5stack.com/zh_CN/arduino/m5sticks3/m5pm1
author: 
---

# StickS3 低功耗配置

> ## Excerpt
> The reference docs for M5Stack products. Quick start, get the detailed information or instructions such as IDE,UIFLOW,Arduino. The tutorials for M5Burner, Firmware, Burning, programming. ESP32,M5StickC,StickV, StickT,M5ATOM.

---
## 1\. 多级电源开关设计

StickS3 内部集成 M5PM1，结合硬件电路实现了多级电源开关，不同的级别的电源开关对应控制了相关的外设与接口供电。用户可根据运行需求，切换不同级别的电源使能，关闭未使用的外设部分，实现整机低功耗。

### M5PM1 驱动库

使用 M5PM1 驱动库能够非常便捷地配置 M5PM1 的引脚功能，用于低功耗唤醒以及外设供电开关。

-   [M5PM1 Arduino Library](https://github.com/m5stack/M5PM1)

![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/sticks3_pmic_level_01.png)

注意事项

StickS3 的多级电源开关设计并非串联结构。L1 ~ L3B 开关的电源输入均来自于 L0 级源头，而非上一级电源，因此支持独立控制不同级别的电源开关，以上分级根据外设供电与功耗进行区分。

M5PM1 启动上电后 L0, L1, L2 将自动开启 (默认打开 `DCDC3V3_EN_PP`, `LDO3V3_EN_PP`, `CHG_EN_PP`)，在 M5Unified 初始化流程中，将进一步打开 L3A, L3B，使能其他的外设供电。

### L0

此级电源条件下，电池保持对 M5PM1 供电，其余外设可关闭供电。电池电量未耗尽的情况下，该层电源将一直保持，M5PM1 支持基础的按键开关机操作。

### L1

启用 IMU 外设供电。该层级的电源开关使用 M5PM1 的 `LDO3V3_EN_PP`，可通过以下 API 实现开关控制。

```cpp
pm1.setLdoEnable(true); // L1 ON
pm1.setLdoEnable(false); // L1 OFF
```

同时，IMU 的 INT1 中断引脚连接至 M5PM1 的 PYG4，可通过配置相关的寄存器实现保持 IMU 供电，M5PM1 进入休眠状态，通过翻转设备，触发 IMU 中断唤醒 M5PM1。 可通过以下 API 实现 IMU (L1) 供电保持。

```cpp
pm1.setLdoEnable(true);
pm1.ldoSetPowerHold(true);
pm1.setLedEnLevel(true);
pm1.shutdown();
```

### L2/L3A

此级电源条件下，将启用 ESP32-S3, Grove 接口，Hat 拓展接口，红外收发，按键上拉部分的供电。

当 ESP32-S3 处于休眠状态时，电源处于 L2 级。当 ESP32-S3 处于工作状态时，电源处于 L3A 级。

ESP32-S3 可通过控制 M5PM1 进入休眠的方式，关断自身的供电 (L2->L1/L0)。

其中，拓展接口的输入 / 输出供电以及红外电路的供电，需额外通过 M5PM1 的 `EXT_5V_EN`(BOOST5V\_EN\_PP) 引脚进行开关控制。

因此在拓展接口外接传感器，以及使用红外收发功能前，请确保外设处于供电状态。

EXT\_5V\_EN 输入供电注意

设备 5V 供电接口可配置为 DC 5V 输出 / 输入模式。接口默认为输入模式，此时可通过 Grove 接口，顶部 Hat2-Bus 的 EXT\_5V， 5VIN 接口输入 DC 5V 供电。 当配置为输出模式时，仅允许通过 USB 或 顶部 Hat2-Bus 的 5VIN 进行输入，请勿从其他输出接口进行供电输入，否则设备存在短路损坏风险。

M5Unified 默认初始化中将关闭 EXT\_5V\_EN，此操作将关闭 Grove，Hat EXT\_5V 接口，IR TX/RX 的供电，切换为输入模式。此时需要外部 5V 输入供电， IR TX/RX 才能正常工作。无外接供电的使用场景，则可以通过以下 API 重新打开 EXT\_5V 输出模式，恢复 IR TX/RX 供电。

```cpp
M5.Power.setExtOutput(true); // EXT_5V OUTPUT
// M5.Power.setExtOutput(false); // EXT_5V INPUT
```

### L3B

该电源层级将打开所有外设供电，M5Unified 默认初始化该层级供电，包含 LCD 背光，MIC，SPK。该层级的电源开关使用 M5PM1 的 `PYG2`，可通过以下 API 实现开关控制。

```cpp
pm1.gpioSetFunc(M5PM1_GPIO_NUM_2, M5PM1_GPIO_FUNC_GPIO);
pm1.gpioSetMode(M5PM1_GPIO_NUM_2, M5PM1_GPIO_MODE_OUTPUT);
pm1.gpioSetDrive(M5PM1_GPIO_NUM_2, M5PM1_GPIO_DRIVE_PUSHPULL);
pm1.gpioSetOutput(M5PM1_GPIO_NUM_2, false);
```

### SPK AMP

设备 SPK 功放开关通过 M5PM1 的 `PYG3` 引脚进行开关控制，该部分可通过 M5Unified API 或 M5PM1 API 实现启用和关断。

注意：在使用 IR 接收功能的时候需要关闭 SPK 功放。

```cpp
M5.Speaker.begin(); // 初始化 SPK 并打开功放
// M5.Speaker.end(); // 释放 SPK，并关闭功放
```

or

```cpp
pm1.gpioSetFunc(M5PM1_GPIO_NUM_3, M5PM1_GPIO_FUNC_GPIO);
pm1.gpioSetMode(M5PM1_GPIO_NUM_3, M5PM1_GPIO_MODE_OUTPUT);
pm1.gpioSetDrive(M5PM1_GPIO_NUM_3, M5PM1_GPIO_DRIVE_PUSHPULL);
pm1.gpioSetOutput(M5PM1_GPIO_NUM_3, true);  // 打开 SPK 功放
// pm1.gpioSetOutput(M5PM1_GPIO_NUM_3, false); // 关闭 SPK 功放
```

## 2\. M5PM1 休眠

### 手动休眠

M5PM1 可通过程序手动控制进入休眠状态，来降低整机功耗。默认状态下，直接设置休眠将回退至 L0 级电源，此时仅 M5PM1 保持供电。

在一些特殊的使用场景 (例如 IMU 唤醒，或是 ESP32-S3 SoC 休眠)，允许 M5PM1 进入休眠降低的同时，保持一些层级的外设供电，用于唤醒源或是状态保持。

该类应用需要在 M5PM1 进入休眠模式前，配置对应层级的电源开关引脚状态，并激活状态保持。

### I2C 空闲休眠

M5PM1 支持配置 I2C 空闲通信自动休眠状态，来降低整机功耗。进入休眠状态后，ESP32-S3 与 M5PM1 的首次通信将用于 M5PM1 唤醒，因为将通信失败，有效的通信将与唤醒后的下一次通信。

```cpp
m5pm1_err_t setI2cSleepTime(uint8_t seconds);
```

## 3\. M5PM1 Timer 定时器

M5PM1 支持配置定时器功能，当计时结束后，执行相应的操作，如开机，关机，复位等。

```cpp
m5pm1_err_t timerSet(uint32_t seconds, m5pm1_tim_action_t action);
```

```cpp
typedef enum {
    M5PM1_TIM_ACTION_STOP = 0b000,     // 停止，无动作
                                       // Stop, no action
    M5PM1_TIM_ACTION_FLAG = 0b001,     // 仅设置标志
                                       // Set flag only
    M5PM1_TIM_ACTION_REBOOT = 0b010,   // 系统复位
                                       // System reboot
    M5PM1_TIM_ACTION_POWERON = 0b011,  // 开机
                                       // Power on
    M5PM1_TIM_ACTION_POWEROFF = 0b100  // 关机
                                       // Power off
} m5pm1_tim_action_t;
```

### 定时开机 / 关机案例

案例说明：设备开机后，单击按键 A 配置 10s 定时器触发重新开机，单击按键 B 配置 10s 后定时器触发 M5PM1 关机 (可通过单击电源按键重新开机)。

```cpp
#include <M5Unified.h>
#include <M5PM1.h>
#include <Wire.h>

M5PM1 pm1;

void setup(void)
{
    M5.begin();
    M5.Display.setRotation(1);
    Serial.begin(115200);
    auto pin_num_sda = M5.getPin(m5::pin_name_t::in_i2c_sda);
    auto pin_num_scl = M5.getPin(m5::pin_name_t::in_i2c_scl);
    M5_LOGI("getPin: SDA:%u SCL:%u", pin_num_sda, pin_num_scl);
    Wire.end();
    Wire.begin(pin_num_sda, pin_num_scl, 100000U);

    // Initialize PM1
    m5pm1_err_t err = pm1.begin(&Wire, M5PM1_DEFAULT_ADDR, pin_num_sda, pin_num_scl, M5PM1_I2C_FREQ_100K);

    if (err == M5PM1_OK) {
        Serial.println("PM1 initialization successful");
    } else {
        Serial.printf("PM1 initialization failed, error code: %d\n", err);
    }
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(0, 10);
    M5.Display.println("Timer Power Test");
    M5.Display.println("BtnA: After 10s ON");
    M5.Display.println("BtnB: After 10s OFF");
}

void loop(void)
{
    M5.update();
    if (M5.BtnA.wasPressed()) {
        M5.Display.fillScreen(BLACK);
        M5.Display.setCursor(0, 10);
        M5.Display.println("Shutdown");
        M5.Display.println("After 10s");
        M5.Display.println("Power ON");
        delay(1000);
        pm1.timerSet(10, M5PM1_TIM_ACTION_POWERON);
        pm1.shutdown();
    }
    if (M5.BtnB.wasPressed()) {
        M5.Display.fillScreen(BLACK);
        M5.Display.setCursor(0, 10);
        M5.Display.println("After 10s");
        M5.Display.println("Power OFF");
        delay(1000);
        pm1.timerSet(10, M5PM1_TIM_ACTION_POWEROFF);
    }
}
```

## 4\. IMU 案例程序

### BMI270 驱动库

-   [SparkFun\_BMI270\_Arduino\_Library](https://github.com/sparkfun/SparkFun_BMI270_Arduino_Library)

### IMU M5PM1 唤醒

电源切换至 L1 Mode, 整机仅 IMU 和 M5PM1 处于供电状态。在配置 IMU 唤醒功能后，M5PM1 也进入休眠状态，同时保持 L1 的输出供电 (3V3\_L1\_EN)，用于维持 IMU 工作。

此时可通过翻转或移动设备，触发 IMU 唤醒信号，唤醒 M5PM1 重新启动。

M5PM1 唤醒后将重新运行 L0、L1 和 L2 上电流程。ESP32-S3 重新执行初始化。

案例说明：设备开机后，单击按键 A 配置 IMU 中断模式与 M5PM1 L1 电源保持，M5PM1 进入休眠。 此时翻转或移动设备即可触发 M5PM1 唤醒，ESP32-S3 重新上电。

```cpp
#include <M5Unified.h>
#include <M5PM1.h>
#include <Wire.h>
#include "SparkFun_BMI270_Arduino_Library.h"

BMI270 imu;
M5PM1 pm1;

void setup(void)
{
    M5.begin();
    M5.Display.setRotation(1);
    Serial.begin(115200);
    auto pin_num_sda = M5.getPin(m5::pin_name_t::in_i2c_sda);
    auto pin_num_scl = M5.getPin(m5::pin_name_t::in_i2c_scl);
    M5_LOGI("getPin: SDA:%u SCL:%u", pin_num_sda, pin_num_scl);
    Wire.end();
    Wire.begin(pin_num_sda, pin_num_scl, 100000U);

    // Initialize PM1
    m5pm1_err_t err = pm1.begin(&Wire, M5PM1_DEFAULT_ADDR, pin_num_sda, pin_num_scl, M5PM1_I2C_FREQ_100K);

    if (err == M5PM1_OK) {
        Serial.println("PM1 initialization successful");
        pm1.gpioSetWakeEnable(M5PM1_GPIO_NUM_4, true);
        pm1.gpioSetWakeEdge(M5PM1_GPIO_NUM_4, M5PM1_GPIO_WAKE_FALLING);  // Falling edge
    } else {
        Serial.printf("PM1 initialization failed, error code: %d\n", err);
    }

    // Check if sensor is connected and initialize
    // Address defaults to 0x68)
    while(imu.beginI2C(BMI2_I2C_PRIM_ADDR) != BMI2_OK)
    {
        Serial.println("Error: BMI270 not connected, check wiring and I2C address!");
        delay(1000);
    }
    imu.disableFeature(BMI2_ANY_MOTION);
    Serial.println("BMI270 initialization successful");

    M5.Display.setFont(&fonts::FreeMonoBold9pt7b);
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(0, 10);
    M5.Display.println("IMU Wakeup Test");
    M5.Display.println("Press BtnA to Sleep");
    M5.Display.println("Shake to wake up");
}

void loop(void)
{
    M5.update();
    if (M5.BtnA.wasPressed()) {
        int8_t ret = imu.enableFeature(BMI2_ANY_MOTION);
        // Optional
        // bmi2_sens_config config;
        // config.type = BMI2_ANY_MOTION;
        // config.cfg.any_motion.threshold = 0xA0;// 1LSB equals to 0.48mg. Default is 83mg. Lower is more sensitive
        // config.cfg.any_motion.duration = 0x0A; // 1LSB equals 20ms. Default is 100ms.
        // ret |= imu.setConfig(config);
        //
        bmi2_int_pin_config intPinConfig;
        intPinConfig.pin_type = BMI2_INT1;
        intPinConfig.int_latch = BMI2_INT_NON_LATCH;
        intPinConfig.pin_cfg[0].lvl = BMI2_INT_ACTIVE_LOW;
        intPinConfig.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
        intPinConfig.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;
        intPinConfig.pin_cfg[0].input_en = BMI2_INT_INPUT_DISABLE;
        ret |= imu.setInterruptPinConfig(intPinConfig);
        ret |= imu.mapInterruptToPin(BMI2_ANY_MOTION_INT, BMI2_INT1);
        if (!ret){
            Serial.println("BMI270 AnyMotionInterrupt enabled successfully");
        } else {
            Serial.println("Failed to enable BMI270 AnyMotionInterrupt");
        }

        M5.Display.fillScreen(BLACK);
        M5.Display.setCursor(0, 10);
        M5.Display.println("Power OFF");
        delay(1000);
        // Shutdown
        pm1.setLdoEnable(true);
        pm1.ldoSetPowerHold(true);
        pm1.setLedEnLevel(true);
        pm1.shutdown();
    }
}
```

例程效果演示：

### IMU ESP32-S3 唤醒

M5PM1 的 PYG1\_IRQ 引脚在电路上连接到了 ESP32-S3 的 G13 上，可以通过链式的唤醒信号实现 ESP32-S3 唤醒，实现步骤如下:

1.  配置 M5PM1 PYG4 为输入模式，该引脚连接 IMU 的 INT1 输出信号。
2.  配置 M5PM1 PYG1\_IRQ 为 IRQ 输出信号引脚。当 PYG4 (IMU 产生中断信号) 引脚状态变化时， PYG1\_IRQ 引脚将输出
3.  配置 ESP32-S3 进入休眠，配置唤醒引脚为 G13
4.  翻转或移动设备：触发 IMU 唤醒信号 -> 触发 M5PM1 PYG1\_IRQ -> 唤醒 ESP32-S3

案例说明：设备开机后，单击按键 A 配置 IMU 中断模式。翻转或移动设备即可触发 GPIO 中断处理函数。再次单击按键 A 可清除 M5PM1 IRQ 标志位，再次进行测试。

```cpp
#include <M5Unified.h>
#include <M5PM1.h>
#include <Wire.h>
#include "SparkFun_BMI270_Arduino_Library.h"
#include "driver/rtc_io.h"

BMI270 imu;
M5PM1 pm1;

void setup(void)
{
    M5.begin();
    M5.Display.setRotation(1);
    Serial.begin(115200);
    auto pin_num_sda = M5.getPin(m5::pin_name_t::in_i2c_sda);
    auto pin_num_scl = M5.getPin(m5::pin_name_t::in_i2c_scl);
    M5_LOGI("getPin: SDA:%u SCL:%u", pin_num_sda, pin_num_scl);
    Wire.end();
    Wire.begin(pin_num_sda, pin_num_scl, 100000U);

    // Initialize PM1
    m5pm1_err_t err = pm1.begin(&Wire, M5PM1_DEFAULT_ADDR, pin_num_sda, pin_num_scl, M5PM1_I2C_FREQ_100K);

    if (err == M5PM1_OK) {
        Serial.println("PM1 initialization successful");
        pm1.irqClearGpioAll();
        pm1.irqClearSysAll();
        pm1.irqClearBtnAll();

        pm1.irqSetGpioMaskAll(M5PM1_IRQ_MASK_ENABLE);
        pm1.irqSetSysMaskAll(M5PM1_IRQ_MASK_ENABLE);
        pm1.irqSetBtnMaskAll(M5PM1_IRQ_MASK_ENABLE);

        pm1.irqSetGpioMask(M5PM1_IRQ_GPIO4, M5PM1_IRQ_MASK_DISABLE);
        pm1.gpioSetMode(M5PM1_GPIO_NUM_4, M5PM1_GPIO_MODE_INPUT);
        pm1.gpioSetPull(M5PM1_GPIO_NUM_4, M5PM1_GPIO_PULL_UP);

        pm1.gpioSetMode(M5PM1_GPIO_NUM_1, M5PM1_GPIO_MODE_OUTPUT);
        pm1.gpioSetDrive(M5PM1_GPIO_NUM_1, M5PM1_GPIO_DRIVE_PUSHPULL);
        pm1.gpioSetFunc(M5PM1_GPIO_NUM_1, M5PM1_GPIO_FUNC_IRQ);
    } else {
        Serial.printf("PM1 initialization failed, error code: %d\n", err);
    }

    // Check if sensor is connected and initialize
    // Address is optional (defaults to 0x68)
    while(imu.beginI2C(BMI2_I2C_PRIM_ADDR) != BMI2_OK)
    {
        Serial.println("Error: BMI270 not connected, check wiring and I2C address!");
        delay(1000);
    }
    imu.disableFeature(BMI2_ANY_MOTION);
    Serial.println("BMI270 initialization successful");

    M5.Display.setFont(&fonts::FreeMonoBold9pt7b);
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(0, 10);
    M5.Display.println("IMU IRQ Test");
    M5.Display.println("Press BtnA to Sleep");
    M5.Display.println("Shake to wake up");
}

void ARDUINO_ISR_ATTR pm1_irq_handler()
{
    Serial.println("PM1 IRQ triggered");
    M5.Display.println("PM1 IRQ triggered");
}

void loop(void)
{
    M5.update();
    if (M5.BtnA.wasPressed()) {
        pm1.irqClearGpioAll();
        pm1.irqClearSysAll();
        pm1.irqClearBtnAll();
        int8_t ret = imu.enableFeature(BMI2_ANY_MOTION);
        // Optional
        // bmi2_sens_config config;
        // config.type = BMI2_ANY_MOTION;
        // config.cfg.any_motion.threshold = 0xE0;// 1LSB equals to 0.48mg. Default is 83mg. Lower is more sensitive
        // config.cfg.any_motion.duration = 0x0A; // 1LSB equals 20ms. Default is 100ms.
        // imu.setConfig(config);
        //
        bmi2_int_pin_config intPinConfig;
        intPinConfig.pin_type = BMI2_INT1;
        intPinConfig.int_latch = BMI2_INT_NON_LATCH;
        intPinConfig.pin_cfg[0].lvl = BMI2_INT_ACTIVE_LOW;
        intPinConfig.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
        intPinConfig.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;
        intPinConfig.pin_cfg[0].input_en = BMI2_INT_INPUT_DISABLE;
        ret |= imu.setInterruptPinConfig(intPinConfig);
        ret |= imu.mapInterruptToPin(BMI2_ANY_MOTION_INT, BMI2_INT1);
        if (!ret){
            Serial.println("BMI270 AnyMotionInterrupt enabled successfully");
        } else {
            Serial.println("Failed to enable BMI270 AnyMotionInterrupt");
        }

        M5.Display.fillScreen(BLACK);
        M5.Display.setCursor(0, 10);
        M5.Display.println("Now Shake!");

        // Choose either of the two pieces of code below.
        pinMode(GPIO_NUM_13, INPUT_PULLUP);
        attachInterrupt(GPIO_NUM_13, pm1_irq_handler, FALLING);

        // esp_sleep_enable_ext0_wakeup(GPIO_NUM_13, 0);  // 0 = Low
        // rtc_gpio_pullup_en(GPIO_NUM_13);
        // Serial.println("Going to sleep now");
        // esp_deep_sleep_start();
    }
}
```

