#pragma once

// voice_ble 入站连接表（A4 深修，9-22 评审 §366 / 跟进 backlog）：
// 按 conn_handle 记账入站链路，替换「任一链路断连即全清」的单值语义
//（Doc/Expe/gateway-p1-ota-session-2026-09-20.md 事故：旧连接的 stale DISCONNECT
// 迟到会清掉新连接的订阅态 → 桌面端 state 发送静默失败）。
// 纯 C、零 ESP/NimBLE 依赖——宿主编译即可单测（test/conn_table_test.c）。

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ≥ CONFIG_BT_NIMBLE_MAX_CONNECTIONS(3)：留 1 槽余量；宿主测试同值。
#define VOICE_BLE_CONN_MAX 4
// 与 NimBLE BLE_HS_CONN_HANDLE_NONE 同值（0xFFFF）。
#define VOICE_BLE_CONN_NONE 0xFFFFu

typedef struct {
    uint16_t handle;
    uint32_t connected_ms;  // 建立时间戳（断连时长日志用）
    bool used;
    bool audio_sub;         // audio_tx CCCD 订阅态
    bool state_sub;         // state_tx CCCD 订阅态
} voice_ble_conn_t;

typedef struct {
    voice_ble_conn_t entries[VOICE_BLE_CONN_MAX];
} voice_ble_conn_table_t;

// 静态零值即已初始化；显式提供以便测试。
void voice_ble_conn_table_init(voice_ble_conn_table_t *table);

int voice_ble_conn_table_count(const voice_ble_conn_table_t *table);

// 新增连接。已存在视为幂等成功（不重置订阅态），out_first 置「表从 0→1」；
// 表满返回 false。
bool voice_ble_conn_table_add(voice_ble_conn_table_t *table, uint16_t handle,
                              uint32_t connected_ms, bool *out_first);

// 按 handle 移除**本条**连接（stale 断连只清自己）；找不到返回 false。
// out_last 置「表→0」（调用方据此决定是否发归零回调/重启广播）。
bool voice_ble_conn_table_remove(voice_ble_conn_table_t *table, uint16_t handle,
                                 bool *out_last);

voice_ble_conn_t *voice_ble_conn_table_find(voice_ble_conn_table_t *table,
                                            uint16_t handle);

// SUBSCRIBE 防御性补录：连接不存在则新增（乱序事件），存在则原样返回；表满 NULL。
voice_ble_conn_t *voice_ble_conn_table_touch(voice_ble_conn_table_t *table,
                                             uint16_t handle,
                                             uint32_t connected_ms);

// 应用链路（桌面 app）= 已订阅 state/audio 特征的连接，state 订阅者优先；
// OS-HID 中央只订 HID 服务不入选。无则 VOICE_BLE_CONN_NONE。
uint16_t voice_ble_conn_table_app_handle(const voice_ble_conn_table_t *table);

#ifdef __cplusplus
}
#endif
