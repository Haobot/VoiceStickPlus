// test_gateway_atvv.c — gateway ATVV 纯逻辑模块 host 侧单测
//
// 覆盖 gateway_adpcm（IMA ADPCM 解码 / 120B 帧累积 / 640 采样切片，与桌面端
// C++ 实现互为金标准）与 gateway_atvv_session（caps 握手 / 会话沿 / 尾包宽限 /
// 重开拒绝窗，蓝本 xiaomi_atvv_session.cc 的协议层裁剪版）。
// ADPCM 期望值按 IMA/DVI 1992 标准算法离线手推；步长表源自
// desktop/windows/src/ima_adpcm_decoder.cc。
#include <stdio.h>
#include <string.h>

#include "gateway_adpcm.h"
#include "gateway_atvv_session.h"

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

// ---------- gateway_adpcm：解码器 ----------

static void test_adpcm_silence_from_zero(void) {
    printf("[用例] ADPCM：Reset(0,0) 输入 0x00 → 两个零样本\n");
    gateway_adpcm_state_t st;
    gateway_adpcm_reset(&st, 0, 0);
    int16_t out[8];
    size_t n = gateway_adpcm_decode(&st, (const uint8_t[]){0x00}, 1, out, 8);
    CHECK(n == 2, "产出 2 样本");
    CHECK(out[0] == 0 && out[1] == 0, "全零输入零样本");
    CHECK(st.step_index == 0, "步长索引钳在 0");
}

static void test_adpcm_negative_ramp(void) {
    printf("[用例] ADPCM：Reset(0,0) 输入 0xFF → [-11, -41]，idx=16\n");
    gateway_adpcm_state_t st;
    gateway_adpcm_reset(&st, 0, 0);
    int16_t out[8];
    size_t n = gateway_adpcm_decode(&st, (const uint8_t[]){0xFF}, 1, out, 8);
    CHECK(n == 2, "产出 2 样本");
    CHECK(out[0] == -11, "首样本 -11（高半字节，step=7，diff=11，负号位）");
    CHECK(out[1] == -41, "次样本 -41（step=16，diff=30）");
    CHECK(st.step_index == 16, "步长索引 0+8=16");
    CHECK(st.predictor == -41, "predictor 延续");
}

static void test_adpcm_positive_step(void) {
    printf("[用例] ADPCM：Reset(0,0) 输入 0x55 → [8, 22]，idx=8\n");
    gateway_adpcm_state_t st;
    gateway_adpcm_reset(&st, 0, 0);
    int16_t out[8];
    size_t n = gateway_adpcm_decode(&st, (const uint8_t[]){0x55}, 1, out, 8);
    CHECK(n == 2, "产出 2 样本");
    CHECK(out[0] == 8, "首样本 8（step=7，diff=1+7）");
    CHECK(out[1] == 22, "次样本 22（step=11，diff=1+2+11）");
    CHECK(st.step_index == 8, "步长索引 0+4=8");
}

static void test_adpcm_multibyte_continuity(void) {
    printf("[用例] ADPCM：跨字节状态连续 [0xFF,0x00] → [-11,-41,-37,-34]\n");
    gateway_adpcm_state_t st;
    gateway_adpcm_reset(&st, 0, 0);
    int16_t out[8];
    size_t n = gateway_adpcm_decode(&st, (const uint8_t[]){0xFF, 0x00}, 2, out, 8);
    CHECK(n == 4, "产出 4 样本");
    CHECK(out[0] == -11 && out[1] == -41 && out[2] == -37 && out[3] == -34,
          "第二字节延续 predictor/step（step[16]=34→diff 4，step[15]=31→diff 3）");
}

static void test_adpcm_nonzero_reset(void) {
    printf("[用例] ADPCM：Reset(-100,40) 输入 0x00 → [-58, -20]\n");
    gateway_adpcm_state_t st;
    gateway_adpcm_reset(&st, -100, 40);
    int16_t out[8];
    size_t n = gateway_adpcm_decode(&st, (const uint8_t[]){0x00}, 1, out, 8);
    CHECK(n == 2, "产出 2 样本");
    CHECK(out[0] == -58, "首样本 -100+337/8=-58（step 表 [40]=337）");
    CHECK(out[1] == -20, "次样本 -58+307/8=-20（step 表 [39]=307）");
}

static void test_adpcm_saturation_clamp(void) {
    printf("[用例] ADPCM：负满量程输入下 predictor 钳位 -32768、idx 钳位 88\n");
    gateway_adpcm_state_t st;
    gateway_adpcm_reset(&st, -32760, 88);
    int16_t out[4];
    size_t n = gateway_adpcm_decode(&st, (const uint8_t[]){0xFF}, 1, out, 4);
    CHECK(n == 2, "产出 2 样本");
    CHECK(out[0] == -32768 && out[1] == -32768, "双样本钳到 -32768");
    CHECK(st.step_index == 88, "步长索引钳在 88");
}

// ---------- gateway_adpcm：帧累积器 ----------

static void test_accumulator_fill_and_flush(void) {
    printf("[用例] 累积器：默认 120B，跨包凑满一帧\n");
    gateway_frame_accumulator_t acc;
    gateway_frame_accumulator_init(&acc);
    CHECK(acc.frame_bytes == GATEWAY_ADPCM_DEFAULT_FRAME_BYTES, "默认帧长 120");
    uint8_t big[100];
    memset(big, 0xAB, sizeof(big));
    uint8_t frames[512];
    size_t fn = gateway_frame_accumulator_append(&acc, big, 100, frames, sizeof(frames));
    CHECK(fn == 0, "100B 不满一帧");
    CHECK(gateway_frame_accumulator_pending(&acc) == 100, "暂存 100B");
    uint8_t tail[20];
    memset(tail, 0xCD, sizeof(tail));
    fn = gateway_frame_accumulator_append(&acc, tail, 20, frames, sizeof(frames));
    CHECK(fn == 1, "补 20B 凑满一帧");
    CHECK(frames[0] == 0xAB && frames[99] == 0xAB && frames[100] == 0xCD,
          "帧内容跨包拼接有序");
    CHECK(gateway_frame_accumulator_pending(&acc) == 0, "100+20=120 整帧后暂存清零");
}

static void test_accumulator_multi_frame(void) {
    printf("[用例] 累积器：单包 250B → 2 帧整 + 10B 余\n");
    gateway_frame_accumulator_t acc;
    gateway_frame_accumulator_init(&acc);
    uint8_t data[250];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    uint8_t frames[512];
    size_t fn = gateway_frame_accumulator_append(&acc, data, 250, frames, sizeof(frames));
    CHECK(fn == 2, "250/120=2 帧整");
    CHECK(gateway_frame_accumulator_pending(&acc) == 10, "余 10B");
    CHECK(frames[0] == 0 && frames[119] == 119 && frames[120] == 120 && frames[239] == 239,
          "两帧平铺连续");
}

static void test_accumulator_set_frame_bytes(void) {
    printf("[用例] 累积器：协商帧长清空残量并钳位\n");
    gateway_frame_accumulator_t acc;
    gateway_frame_accumulator_init(&acc);
    uint8_t frames[512];
    gateway_frame_accumulator_append(&acc, (const uint8_t[]){1, 2, 3}, 3, frames, sizeof(frames));
    CHECK(gateway_frame_accumulator_pending(&acc) == 3, "先攒 3B");
    gateway_frame_accumulator_set_frame_bytes(&acc, 40);
    CHECK(acc.frame_bytes == 40, "协商帧长生效");
    CHECK(gateway_frame_accumulator_pending(&acc) == 0, "残量清空");
    gateway_frame_accumulator_set_frame_bytes(&acc, 0);
    CHECK(acc.frame_bytes == GATEWAY_ADPCM_DEFAULT_FRAME_BYTES, "0 回退默认 120");
    gateway_frame_accumulator_set_frame_bytes(&acc, 9999);
    CHECK(acc.frame_bytes == GATEWAY_ADPCM_MAX_FRAME_BYTES, "超上限钳位 256");
}

static void test_accumulator_out_cap_insufficient(void) {
    printf("[用例] 累积器：out 容不足时多余帧整体丢弃不切半帧\n");
    gateway_frame_accumulator_t acc;
    gateway_frame_accumulator_init(&acc);
    uint8_t data[240];
    memset(data, 0x11, sizeof(data));
    uint8_t frames[150];  // 只容 1 帧
    size_t fn = gateway_frame_accumulator_append(&acc, data, 240, frames, sizeof(frames));
    CHECK(fn == 1, "只吐 1 帧");
    CHECK(gateway_frame_accumulator_pending(&acc) == 120,
          "第二帧完整保留在暂存（不切半帧）");
}

// ---------- gateway_adpcm：PCM 切片器 ----------

static void test_slicer_fill_and_flush(void) {
    printf("[用例] 切片器：640 采样攒满吐帧，余量跨调用保留\n");
    gateway_pcm_slicer_t sl;
    gateway_pcm_slicer_init(&sl);
    int16_t pcm[1400];
    for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++) pcm[i] = (int16_t)i;
    int16_t out[1400];
    size_t fn = gateway_pcm_slicer_append(&sl, pcm, 600, out, sizeof(out) / sizeof(out[0]));
    CHECK(fn == 0, "600 样本不满帧");
    CHECK(gateway_pcm_slicer_pending(&sl) == 600, "暂存 600");
    fn = gateway_pcm_slicer_append(&sl, pcm + 600, 680, out, sizeof(out) / sizeof(out[0]));
    CHECK(fn == 2, "600+680=1280 → 2 帧");
    CHECK(gateway_pcm_slicer_pending(&sl) == 0, "1280=2×640 整除无余");
    CHECK(out[0] == 0 && out[639] == 639 && out[640] == 640, "帧内容有序");
}

static void test_slicer_take_remainder(void) {
    printf("[用例] 切片器：take_remainder 取余量并清空\n");
    gateway_pcm_slicer_t sl;
    gateway_pcm_slicer_init(&sl);
    int16_t pcm[700];
    for (int i = 0; i < 700; i++) pcm[i] = (int16_t)(i + 1);
    int16_t out[700];
    gateway_pcm_slicer_append(&sl, pcm, 700, out, 700);
    CHECK(gateway_pcm_slicer_pending(&sl) == 60, "700-640=60 余量");
    int16_t rem[64];
    size_t rn = gateway_pcm_slicer_take_remainder(&sl, rem, 64);
    CHECK(rn == 60, "取 60 样本");
    CHECK(rem[0] == 641 && rem[59] == 700, "余量是帧后第 641~700 样本");
    CHECK(gateway_pcm_slicer_pending(&sl) == 0, "取后清空");
    rn = gateway_pcm_slicer_take_remainder(&sl, rem, 64);
    CHECK(rn == 0, "再次取为空");
}

// ---------- gateway_atvv_session：辅助 ----------

static gateway_atvv_session_t g_s;
static gateway_atvv_action_t g_acts[GATEWAY_ATVV_MAX_ACTIONS];

static void session_setup(void) {
    memset(&g_s, 0, sizeof(g_s));
    gateway_atvv_session_reset(&g_s);
    memset(g_acts, 0, sizeof(g_acts));
}

// 找第一个指定类型的动作，找不到返回 NULL
static const gateway_atvv_action_t *find_action(size_t n, gateway_atvv_action_kind_t kind) {
    for (size_t i = 0; i < n; i++) {
        if (g_acts[i].kind == kind) return &g_acts[i];
    }
    return NULL;
}

static size_t count_action(size_t n, gateway_atvv_action_kind_t kind) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (g_acts[i].kind == kind) c++;
    }
    return c;
}

// 驱动到 READY（Start + CAPS 应答），返回动作数
static size_t drive_to_ready(int64_t now_ms) {
    size_t n = gateway_atvv_session_start(&g_s, now_ms, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    uint8_t caps[] = {0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78};  // v1.0/16k/120B
    n = gateway_atvv_session_control(&g_s, caps, sizeof(caps), now_ms + 10, g_acts,
                                     GATEWAY_ATVV_MAX_ACTIONS);
    return n;
}

// 驱动到 STREAMING（READY 后喂 MIC_OPEN），返回 MIC_OPEN 处理的动作数
static size_t drive_to_streaming(int64_t now_ms) {
    drive_to_ready(now_ms);
    uint8_t mic_open[] = {0x08};
    return gateway_atvv_session_control(&g_s, mic_open, sizeof(mic_open), now_ms + 20,
                                        g_acts, GATEWAY_ATVV_MAX_ACTIONS);
}

// 喂 3 个 120B 全零 ADPCM 帧 → 720 零样本 → 1 个 640 采样 PCM 帧
static size_t feed_zero_audio(int64_t now_ms) {
    uint8_t frame[GATEWAY_ADPCM_DEFAULT_FRAME_BYTES];
    memset(frame, 0, sizeof(frame));
    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        n = gateway_atvv_session_audio(&g_s, frame, sizeof(frame), now_ms, g_acts,
                                       GATEWAY_ATVV_MAX_ACTIONS);
    }
    return n;
}

// ---------- gateway_atvv_session：caps 握手 ----------

static void test_session_start_gets_caps(void) {
    printf("[用例] 会话：Start 产 GET_CAPS(0A 01 00 00 03 03)，状态 CAPS_REQUESTED\n");
    session_setup();
    size_t n = gateway_atvv_session_start(&g_s, 100, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(n == 1, "1 个动作");
    const gateway_atvv_action_t *tx = find_action(n, GATEWAY_ATVV_ACTION_WRITE_TX);
    CHECK(tx != NULL, "有 WRITE_TX");
    CHECK(tx->tx_len == 6, "GET_CAPS 6 字节");
    static const uint8_t expect[6] = {0x0A, 0x01, 0x00, 0x00, 0x03, 0x03};
    CHECK(tx->tx_len == 6 && memcmp(tx->tx, expect, 6) == 0, "GET_CAPS 字节与桌面端一致");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_CAPS_REQUESTED, "状态 CAPS_REQUESTED");

    printf("[用例] 会话：非 IDLE 重复 Start 无动作\n");
    n = gateway_atvv_session_start(&g_s, 120, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(n == 0, "重复 Start 返回 0");
}

static void test_session_caps_ok_and_timeout(void) {
    printf("[用例] 会话：CAPS v1.0/16kHz/120B → READY，帧长协商生效\n");
    session_setup();
    size_t n = drive_to_ready(100);
    CHECK(g_s.state == GATEWAY_ATVV_STATE_READY, "状态 READY");
    CHECK(g_s.legacy_layout == false, "v1.0 非旧布局");
    CHECK(g_s.frame_bytes == 120, "帧长 0x0078=120");
    (void)n;

    printf("[用例] 会话：CAPS 2s 超时 → ERROR(caps_timeout)\n");
    session_setup();
    gateway_atvv_session_start(&g_s, 100, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    n = gateway_atvv_session_tick(&g_s, 100 + GATEWAY_ATVV_CAPS_TIMEOUT_MS - 1, g_acts,
                                  GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.state == GATEWAY_ATVV_STATE_CAPS_REQUESTED, "差 1ms 不超时");
    n = gateway_atvv_session_tick(&g_s, 100 + GATEWAY_ATVV_CAPS_TIMEOUT_MS, g_acts,
                                  GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.state == GATEWAY_ATVV_STATE_ERROR, "到点转 ERROR");
    const gateway_atvv_action_t *err = find_action(n, GATEWAY_ATVV_ACTION_ERROR);
    CHECK(err != NULL && strcmp(err->error_code, "caps_timeout") == 0, "错误码 caps_timeout");
}

static void test_session_caps_reject_8khz(void) {
    printf("[用例] 会话：CAPS 仅 8kHz → ERROR(unsupported_codec)\n");
    session_setup();
    gateway_atvv_session_start(&g_s, 100, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    uint8_t caps8k[] = {0x0B, 0x01, 0x00, 0x01, 0x03, 0x00, 0x78};
    size_t n = gateway_atvv_session_control(&g_s, caps8k, sizeof(caps8k), 110, g_acts,
                                            GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.state == GATEWAY_ATVV_STATE_ERROR, "状态 ERROR");
    const gateway_atvv_action_t *err = find_action(n, GATEWAY_ATVV_ACTION_ERROR);
    CHECK(err != NULL && strcmp(err->error_code, "unsupported_codec") == 0,
          "错误码 unsupported_codec");
}

static void test_session_caps_legacy_layout(void) {
    printf("[用例] 会话：CAPS 旧版布局（v<1.0，codecs=[4]）→ READY 且 legacy 生效\n");
    session_setup();
    gateway_atvv_session_start(&g_s, 100, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    uint8_t legacy[] = {0x0B, 0x00, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};
    size_t n = gateway_atvv_session_control(&g_s, legacy, sizeof(legacy), 110, g_acts,
                                            GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.state == GATEWAY_ATVV_STATE_READY, "状态 READY");
    CHECK(g_s.legacy_layout == true, "旧版布局标记");
    CHECK(g_s.frame_bytes == GATEWAY_ADPCM_DEFAULT_FRAME_BYTES, "旧版帧长默认 120");
    (void)n;

    printf("[用例] 会话：legacy 下 MIC_OPEN 的 ACK 补 codec 字节（0C 00 02）\n");
    uint8_t mic_open[] = {0x08};
    n = gateway_atvv_session_control(&g_s, mic_open, sizeof(mic_open), 120, g_acts,
                                     GATEWAY_ATVV_MAX_ACTIONS);
    const gateway_atvv_action_t *tx = find_action(n, GATEWAY_ATVV_ACTION_WRITE_TX);
    CHECK(tx != NULL && tx->tx_len == 3 && tx->tx[0] == 0x0C && tx->tx[1] == 0x00 &&
              tx->tx[2] == 0x02,
          "legacy ACK = 0C 00 02");
}

// ---------- gateway_atvv_session：语音键会话沿 ----------

static void test_session_mic_open_press_down(void) {
    printf("[用例] 会话：READY 收 MIC_OPEN(0x08) → ACK(0C 00) + PRESS_DOWN + STREAMING\n");
    session_setup();
    size_t n = drive_to_streaming(100);
    const gateway_atvv_action_t *tx = find_action(n, GATEWAY_ATVV_ACTION_WRITE_TX);
    CHECK(tx != NULL && tx->tx_len == 2 && tx->tx[0] == 0x0C && tx->tx[1] == 0x00,
          "v1.0 ACK = 0C 00");
    CHECK(find_action(n, GATEWAY_ATVV_ACTION_PRESS_DOWN) != NULL, "有 PRESS_DOWN");
    CHECK(count_action(n, GATEWAY_ATVV_ACTION_PRESS_UP) == 0, "无 PRESS_UP");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_STREAMING, "状态 STREAMING");
    CHECK(g_s.mic_open_remote == true, "远端 mic 打开标记");
}

static void test_session_stream_start_2pro(void) {
    printf("[用例] 会话：READY 收 0x04 一体帧 → PRESS_DOWN 无 ACK，codec 校验\n");
    session_setup();
    drive_to_ready(100);
    uint8_t start[] = {0x04, 0x00, 0x02, 0x05};  // interaction/16kHz/session=5
    size_t n = gateway_atvv_session_control(&g_s, start, sizeof(start), 120, g_acts,
                                            GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(find_action(n, GATEWAY_ATVV_ACTION_PRESS_DOWN) != NULL, "有 PRESS_DOWN");
    CHECK(count_action(n, GATEWAY_ATVV_ACTION_WRITE_TX) == 0, "2 Pro 直开无 ACK");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_STREAMING, "状态 STREAMING");
    CHECK(g_s.remote_session_id == 5, "会话计数透传缓存");

    printf("[用例] 会话：0x04 byte2 非 16kHz → ERROR(unsupported_codec)\n");
    session_setup();
    drive_to_ready(100);
    uint8_t bad[] = {0x04, 0x00, 0x01, 0x06};
    n = gateway_atvv_session_control(&g_s, bad, sizeof(bad), 130, g_acts,
                                     GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.state == GATEWAY_ATVV_STATE_ERROR, "状态 ERROR");
    CHECK(find_action(n, GATEWAY_ATVV_ACTION_ERROR) != NULL, "有 ERROR 动作");
}

static void test_session_stop_press_up_and_drain(void) {
    printf("[用例] 会话：STREAMING 收 STOP → PRESS_UP + DRAINING\n");
    session_setup();
    drive_to_streaming(100);
    uint8_t stop[] = {0x00};
    size_t n = gateway_atvv_session_control(&g_s, stop, sizeof(stop), 500, g_acts,
                                            GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(find_action(n, GATEWAY_ATVV_ACTION_PRESS_UP) != NULL, "有 PRESS_UP");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_DRAINING, "状态 DRAINING");
    CHECK(g_s.mic_open_remote == false, "远端 mic 标记清除");
}

static void test_session_reopen_reject_window(void) {
    printf("[用例] 会话：STOP 后 300ms 内 MIC_OPEN 被拒（无 ACK 无 PRESS_DOWN）\n");
    session_setup();
    drive_to_streaming(100);
    uint8_t stop[] = {0x00};
    gateway_atvv_session_control(&g_s, stop, sizeof(stop), 500, g_acts,
                                 GATEWAY_ATVV_MAX_ACTIONS);
    uint8_t mic_open[] = {0x08};
    // 拒绝窗：[500, 800)。DRAINING 态窗内（t=700，tick 未跑不自动迁移）
    size_t n = gateway_atvv_session_control(&g_s, mic_open, sizeof(mic_open), 700, g_acts,
                                            GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(n == 0, "DRAINING 期拒绝窗内无任何动作");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_DRAINING, "状态不变");

    printf("[用例] 会话：宽限收尾回 READY 后仍在拒绝窗内继续拒\n");
    gateway_atvv_session_tick(&g_s, 750, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.state == GATEWAY_ATVV_STATE_READY, "宽限早过（651），tick 收尾回 READY");
    n = gateway_atvv_session_control(&g_s, mic_open, sizeof(mic_open), 760, g_acts,
                                     GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(n == 0, "READY 期拒绝窗内（760<800）无动作");

    printf("[用例] 会话：拒绝窗外重开正常应答\n");
    n = gateway_atvv_session_control(&g_s, mic_open, sizeof(mic_open), 850, g_acts,
                                     GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(find_action(n, GATEWAY_ATVV_ACTION_WRITE_TX) != NULL, "窗外回 ACK");
    CHECK(find_action(n, GATEWAY_ATVV_ACTION_PRESS_DOWN) != NULL, "窗外 PRESS_DOWN");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_STREAMING, "重开进入 STREAMING");
}

// ---------- gateway_atvv_session：音频链 ----------

static void test_session_audio_to_pcm_frame(void) {
    printf("[用例] 音频：3×120B 零帧 → 1 个 640 样本 PCM 帧动作\n");
    session_setup();
    drive_to_streaming(100);
    // 前两包攒量不吐帧
    uint8_t frame[GATEWAY_ADPCM_DEFAULT_FRAME_BYTES];
    memset(frame, 0, sizeof(frame));
    size_t n = gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 200, g_acts,
                                          GATEWAY_ATVV_MAX_ACTIONS);
    n = gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 240, g_acts,
                                   GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(count_action(n, GATEWAY_ATVV_ACTION_PCM_FRAME) == 0, "480 样本不满帧不吐");
    n = gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 280, g_acts,
                                   GATEWAY_ATVV_MAX_ACTIONS);
    const gateway_atvv_action_t *pcm = find_action(n, GATEWAY_ATVV_ACTION_PCM_FRAME);
    CHECK(pcm != NULL, "720 样本吐 1 帧");
    CHECK(pcm->pcm_len == GATEWAY_PCM_SLICER_FRAME_SAMPLES, "帧长 640");
    CHECK(pcm->pcm[0] == 0 && pcm->pcm[639] == 0, "全零 ADPCM 从 0/0 解码全零 PCM");
}

static void test_session_sync_resets_decoder(void) {
    printf("[用例] 音频：AUDIO_SYNC(0x0a) 按 predictor/step 重置解码器\n");
    session_setup();
    drive_to_streaming(100);
    // 先喂非零 ADPCM 推高 predictor（0xFF × 120B）
    uint8_t frame[GATEWAY_ADPCM_DEFAULT_FRAME_BYTES];
    memset(frame, 0xFF, sizeof(frame));
    gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 200, g_acts,
                               GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.adpcm.predictor < 0, "负满量程输入后 predictor 为负");
    uint8_t sync[] = {0x0A, 0x00, 0x00, 0x00, 0x01, 0x00, 100};  // predictor=256, step=100→88
    size_t n = gateway_atvv_session_control(&g_s, sync, sizeof(sync), 210, g_acts,
                                            GATEWAY_ATVV_MAX_ACTIONS);
    (void)n;
    CHECK(g_s.adpcm.predictor == 256, "predictor 按值重置 (0x0100)");
    CHECK(g_s.adpcm.step_index == 88, "step 钳位 88");
    CHECK(gateway_frame_accumulator_pending(&g_s.acc) == 0, "SYNC 清空累积器");
    CHECK(gateway_pcm_slicer_pending(&g_s.slicer) == 0, "SYNC 清空切片器");
}

static void test_session_audio_tail_grace(void) {
    printf("[用例] 音频：STOP 后 150ms 内尾包仍出帧，超宽限丢弃\n");
    session_setup();
    drive_to_streaming(100);
    // 攒 2 包（480 样本）
    uint8_t frame[GATEWAY_ADPCM_DEFAULT_FRAME_BYTES];
    memset(frame, 0, sizeof(frame));
    gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 200, g_acts,
                               GATEWAY_ATVV_MAX_ACTIONS);
    gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 240, g_acts,
                               GATEWAY_ATVV_MAX_ACTIONS);
    uint8_t stop[] = {0x00};
    gateway_atvv_session_control(&g_s, stop, sizeof(stop), 300, g_acts,
                                 GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.state == GATEWAY_ATVV_STATE_DRAINING, "DRAINING");
    size_t n = gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 300 + 100, g_acts,
                                          GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(find_action(n, GATEWAY_ATVV_ACTION_PCM_FRAME) != NULL, "宽限内尾包凑满出帧");
    n = gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 300 + 151, g_acts,
                                   GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(count_action(n, GATEWAY_ATVV_ACTION_PCM_FRAME) == 0, "超宽限丢弃");
}

static void test_session_tick_finalize(void) {
    printf("[用例] 收尾：宽限到期余量补零出末帧，无余量不造帧\n");
    session_setup();
    drive_to_streaming(100);
    uint8_t frame[GATEWAY_ADPCM_DEFAULT_FRAME_BYTES];
    memset(frame, 0, sizeof(frame));
    gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 200, g_acts,
                               GATEWAY_ATVV_MAX_ACTIONS);  // 240 样本余量
    uint8_t stop[] = {0x00};
    gateway_atvv_session_control(&g_s, stop, sizeof(stop), 300, g_acts,
                                 GATEWAY_ATVV_MAX_ACTIONS);
    size_t n = gateway_atvv_session_tick(&g_s, 300 + GATEWAY_ATVV_AUDIO_TAIL_GRACE_MS, g_acts,
                                         GATEWAY_ATVV_MAX_ACTIONS);
    const gateway_atvv_action_t *pcm = find_action(n, GATEWAY_ATVV_ACTION_PCM_FRAME);
    CHECK(pcm != NULL && pcm->pcm_len == GATEWAY_PCM_SLICER_FRAME_SAMPLES,
          "240 余量补零成 640 末帧");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_READY, "收尾回 READY");

    printf("[用例] 收尾：零余量会话不造帧\n");
    session_setup();
    drive_to_streaming(100);
    gateway_atvv_session_control(&g_s, stop, sizeof(stop), 300, g_acts,
                                 GATEWAY_ATVV_MAX_ACTIONS);
    n = gateway_atvv_session_tick(&g_s, 300 + GATEWAY_ATVV_AUDIO_TAIL_GRACE_MS, g_acts,
                                  GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(count_action(n, GATEWAY_ATVV_ACTION_PCM_FRAME) == 0, "无余量无末帧");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_READY, "回 READY");
}

static void test_session_stream_start_hard_reset(void) {
    printf("[用例] 会话：STREAMING 中再收 0x04 → 硬重置流（RC003 重启场景）\n");
    session_setup();
    drive_to_streaming(100);
    uint8_t frame[GATEWAY_ADPCM_DEFAULT_FRAME_BYTES];
    memset(frame, 0xFF, sizeof(frame));
    gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 200, g_acts,
                               GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(g_s.adpcm.predictor < 0, "先推负");
    uint8_t restart[] = {0x04, 0x00, 0x02, 0x07};
    size_t n = gateway_atvv_session_control(&g_s, restart, sizeof(restart), 250, g_acts,
                                            GATEWAY_ATVV_MAX_ACTIONS);
    (void)n;
    CHECK(g_s.adpcm.predictor == 0 && g_s.adpcm.step_index == 0, "解码器硬重置 0/0");
    CHECK(gateway_frame_accumulator_pending(&g_s.acc) == 0, "累积器清空");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_STREAMING, "保持 STREAMING（会话内重启）");
}

static void test_session_idle_ignores_frames(void) {
    printf("[用例] 会话：IDLE/CAPS 期收 0x08/0x04/音频一律忽略\n");
    session_setup();
    uint8_t mic_open[] = {0x08};
    size_t n = gateway_atvv_session_control(&g_s, mic_open, sizeof(mic_open), 100, g_acts,
                                            GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(n == 0, "IDLE 忽略 MIC_OPEN");
    uint8_t frame[8] = {0};
    n = gateway_atvv_session_audio(&g_s, frame, sizeof(frame), 100, g_acts,
                                   GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(n == 0, "IDLE 忽略音频");
    gateway_atvv_session_start(&g_s, 100, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    n = gateway_atvv_session_control(&g_s, mic_open, sizeof(mic_open), 110, g_acts,
                                     GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(n == 0, "CAPS 期忽略 MIC_OPEN（握手未完成）");
}

static void test_session_disconnect_close(void) {
    printf("[用例] 断开：mic 打开时 Stop 产 MIC_CLOSE 并回 IDLE\n");
    session_setup();
    drive_to_streaming(100);
    size_t n = gateway_atvv_session_stop(&g_s, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    const gateway_atvv_action_t *tx = find_action(n, GATEWAY_ATVV_ACTION_WRITE_TX);
    CHECK(tx != NULL && tx->tx_len == 2 && tx->tx[0] == 0x0D,
          "v1.0 MIC_CLOSE = 0D <session>");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_IDLE, "回 IDLE");

    printf("[用例] 断开：mic 未开时 Stop 无 MIC_CLOSE\n");
    session_setup();
    drive_to_ready(100);
    n = gateway_atvv_session_stop(&g_s, g_acts, GATEWAY_ATVV_MAX_ACTIONS);
    CHECK(count_action(n, GATEWAY_ATVV_ACTION_WRITE_TX) == 0, "无 MIC_CLOSE");
    CHECK(g_s.state == GATEWAY_ATVV_STATE_IDLE, "回 IDLE");
}

int main(void) {
    test_adpcm_silence_from_zero();
    test_adpcm_negative_ramp();
    test_adpcm_positive_step();
    test_adpcm_multibyte_continuity();
    test_adpcm_nonzero_reset();
    test_adpcm_saturation_clamp();
    test_accumulator_fill_and_flush();
    test_accumulator_multi_frame();
    test_accumulator_set_frame_bytes();
    test_accumulator_out_cap_insufficient();
    test_slicer_fill_and_flush();
    test_slicer_take_remainder();
    test_session_start_gets_caps();
    test_session_caps_ok_and_timeout();
    test_session_caps_reject_8khz();
    test_session_caps_legacy_layout();
    test_session_mic_open_press_down();
    test_session_stream_start_2pro();
    test_session_stop_press_up_and_drain();
    test_session_reopen_reject_window();
    test_session_audio_to_pcm_frame();
    test_session_sync_resets_decoder();
    test_session_audio_tail_grace();
    test_session_tick_finalize();
    test_session_stream_start_hard_reset();
    test_session_idle_ignores_frames();
    test_session_disconnect_close();

    printf("\n结果：%d/%d 通过\n", s_total - s_failed, s_total);
    if (s_failed == 0) {
        printf("全部通过\n");
        return 0;
    }
    return 1;
}
