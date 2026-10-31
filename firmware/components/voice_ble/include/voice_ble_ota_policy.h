#pragma once

// D7：OTA abort 作用判定（纯逻辑、零依赖，宿主编译单测见 test/）。
//
// 原实现的缺陷：id 不匹配时跳过 esp_ota_abort，却**照旧** ota_clear_state() +
// 发 {event:aborted} + 触发 ABORT 回调——进行中的传输被悄悄废掉而 esp_ota_handle
// 泄漏，桌面端还拿到假 aborted 帧误判已中止。
//
// 语义：
//   - 无活动传输 → 允许（幂等收尾：仍回 aborted，桌面取消流程不因二次 abort 挂住）
//   - id 匹配   → 允许（正常中止）
//   - **有活动传输且 id 不匹配 → 拒绝**：调用方必须返回错误且不清理、不发帧
#include <stdbool.h>
#include <stdint.h>

static inline bool voice_ble_ota_abort_allowed(bool active, uint32_t incoming,
                                              uint32_t current_transfer)
{
    if (!active) return true;
    return incoming == current_transfer;
}
