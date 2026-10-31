// D7 回归（宿主编译，零 ESP 依赖）：
// ota_abort 在 id 不匹配时必须拒绝——原实现跳过 esp_ota_abort 却照旧清状态+发
// aborted，进行中的传输被悄悄废掉而 esp_ota_handle 泄漏，桌面端拿到假 aborted。
#include "voice_ble_ota_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    // 1) 无活动传输 → 允许（幂等收尾，桌面取消流程不因二次 abort 挂住）
    assert(voice_ble_ota_abort_allowed(false, 1u, 2u) == true);
    assert(voice_ble_ota_abort_allowed(false, 0u, 0u) == true);

    // 2) id 匹配 → 允许（正常中止）
    assert(voice_ble_ota_abort_allowed(true, 7u, 7u) == true);
    assert(voice_ble_ota_abort_allowed(true, 0u, 0u) == true);

    // 3) **有活动传输且 id 不匹配 → 拒绝**（D7 核心：不清理、不发假 aborted）
    assert(voice_ble_ota_abort_allowed(true, 8u, 7u) == false);
    assert(voice_ble_ota_abort_allowed(true, 0u, 7u) == false);
    assert(voice_ble_ota_abort_allowed(true, 7u, 8u) == false);

    printf("voice_ble_ota_policy_test: ALL PASS\n");
    return 0;
}
