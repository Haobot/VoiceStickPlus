// test_gateway_logic.c — gateway 纯逻辑模块 host 侧单测
//
// 覆盖 gateway_report_parser（8 字节报文→usage 集合 diff→沿）与
// gateway_keymap（小米 usage→标准 HID 动作翻译）。
// 报文格式依据 Phase 0 spike 真机结论（Doc/Plan/xiaomi-remote-stick-gateway.md §6.1）：
//   8 字节 = 2 字节头 + 3×LE16 usage 槽（0=空），松开帧全零。
// 用例事实源：back=0xF1/vol+=0x80/vol-=0x81 真机捕获；其余 13 键表见 usage-tap 文档 §1。
#include <stdio.h>
#include <string.h>

#include "gateway_keymap.h"
#include "gateway_report_parser.h"

static int s_failed = 0;
static int s_total = 0;

#define CHECK(cond, desc)                                              \
    do {                                                               \
        s_total++;                                                     \
        if (!(cond)) {                                                 \
            s_failed++;                                                \
            printf("  [失败] %s（第 %d 行）\n", desc, __LINE__);       \
        }                                                              \
    } while (0)

// 辅助：构造一帧报文（三槽 usage，0 表示空槽）
static void make_report(uint8_t out[8], uint16_t s0, uint16_t s1, uint16_t s2) {
    memset(out, 0, 8);
    out[2] = (uint8_t)(s0 & 0xFF);
    out[3] = (uint8_t)(s0 >> 8);
    out[4] = (uint8_t)(s1 & 0xFF);
    out[5] = (uint8_t)(s1 >> 8);
    out[6] = (uint8_t)(s2 & 0xFF);
    out[7] = (uint8_t)(s2 >> 8);
}

static void feed(gateway_report_parser_t *p, const uint8_t rep[8],
                 uint16_t pressed[3], size_t *pc, uint16_t released[3], size_t *rc) {
    int err = gateway_report_parser_feed(p, rep, 8, pressed, pc, released, rc);
    if (err != 0) {
        printf("  [失败] feed 返回 %d（应为 0）\n", err);
        s_failed++;
    }
    s_total++;
}

// ---------- gateway_report_parser ----------

static void test_parser_initial_empty_report(void) {
    printf("[用例] 初始态收到全零报文：不产生任何沿\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8] = {0};
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 0 && rc == 0, "空报文无沿");
}

static void test_parser_single_press(void) {
    printf("[用例] 单键按下：pressed 含该 usage\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8];
    make_report(rep, 0x00F1, 0, 0);  // back（真机捕获值）
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 1 && pressed[0] == 0x00F1, "back 按下沿");
    CHECK(rc == 0, "无松开沿");

    printf("[用例] 同报文重复：集合不变，无沿\n");
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 0 && rc == 0, "重复报文无沿");

    printf("[用例] 松开帧（全零）：released 含该 usage\n");
    uint8_t rel[8] = {0};
    feed(&p, rel, pressed, &pc, released, &rc);
    CHECK(rc == 1 && released[0] == 0x00F1, "back 松开沿");
    CHECK(pc == 0, "无按下沿");
}

static void test_parser_multi_key(void) {
    printf("[用例] 双键同按：一次报文 pressed 两个 usage\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8];
    make_report(rep, 0x0080, 0x0081, 0);  // vol+ + vol-
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 2, "两键按下");
    CHECK((pressed[0] == 0x0080 && pressed[1] == 0x0081) ||
              (pressed[0] == 0x0081 && pressed[1] == 0x0080),
          "集合含两 usage（顺序无关）");

    printf("[用例] 一键保持一键新增：pressed 仅新键\n");
    make_report(rep, 0x0080, 0x0081, 0x00F1);
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 1 && pressed[0] == 0x00F1, "仅新增键产生按下沿");
    CHECK(rc == 0, "无松开沿");

    printf("[用例] 一键保持一键松开：released 仅松开键\n");
    make_report(rep, 0x0080, 0, 0x00F1);  // vol- 松开
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(rc == 1 && released[0] == 0x0081, "仅消失键产生松开沿");
    CHECK(pc == 0, "无按下沿");
}

static void test_parser_header_ignored(void) {
    printf("[用例] 头两字节内容不影响解析（只认三槽）\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8];
    make_report(rep, 0x0052, 0, 0);
    rep[0] = 0xAB;  // 真机实测头为 00 00，但解析器不依赖头内容
    rep[1] = 0xCD;
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 1 && pressed[0] == 0x0052, "头部任意值仍解析出 up 按下");
}

static void test_parser_duplicate_usage_dedup(void) {
    printf("[用例] 同报重复 usage（异常输入）：集合语义去重，只产生一个沿\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8];
    make_report(rep, 0x0028, 0x0028, 0);
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 1, "重复 usage 去重后仅一个按下沿");

    printf("[用例] 松开：去重后仅一个松开沿\n");
    uint8_t rel[8] = {0};
    feed(&p, rel, pressed, &pc, released, &rc);
    CHECK(rc == 1, "仅一个松开沿");
}

static void test_parser_invalid_length(void) {
    printf("[用例] 长度不足 8：拒绝并保持状态\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t short_rep[7] = {0};
    uint16_t pressed[3], released[3];
    size_t pc = 9, rc = 9;  // 哨兵值：失败路径不应改动
    int err = gateway_report_parser_feed(&p, short_rep, 7, pressed, &pc, released, &rc);
    CHECK(err != 0, "短报文返回错误");
    CHECK(pc == 9 && rc == 9, "失败路径不污染输出参数");

    printf("[用例] NULL 指针：返回错误不崩溃\n");
    err = gateway_report_parser_feed(NULL, short_rep, 7, pressed, &pc, released, &rc);
    CHECK(err != 0, "NULL 解析器返回错误");
    err = gateway_report_parser_feed(&p, NULL, 8, pressed, &pc, released, &rc);
    CHECK(err != 0, "NULL 报文返回错误");
}

// ---------- gateway_keymap ----------

static void test_keymap_consumer_keys(void) {
    // 三键 + mute：键盘页非标 usage → 翻译为 Consumer 页标准 usage（R1 目标）
    printf("[用例] 三键翻译为标准 Consumer usage\n");
    struct {
        uint16_t in;
        uint16_t want;
        const char *name;
    } cases[] = {
        {0x00F1, 0x0224, "back→AC Back"},
        {0x0080, 0x00E9, "vol+→Volume Increment"},
        {0x0081, 0x00EA, "vol-→Volume Decrement"},
        {0x007F, 0x00E2, "mute→Mute"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gateway_key_action_t a = gateway_keymap_translate(cases[i].in);
        s_total++;
        if (a.kind != GATEWAY_KEY_CONSUMER || a.value != cases[i].want) {
            s_failed++;
            printf("  [失败] %s：得到 kind=%d value=0x%04x\n", cases[i].name, (int)a.kind, a.value);
        }
    }
}

static void test_keymap_keyboard_keys(void) {
    // 方向/ok/menu/home：小米报的是键盘页 usage，值域恰与 HID keycode 一致，直接透传
    printf("[用例] 方向/OK/菜单/Home 键盘页透传\n");
    struct {
        uint16_t in;
        uint16_t want;
        const char *name;
    } cases[] = {
        {0x0052, 0x52, "up"},
        {0x0051, 0x51, "down"},
        {0x0050, 0x50, "left"},
        {0x004F, 0x4F, "right"},
        {0x0028, 0x28, "ok→Enter"},
        {0x0065, 0x65, "menu→Application"},
        {0x004A, 0x4A, "home→Home"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gateway_key_action_t a = gateway_keymap_translate(cases[i].in);
        s_total++;
        if (a.kind != GATEWAY_KEY_KEYBOARD || a.value != cases[i].want) {
            s_failed++;
            printf("  [失败] %s：得到 kind=%d value=0x%04x\n", cases[i].name, (int)a.kind, a.value);
        }
    }
}

static void test_keymap_intercept_keys(void) {
    printf("[用例] 截留键：语音/电源/tv/未知 usage 不向目标设备输出\n");
    uint16_t intercepts[] = {
        0x003E,  // 语音键（键盘页 F5；桌面端直连即识别为 F5，待真机复核 usage 值）
        0x0066,  // power：防误关机
        0x0035,  // tv
        0x0299,  // 未知 usage
    };
    for (size_t i = 0; i < sizeof(intercepts) / sizeof(intercepts[0]); i++) {
        gateway_key_action_t a = gateway_keymap_translate(intercepts[i]);
        s_total++;
        if (a.kind != GATEWAY_KEY_INTERCEPT) {
            s_failed++;
            printf("  [失败] usage 0x%04x 应截留，得到 kind=%d\n", intercepts[i], (int)a.kind);
        }
    }
}

int main(void) {
    test_parser_initial_empty_report();
    test_parser_single_press();
    test_parser_multi_key();
    test_parser_header_ignored();
    test_parser_duplicate_usage_dedup();
    test_parser_invalid_length();
    test_keymap_consumer_keys();
    test_keymap_keyboard_keys();
    test_keymap_intercept_keys();

    printf("\n结果：%d/%d 通过\n", s_total - s_failed, s_total);
    if (s_failed == 0) {
        printf("全部通过\n");
        return 0;
    }
    return 1;
}
