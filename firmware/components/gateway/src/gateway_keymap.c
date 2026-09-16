// gateway_keymap.c — 翻译表实现（纯逻辑，无 ESP-IDF 依赖）
//
// usage 事实源：13 键表 Doc/Plan/xiaomi-remote-usage-tap.md §1（Raw Input 实测互证）；
// 语音键 0x003E 为桌面端 F5 识别反推值，Phase 1 真机全键复核后如不符在此修正。
#include "gateway_keymap.h"

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

gateway_key_action_t gateway_keymap_translate(uint16_t xiaomi_usage) {
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
