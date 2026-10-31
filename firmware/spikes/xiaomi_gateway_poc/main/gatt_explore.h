// gatt_explore.h — Phase 0 spike：连接建立后的 GATT 数据库探索器
//
// 职责（对应 Doc/Plan/xiaomi-remote-stick-gateway.md §6 spike 清单第 4/5/6 项）：
//   4. 全库枚举服务/特征，定位 HID 服务(0x1812)并读取 Report Map(0x2A4B) 打印 hex
//   5. 定位 Report 特征(0x2A4D)，写 CCCD 使能 notify（真实按键报文由 main 收到后转发回这里解码）
//   6. 核对 ATVV 服务(AB5E0001-…)及其 TX/Audio/Control 三个特征是否齐全
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 在连接（含加密恢复）成功后启动一次全库探索；内部为异步回调链，不可重入
void gatt_explore_start(uint16_t conn_handle);

// 探索结果：Report(0x2A4D) 特征值句柄；0 表示未找到（main 据此区分 notify 归属）
uint16_t gatt_explore_report_handle(void);

// main 的 GAP_EVENT_NOTIFY_RX 转发入口：打印原始 hex；命中 Report 句柄时附 usage 解码
void gatt_dump_notify(uint16_t attr_handle, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
