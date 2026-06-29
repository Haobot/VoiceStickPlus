# VOICE_NET_DISABLE 在顶层 CMakeLists.txt 设为 ON。这里仅负责把它作为预处理宏
# 传给所有组件，让 voice_net.h 的 #ifdef VOICE_NET_DISABLE stub 生效。
# 回退：顶层注释掉 set(...) 后，这里也注释掉下一行。
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES stick_s3_board voice_ble voice_net audio_pipeline ui_status button esp_pm esp_timer json bmi270
)

# 作为预处理宏传给所有组件。必须在 idf_component_register 之后调用。
idf_build_set_property(COMPILE_DEFINITIONS VOICE_NET_DISABLE APPEND)


