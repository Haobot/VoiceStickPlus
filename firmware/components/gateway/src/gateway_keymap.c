// gateway_keymap.c — 翻译表实现（纯逻辑，无 ESP-IDF 依赖）
//
// usage 事实源：13 键表 Doc/Plan/xiaomi-remote-usage-tap.md §1（Raw Input 实测互证）；
// 语音键 0x003E 为桌面端 F5 识别反推值，Phase 1 真机全键复核后如不符在此修正。
#include "gateway_keymap.h"

#include <string.h>

// Consumer 页标准 usage（翻译目标，全平台原生识别）
#define CONSUMER_AC_BACK 0x0224
#define CONSUMER_VOLUME_INCREMENT 0x00E9
#define CONSUMER_VOLUME_DECREMENT 0x00EA
#define CONSUMER_MUTE 0x00E2

// 小米键盘页 usage（真机实测）
#define XIAOMI_BACK 0x00F1
#define XIAOMI_VOLUME_UP 0x0080
#define XIAOMI_VOLUME_DOWN 0x0081
#define XIAOMI_VOLUME_MUTE 0x007F
#define XIAOMI_VOICE 0x003E  // F5 反推，待真机复核
#define XIAOMI_POWER 0x0066
#define XIAOMI_TV 0x0035

// 可软件路由键表（语音键除外）：usage + 协议 key 名（与桌面端 keymap 配置一致）
typedef struct {
    uint16_t usage;
    const char *name;
} gateway_key_def_t;
static const gateway_key_def_t k_routable_keys[GATEWAY_KEYMAP_ROUTABLE_COUNT] = {
    {0x0028, "ok"},          {0x004F, "right"},        {0x0050, "left"},
    {0x0051, "down"},        {0x0052, "up"},           {0x0065, "menu"},
    {0x004A, "home"},        {XIAOMI_BACK, "back"},    {XIAOMI_VOLUME_UP, "volume_up"},
    {XIAOMI_VOLUME_DOWN, "volume_down"}, {XIAOMI_VOLUME_MUTE, "volume_mute"},
    {XIAOMI_POWER, "power"}, {XIAOMI_TV, "tv"},
};

// 软件路由表（内存态；索引同 k_routable_keys，NVS 持久化由 main.c 负责）
static gateway_route_t s_routes[GATEWAY_KEYMAP_ROUTABLE_COUNT];

static int routable_index(uint16_t xiaomi_usage) {
    for (size_t i = 0; i < GATEWAY_KEYMAP_ROUTABLE_COUNT; i++) {
        if (k_routable_keys[i].usage == xiaomi_usage) {
            return (int)i;
        }
    }
    return -1;
}

int gateway_keymap_set_route(uint16_t xiaomi_usage, gateway_route_t route) {
    int idx = routable_index(xiaomi_usage);
    if (idx < 0) {
        return -1;  // 语音键/未知 usage 不可路由
    }
    s_routes[idx] = route;
    return 0;
}

gateway_route_t gateway_keymap_get_route(uint16_t xiaomi_usage) {
    int idx = routable_index(xiaomi_usage);
    return idx < 0 ? GATEWAY_ROUTE_PASSTHROUGH : s_routes[idx];
}

void gateway_keymap_reset_routes(void) {
    memset(s_routes, 0, sizeof(s_routes));
}

const char *gateway_keymap_key_name(uint16_t xiaomi_usage) {
    int idx = routable_index(xiaomi_usage);
    return idx < 0 ? NULL : k_routable_keys[idx].name;
}

size_t gateway_keymap_routable_key_count(void) {
    return GATEWAY_KEYMAP_ROUTABLE_COUNT;
}

uint16_t gateway_keymap_routable_usage_at(size_t index) {
    if (index >= GATEWAY_KEYMAP_ROUTABLE_COUNT) {
        return 0;
    }
    return k_routable_keys[index].usage;
}

gateway_key_action_t gateway_keymap_translate(uint16_t xiaomi_usage) {
    // 软件路由优先于直通/截留默认（语音键不在路由表，恒走 ATVV 截留语义）
    if (gateway_keymap_get_route(xiaomi_usage) == GATEWAY_ROUTE_SOFTWARE) {
        return (gateway_key_action_t){GATEWAY_KEY_SOFTWARE, xiaomi_usage};
    }

    switch (xiaomi_usage) {
        // 键盘页 usage 值 == HID keycode，直接透传
        case 0x0028:  // ok → Enter
        case 0x004F:  // right
        case 0x0050:  // left
        case 0x0051:  // down
        case 0x0052:  // up
        case 0x0065:  // menu → Application 键
        case 0x004A:  // home → Home 键
            return (gateway_key_action_t){GATEWAY_KEY_KEYBOARD, xiaomi_usage};

        // 键盘页非标 usage → Consumer 页标准 usage（R1：三键全平台识别）
        case XIAOMI_BACK:
            return (gateway_key_action_t){GATEWAY_KEY_CONSUMER, CONSUMER_AC_BACK};
        case XIAOMI_VOLUME_UP:
            return (gateway_key_action_t){GATEWAY_KEY_CONSUMER, CONSUMER_VOLUME_INCREMENT};
        case XIAOMI_VOLUME_DOWN:
            return (gateway_key_action_t){GATEWAY_KEY_CONSUMER, CONSUMER_VOLUME_DECREMENT};
        case XIAOMI_VOLUME_MUTE:
            return (gateway_key_action_t){GATEWAY_KEY_CONSUMER, CONSUMER_MUTE};

        // 截留键与未知 usage：不出网关
        case XIAOMI_VOICE:
        case XIAOMI_POWER:
        case XIAOMI_TV:
        default:
            return (gateway_key_action_t){GATEWAY_KEY_INTERCEPT, 0};
    }
}
