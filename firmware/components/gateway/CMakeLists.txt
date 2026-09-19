# gateway 组件：小米遥控器 → StickS3 网关（方案 A，Doc/Plan/xiaomi-remote-stick-gateway.md）
# 纯逻辑模块（gateway_report_parser/gateway_keymap/gateway_adpcm/gateway_atvv_session）
# 不依赖 ESP-IDF，另有 test/run_tests.py 主机侧单测（MSVC 编译）；
# 设备侧模块依赖 NimBLE/NVS。
idf_component_register(
    SRCS "src/gateway_report_parser.c"
         "src/gateway_keymap.c"
         "src/gateway_hogp_report.c"
         "src/gateway_mode.c"
         "src/gateway_hogp.c"
         "src/gateway_hid_host.c"
         "src/gateway_adpcm.c"
         "src/gateway_atvv_session.c"
         "src/gateway_targets_core.c"
         "src/gateway_targets.c"
         "src/xiaomi_atvv_client.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash stick_s3_board
)
