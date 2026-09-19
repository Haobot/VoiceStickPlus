// gateway_targets.c — 目标表的设备侧封装（NVS 持久化 + 当前对端跟踪）。
//
// 纯逻辑（增删改查、满表淘汰、显示名）在 gateway_targets_core.c，可被 host 单测覆盖；
// 本文件只负责落盘与「当前连接对端是谁」这一运行期状态。
#include "gateway_targets.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define TAG "gw_targets"
#define NVS_NS "gateway"
#define NVS_KEY_TARGETS "targets"
#define TARGETS_BLOB_VERSION 1u

// 落盘格式：version + count + items[]（定长，便于整体读写与版本演进）。
typedef struct {
    uint32_t version;
    uint32_t count;
    gateway_target_t items[GATEWAY_TARGET_MAX];
} targets_blob_t;

static gateway_target_table_t s_table;
static bool s_current_peer_valid;
static gateway_target_t s_current_peer;  // 只用 id_addr/addr_type 两字段

static void save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, targets not persisted");
        return;
    }
    targets_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = TARGETS_BLOB_VERSION;
    blob.count = (uint32_t)s_table.count;
    memcpy(blob.items, s_table.items, sizeof(s_table.items));
    esp_err_t err = nvs_set_blob(h, NVS_KEY_TARGETS, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "targets save failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

void gateway_targets_init(void)
{
    gateway_targets_core_init(&s_table);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;  // 无记录：空表
    }
    targets_blob_t blob;
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, NVS_KEY_TARGETS, &blob, &len) == ESP_OK && len == sizeof(blob) &&
        blob.version == TARGETS_BLOB_VERSION && blob.count <= GATEWAY_TARGET_MAX) {
        s_table.count = (int)blob.count;
        memcpy(s_table.items, blob.items, sizeof(s_table.items));
        ESP_LOGI(TAG, "loaded %d target(s)", s_table.count);
    }
    nvs_close(h);
}

const gateway_target_table_t *gateway_targets_table(void) { return &s_table; }

int gateway_targets_note_peer(const uint8_t id_addr[6], uint8_t addr_type)
{
    const uint32_t now_s = (uint32_t)(esp_log_timestamp() / 1000u);
    const int index = gateway_targets_core_note(&s_table, id_addr, addr_type, now_s);
    if (index >= 0) {
        save_locked();
    }
    return index;
}

int gateway_targets_set_name(const uint8_t id_addr[6], uint8_t addr_type, const char *name)
{
    if (!gateway_targets_core_set_name(&s_table, id_addr, addr_type, name)) {
        return -1;
    }
    save_locked();
    ESP_LOGI(TAG, "target named: %s", name);
    return gateway_targets_core_find(&s_table, id_addr, addr_type);
}

bool gateway_targets_remove(const uint8_t id_addr[6], uint8_t addr_type)
{
    if (!gateway_targets_core_remove(&s_table, id_addr, addr_type)) {
        return false;
    }
    save_locked();
    ESP_LOGI(TAG, "target removed, count=%d", s_table.count);
    return true;
}

bool gateway_targets_current_peer(uint8_t id_addr[6], uint8_t *addr_type)
{
    if (!s_current_peer_valid) {
        return false;
    }
    memcpy(id_addr, s_current_peer.id_addr, 6);
    if (addr_type) {
        *addr_type = s_current_peer.addr_type;
    }
    return true;
}

void gateway_targets_set_current_peer(bool connected, const uint8_t id_addr[6], uint8_t addr_type)
{
    s_current_peer_valid = connected;
    if (connected && id_addr) {
        memset(&s_current_peer, 0, sizeof(s_current_peer));
        memcpy(s_current_peer.id_addr, id_addr, 6);
        s_current_peer.addr_type = addr_type;
    }
}
