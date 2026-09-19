// gateway_targets.h — 网关目标表（P1 切换器）：谁可以成为当前目标。
//
// 目标 = 一台与本机建立过 BLE 连接/bond 的桌面端（桌面端是 central，本机是 peripheral）。
// 稳定标识取「对端 identity address + 地址类型」：Windows 用 RPA 时地址会轮换，但
// identity address 在 bond 生命周期内稳定，重启/重连后仍命中同一条目。
//
// 分层：gateway_targets_core_* 为**纯逻辑**（无 ESP-IDF 依赖，进 host 单测）；
// gateway_targets_* 为设备侧薄封装（NVS 持久化 + 当前连接对端跟踪）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GATEWAY_TARGET_MAX 4
#define GATEWAY_TARGET_NAME_MAX 24

typedef struct {
    uint8_t id_addr[6];                  // 对端 identity address（NimBLE val 字节序）
    uint8_t addr_type;                   // 0=public 1=random（BLE_ADDR_*）
    char name[GATEWAY_TARGET_NAME_MAX];  // 空串=尚未命名（桌面端连上后上报主机名）
    uint32_t last_seen_s;                // 最近一次连接时刻（秒，esp_log_timestamp()/1000）
} gateway_target_t;

typedef struct {
    gateway_target_t items[GATEWAY_TARGET_MAX];
    int count;
} gateway_target_table_t;

// ---- 纯逻辑核心（host 单测覆盖） ----

void gateway_targets_core_init(gateway_target_table_t *table);

// 查找：返回下标，未找到返回 -1。
int gateway_targets_core_find(const gateway_target_table_t *table, const uint8_t id_addr[6],
                              uint8_t addr_type);

// 记录一次「该目标刚连上」：已存在则只刷新 last_seen_s；否则新增；
// 表满时淘汰 last_seen_s 最小（最旧）的一条，保证新目标一定能进表。
// 返回该目标在表内的下标，失败返回 -1。
int gateway_targets_core_note(gateway_target_table_t *table, const uint8_t id_addr[6],
                              uint8_t addr_type, uint32_t now_s);

// 设置名字（桌面端上报主机名）。名字为空或过长（>= GATEWAY_TARGET_NAME_MAX）视为无效。
bool gateway_targets_core_set_name(gateway_target_table_t *table, const uint8_t id_addr[6],
                                   uint8_t addr_type, const char *name);

// 删除目标（bond 被删除时）。返回是否真的删掉了。
bool gateway_targets_core_remove(gateway_target_table_t *table, const uint8_t id_addr[6],
                                 uint8_t addr_type);

// 显示名：有名字用名字；否则 "未命名-XXXX"（地址后两字节大写 hex），便于屏幕上区分。
void gateway_targets_core_display_name(const gateway_target_t *item, char *out, size_t out_len);

// ---- 设备侧封装（NVS 持久化） ----

// 从 NVS 读取目标表（无记录则空表）。
void gateway_targets_init(void);

// 当前内存中的表（只读）。
const gateway_target_table_t *gateway_targets_table(void);

// 记录/命名/删除 + 落盘。返回表内下标（note/set_name），-1/-2 表示失败。
int gateway_targets_note_peer(const uint8_t id_addr[6], uint8_t addr_type);
int gateway_targets_set_name(const uint8_t id_addr[6], uint8_t addr_type, const char *name);
bool gateway_targets_remove(const uint8_t id_addr[6], uint8_t addr_type);

// 当前连接（peripheral 侧）对端的 identity address：未连接或无记录时返回 false。
bool gateway_targets_current_peer(uint8_t id_addr[6], uint8_t *addr_type);

// 由 voice_ble 的对端回调写入（连接/断连时更新）。
void gateway_targets_set_current_peer(bool connected, const uint8_t id_addr[6], uint8_t addr_type);
