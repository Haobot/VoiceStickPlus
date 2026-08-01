#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 功耗记账模式。与 main.c 电源状态机对应；录音/广播/OTA 为高耗电叠加态。
typedef enum {
    POWER_MODE_S0_ACTIVE = 0,   // 亮屏交互（背光 32）
    POWER_MODE_S1_RESTING,      // 暗屏待机（背光 8）
    POWER_MODE_S2_SCREEN_OFF,   // 熄屏保连
    POWER_MODE_S3_POWER_OFF,    // 关机段（时长由 RTC RAM 锚点/时间锚点补全）
    POWER_MODE_RECORDING,       // 录音会话（含 BLE 音频流）
    POWER_MODE_ADVERTISING,     // 断连广播中
    POWER_MODE_OTA,             // BLE OTA
    POWER_MODE_COUNT
} power_mode_t;

// 初始化：挂载 storage SPIFFS、加载环形文件、恢复 RTC RAM 关机段锚点、
// 启动 60s VBAT 周期采样与 10 分钟 flush 定时器。
// 须在 stick_s3_board_init() 之后调用。SPIFFS 挂载失败时降级为仅 RAM 记录并 ESP_LOGW。
esp_err_t power_log_init(void);

// 模式切换事件：记录进入新模式的时刻（重复模式不重复记录）。
// POWER_MODE_S3_POWER_OFF 额外触发：写 M5PM1 RTC RAM 关机锚点 + 强制 flush。
void power_log_note_mode(power_mode_t mode);

// 时间锚点：主机下发 epoch 秒，记一条 mode=0xFF、flags.bit4=1 的锚点记录，
// reserved[0..3] 存 uint32 LE epoch 秒。分析脚本据此把 uptime 对齐到 wall clock。
void power_log_set_time_anchor(uint32_t epoch_s);

// 当前有效日志数据总字节数（含 16 字节逻辑流头）。
size_t power_log_size(void);

// 从逻辑日志流 offset 处拷贝最多 max 字节到 buf，返回实际字节数。
// 逻辑流格式（按时间顺序）：16 字节头（'P','W','R','L', 版本=1, entry_size=12, 保留,
// uint32 LE 有效条目数, uint32 LE 回卷计数），随后每条目 12 字节 packed：
// uint32 LE uptime_s, uint16 LE vbat_mv, uint8 mode, uint8 flags, uint8 reserved[4]。
// flags: bit0=充电中, bit1=USB供电, bit2=周期采样, bit3=关机段恢复记录, bit4=时间锚点。
size_t power_log_read(size_t offset, uint8_t *buf, size_t max);

// 清空全部日志（RAM + SPIFFS 文件），回卷计数递增。
void power_log_clear(void);

#ifdef __cplusplus
}
#endif
