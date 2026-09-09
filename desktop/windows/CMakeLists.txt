cmake_minimum_required(VERSION 3.24)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" VOICESTICK_VERSION_STRING LIMIT_COUNT 1)
string(STRIP "${VOICESTICK_VERSION_STRING}" VOICESTICK_VERSION_STRING)
project(VoiceStickWindows VERSION ${VOICESTICK_VERSION_STRING} LANGUAGES C CXX RC)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 全树钉死静态库：llama.cpp/ggml 子项目的 option(BUILD_SHARED_LIBS)（默认 ON）
# 会写入 CMakeCache，下次 regenerate 时回灌，把本项目所有未指定类型的
# add_library（opus / voicestick_core）悄悄变成 DLL——符号不导出致下游
# LNK2019、还引入运行时 DLL 依赖。在任何 add_library/add_subdirectory 之前
# FORCE 关闭，ggml/llama/opus 全静态链入（无新增运行时 DLL）。
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(MSVC)
    add_compile_options(/utf-8)
endif()

include(FetchContent)

# libopus：桌面端新增 Opus 解码能力，用于 wechat_input_method 输出模式。
# 使用 vendored 的 xiph/opus v1.5.2 源码，通过标准 CMake 构建为静态库。
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 默认从 github 拉 WinSparkle；网络不可达时可设 VOICESTICK_WINSPARKLE_URL 指向本地 zip：
#   用户环境变量 VOICESTICK_WINSPARKLE_URL=C:/path/to/WinSparkle-0.9.2.zip
# 或 cmake -DVOICESTICK_WINSPARKLE_URL=C:/path/... -S desktop\windows -B ...
set(_winsparkle_default_url "https://github.com/vslavnik/winsparkle/releases/download/v0.9.2/WinSparkle-0.9.2.zip")
if(DEFINED ENV{VOICESTICK_WINSPARKLE_URL})
    set(_winsparkle_url "$ENV{VOICESTICK_WINSPARKLE_URL}")
elseif(DEFINED VOICESTICK_WINSPARKLE_URL)
    set(_winsparkle_url "${VOICESTICK_WINSPARKLE_URL}")
else()
    set(_winsparkle_url "${_winsparkle_default_url}")
endif()
message(STATUS "WinSparkle source: ${_winsparkle_url}")
# Allow pre-extracted source dir to bypass download (for offline/dev builds):
#   cmake -DFETCHCONTENT_SOURCE_DIR_WINSPARKLE=C:/path/to/winsparkle ...
set(FETCHCONTENT_SOURCE_DIR_WINSPARKLE "" CACHE PATH "WinSparkle pre-extracted source dir (optional, for offline builds)")
FetchContent_Declare(
    winsparkle
    URL ${_winsparkle_url}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(winsparkle)

add_library(winsparkle_lib SHARED IMPORTED GLOBAL)
set_target_properties(winsparkle_lib PROPERTIES
    IMPORTED_IMPLIB "${winsparkle_SOURCE_DIR}/x64/Release/WinSparkle.lib"
    IMPORTED_LOCATION "${winsparkle_SOURCE_DIR}/x64/Release/WinSparkle.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${winsparkle_SOURCE_DIR}/include"
)

# sherpa-onnx C API（本机麦克风模式迭代一：SenseVoice 本地离线识别，v1.13.7）。
# 二进制不进 git；来源二选一：
#   1) m0/.venv wheel 同款文件（与 Python 端同版本同 ABI，推荐开发机）：
#      m0/.venv/Lib/site-packages/sherpa_onnx/{include,lib}
#   2) GitHub release v1.13.7 sherpa-onnx-v1.13.7-win-x64.tar.bz2 的 include/lib
# 放到 third_party/sherpa-onnx/（include/ + lib/），或用 VOICESTICK_SHERPA_ONNX_DIR
# 指向解压目录。显式 -DVOICESTICK_ENABLE_LOCAL_ASR=OFF 可跳过（不需要本地识别时）。
option(VOICESTICK_ENABLE_LOCAL_ASR "SenseVoice local offline ASR (local mic mode)" ON)
set(_sherpa_onnx_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sherpa-onnx")
if(DEFINED ENV{VOICESTICK_SHERPA_ONNX_DIR})
    set(_sherpa_onnx_dir "$ENV{VOICESTICK_SHERPA_ONNX_DIR}")
elseif(DEFINED VOICESTICK_SHERPA_ONNX_DIR)
    set(_sherpa_onnx_dir "${VOICESTICK_SHERPA_ONNX_DIR}")
endif()
if(VOICESTICK_ENABLE_LOCAL_ASR)
    if(NOT EXISTS "${_sherpa_onnx_dir}/lib/sherpa-onnx-c-api.lib")
        message(FATAL_ERROR
            "sherpa-onnx 未就位: ${_sherpa_onnx_dir}/lib/sherpa-onnx-c-api.lib 不存在。\n"
            "获取方式：从 m0/.venv/Lib/site-packages/sherpa_onnx/ 复制 include 与 lib 到 "
            "third_party/sherpa-onnx/，或设 VOICESTICK_SHERPA_ONNX_DIR 指向解压的 "
            "GitHub v1.13.7 win-x64 包；不需要本地识别可 -DVOICESTICK_ENABLE_LOCAL_ASR=OFF。")
    endif()
    add_library(sherpa_onnx_c_api SHARED IMPORTED GLOBAL)
    set_target_properties(sherpa_onnx_c_api PROPERTIES
        IMPORTED_IMPLIB "${_sherpa_onnx_dir}/lib/sherpa-onnx-c-api.lib"
        IMPORTED_LOCATION "${_sherpa_onnx_dir}/lib/sherpa-onnx-c-api.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${_sherpa_onnx_dir}/include"
    )
    # 运行时依赖：sherpa-onnx-c-api.dll + onnxruntime.dll 随构建产物部署。
    file(COPY "${_sherpa_onnx_dir}/lib/sherpa-onnx-c-api.dll"
              "${_sherpa_onnx_dir}/lib/onnxruntime.dll"
         DESTINATION "${CMAKE_BINARY_DIR}")
endif()

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

# 内置凭据（编译期注入 exe，预配置 MSI 分发用；公开构建留空）。
# build-msi.bat 从本机 %APPDATA%\VoiceStick\config.toml 提取凭据注入。
# 详见 Doc/Plan/windows-builtin-api-key.md。
set(VOICESTICK_BUILTIN_API_KEY "" CACHE STRING "Built-in volcengine ASR API key")
set(VOICESTICK_BUILTIN_TENCENT_SECRET_ID "" CACHE STRING "Built-in Tencent ASR secret id")
set(VOICESTICK_BUILTIN_TENCENT_SECRET_KEY "" CACHE STRING "Built-in Tencent ASR secret key")
set(VOICESTICK_BUILTIN_TENCENT_APPID "" CACHE STRING "Built-in Tencent ASR app id")
set(VOICESTICK_BUILTIN_LLM_API_KEY "" CACHE STRING "Built-in LLM API key (DeepSeek)")
set(VOICESTICK_BUILTIN_LLM_BASE_URL "" CACHE STRING "Built-in LLM base URL (DeepSeek)")
set(VOICESTICK_BUILTIN_LLM_MODEL "" CACHE STRING "Built-in LLM model name (DeepSeek)")
# 凭据含 " 或 \ 会破坏生成的头文件字符串字面量，直接报错。
foreach(var
    VOICESTICK_BUILTIN_API_KEY
    VOICESTICK_BUILTIN_TENCENT_SECRET_ID
    VOICESTICK_BUILTIN_TENCENT_SECRET_KEY
    VOICESTICK_BUILTIN_TENCENT_APPID
    VOICESTICK_BUILTIN_LLM_API_KEY
    VOICESTICK_BUILTIN_LLM_BASE_URL
    VOICESTICK_BUILTIN_LLM_MODEL)
    if(${var} MATCHES "[\"\\\\]")
        message(FATAL_ERROR "${var} 含非法字符（\" 或 \\），会破坏 builtin_secrets.h 字符串字面量。")
    endif()
endforeach()
configure_file(src/builtin_secrets.h.in ${CMAKE_CURRENT_BINARY_DIR}/builtin_secrets.h @ONLY)

# 显式 STATIC：llama.cpp 子项目的 option(BUILD_SHARED_LIBS) 会向 CMakeCache
# 写入默认 ON，下次 regenerate 时回灌，把未指定类型的本库悄悄变成 DLL
#（符号不导出、下游链接 LNK2019）。显式类型免疫缓存污染。
add_library(voicestick_core STATIC
    src/air_mouse_kin.cc
    src/key_spec.cc
    src/app_config.cc
    src/asr_protocol.cc
    src/audio_opus_decoder.cc
    src/audio_opus_encoder.cc
    src/ble_protocol.cc
    src/cmd_line.cc
    src/com_port_selector.cc
    src/debug_audio_recorder.cc
    src/esptool_flash_command.cc
    src/esptool_progress.cc
    src/voice_stick_flash_tool.cc
    src/firmware_manifest.cc
    src/llm_chat_client.cc
    src/llm_translation_client.cc
    src/llm_refinement_client.cc
    src/text_refiner.cc
    src/local_refinement_client.cc
    src/hotword_extractor.cc
    src/hotword_candidate_miner.cc
    src/hotword_selector.cc
    src/localization.cc
    src/push_to_talk_key.cc
    src/provider_combo.cc
    src/clipboard_vault.cc
    src/shortcut_capture.cc
    third_party/cjson/cJSON.c
    src/log.cc
    src/ogg_crc.cc
    src/ogg_opus_muxer.cc
    src/ogg_opus_demuxer.cc
    src/pair_device_helper.cc
    src/pcm_ring_buffer.cc
    src/power_log_monitor.cc
    src/voice_stick_coordinator.cc
    src/wasapi_virtual_mic_renderer.cc
    src/wechat_input_method_hotkey.cc
    src/device_switch_state.cc
    src/default_audio_device_controller.cc
    src/ima_adpcm_decoder.cc
    src/pcm_postprocessor.cc
    src/wasapi_mic_capture.cc
    src/xiaomi_atvv_protocol.cc
    src/xiaomi_atvv_session.cc
    src/xiaomi_keymap_interceptor.cc
)

target_include_directories(voicestick_core PUBLIC src third_party/cjson third_party/tomlplusplus ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(voicestick_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(voicestick_core PUBLIC winhttp)
target_link_libraries(voicestick_core PUBLIC bcrypt)
target_link_libraries(voicestick_core PUBLIC opus)
target_link_libraries(voicestick_core PUBLIC mmdevapi ole32 uuid)
target_link_libraries(voicestick_core PUBLIC setupapi)  # com_port_selector 串口枚举
if(VOICESTICK_ENABLE_LOCAL_ASR)
    target_sources(voicestick_core PRIVATE src/local_asr_client_win.cc)
    target_link_libraries(voicestick_core PUBLIC sherpa_onnx_c_api)
    target_compile_definitions(voicestick_core PUBLIC VOICESTICK_LOCAL_ASR_ENABLED=1)
endif()

# llama.cpp（本地文本精修 LLM 推理，Qwen3-1.7B GGUF）：pin b10868（与
# m0/refine spike 验证版一致，Doc/Plan/local-text-refinement.md）。
# 源码经 FetchContent 拉取（WinSparkle 同款模式），静态链入 voicestick_core，
# 无新增 DLL、与 sherpa-onnx 的 onnxruntime 零冲突。默认走镜像加速源；
# 网络受限时设 VOICESTICK_LLAMA_CPP_URL 指向本地 zip
# （file:///C:/path/llama-b10868.zip），或 FETCHCONTENT_SOURCE_DIR_LLAMA_CPP
# 指向已解压源码目录。显式 -DVOICESTICK_ENABLE_LOCAL_REFINE=OFF 可跳过。
option(VOICESTICK_ENABLE_LOCAL_REFINE "Local LLM text refinement via llama.cpp" ON)
set(_llama_cpp_default_url "https://ghfast.top/https://github.com/ggml-org/llama.cpp/archive/refs/tags/b10868.zip")
if(DEFINED ENV{VOICESTICK_LLAMA_CPP_URL})
    set(_llama_cpp_url "$ENV{VOICESTICK_LLAMA_CPP_URL}")
elseif(DEFINED VOICESTICK_LLAMA_CPP_URL)
    set(_llama_cpp_url "${VOICESTICK_LLAMA_CPP_URL}")
else()
    set(_llama_cpp_url "${_llama_cpp_default_url}")
endif()
if(VOICESTICK_ENABLE_LOCAL_REFINE)
    message(STATUS "llama.cpp source: ${_llama_cpp_url}")
    # 子项目开关：只编 ggml(CPU) + llama 核心静态库，裁掉工具/示例/测试/下载
    set(LLAMA_STANDALONE OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
    set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
    set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
    set(GGML_NATIVE ON CACHE BOOL "" FORCE)
    set(GGML_CUDA OFF CACHE BOOL "" FORCE)
    set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
    set(GGML_RPC OFF CACHE BOOL "" FORCE)
    # MSVC 下 OpenMP 依赖发现不稳（BuildTools 常缺 LLVM OpenMP），
    # 单句生成场景线程池已并行，收益小风险大，直接关。
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        llama_cpp
        URL ${_llama_cpp_url}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(llama_cpp)
    # MSVC 默认 __cplusplus 报 199711L（无 /Zc:__cplusplus），b10868 的 LU8 宏
    # 因此走 C++17 分支产出裸 u8"" 字面量；而 /std:c++20 又启用了 char8_t，
    # u8"" 成 char8_t[] 无法传 const char* 参数（llama-chat.cpp C2664）。
    # 关闭 char8_t 特性恢复 C++17 行为，与宏分支自洽，改动面最小。
    if(MSVC)
        target_compile_options(llama PRIVATE /Zc:char8_t-)
    endif()
    target_sources(voicestick_core PRIVATE src/llama_cpp_engine.cc)
    target_link_libraries(voicestick_core PRIVATE llama)
    target_compile_definitions(voicestick_core PUBLIC VOICESTICK_LOCAL_REFINE_ENABLED=1)
endif()

add_executable(VoiceStickApp WIN32
    src/main.cc
    src/win32_app.cc
    src/asr_client_win.cc
    src/asr_client_tencent.cc
    src/tencent_asr_vocab_client.cc
    src/input_injector_win.cc
    src/ble_central_win.cc
    src/global_hotkey_win.cc
    src/voice_stick_cloud_api_win.cc
    src/firmware_update_dialog.cc
    src/glass_backdrop_window.cc
    src/overlay_window.cc
    src/subtitle_window.cc
    src/pair_device_dialog.cc
    src/onboarding_dialog.cc
    src/settings_dialog.cc
    src/encoder_settings_dialog.cc
    src/interaction_settings_dialog.cc
    src/remote_settings_dialog.cc
    src/xiaomi_keymap_dialog.cc
    src/battery_monitor_dialog.cc
    src/air_mouse_tuning_window.cc
    src/hotkey_settings_dialog.cc
    src/selection_hotword_manager.cc
    src/voice_f5_suppressor.cc
    src/xiaomi_keymap_hook.cc
    src/mic_mode_hotkey.cc
    resources/VoiceStick.rc
)

target_include_directories(VoiceStickApp PRIVATE resources ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(VoiceStickApp PRIVATE
    VOICESTICK_APPCAST_URL="https://haobot.github.io/VoiceStickPlus/appcast.xml"
)

set_target_properties(VoiceStickApp PROPERTIES OUTPUT_NAME VoiceStick)

# 嵌入应用清单（asInvoker + DPI 感知 + Win10/11 兼容性声明）。CMake 的 vs_link_exe 包装器
# 会拦截 /MANIFEST* 链接标志并嵌入默认 asInvoker 清单（无 DPI/supportedOS），因此改用 POST_BUILD
# 步骤以 mt.exe 把自定义清单写入资源 ID 1（CREATEPROCESS_MANIFEST_RESOURCE_ID），直接覆盖默认清单。
# VoiceStick.rc 不声明 RT_MANIFEST，避免与 mt.exe 重复嵌入冲突。
# 详见 Doc/Plan/windows-uipi-elevated-injection.md。
if(MSVC)
    add_custom_command(TARGET VoiceStickApp POST_BUILD
        COMMAND "${CMAKE_MT}" -nologo
                -manifest "${CMAKE_CURRENT_SOURCE_DIR}/resources/VoiceStick.manifest"
                "-outputresource:$<TARGET_FILE:VoiceStickApp>;#1"
        COMMENT "Embedding application manifest into VoiceStick.exe")
endif()

target_link_libraries(VoiceStickApp PRIVATE
    voicestick_core
    winsparkle_lib
    windowsapp
    runtimeobject
    shell32
    user32
    d2d1
    dwrite
    dwmapi
    gdi32
    gdiplus
    shlwapi
    comdlg32
    windowscodecs
    comctl32
    ole32
    advapi32
    uiautomationcore
)

add_custom_command(TARGET VoiceStickApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PROPERTY:winsparkle_lib,IMPORTED_LOCATION>"
        "$<TARGET_FILE_DIR:VoiceStickApp>"
)

# 复制配置模板到构建目录，供 WiX 打包进 MSI。程序首启从 exe 同级复制到 %APPDATA%。
# 详见 Doc/Plan/windows-msi-config-template-seed.md。
add_custom_command(TARGET VoiceStickApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/config.template.toml"
        "$<TARGET_FILE_DIR:VoiceStickApp>/config.template.toml"
)

# VoiceStickFlash：独立 COM 口固件烧录小工具（BLE OTA 之外的用户级兜底链路）。
# 外壳只做 UI + 子进程，可测试逻辑在 voicestick_core（com_port_selector /
# esptool_flash_command / esptool_progress / voice_stick_flash_tool）。
# 详见 Doc/Plan/windows-com-flash-tool.md。
add_executable(VoiceStickFlash WIN32
    src/flash_tool_main.cc
    src/flash_tool_dialog.cc
    resources/flash_tool.rc
)

target_include_directories(VoiceStickFlash PRIVATE resources ${CMAKE_CURRENT_BINARY_DIR})

set_target_properties(VoiceStickFlash PROPERTIES OUTPUT_NAME VoiceStickFlash)

# 与 VoiceStickApp 同款清单嵌入（asInvoker + DPI 感知）：mt.exe POST_BUILD 覆盖默认清单。
if(MSVC)
    add_custom_command(TARGET VoiceStickFlash POST_BUILD
        COMMAND "${CMAKE_MT}" -nologo
                -manifest "${CMAKE_CURRENT_SOURCE_DIR}/resources/flash_tool.manifest"
                "-outputresource:$<TARGET_FILE:VoiceStickFlash>;#1"
        COMMENT "Embedding application manifest into VoiceStickFlash.exe")
endif()

target_link_libraries(VoiceStickFlash PRIVATE
    voicestick_core
    shell32
    user32
    gdi32
    comctl32
)

add_executable(voicestick_windows_tests
    tests/core_tests.cc
    src/asr_client_tencent.cc
    src/tencent_asr_vocab_client.cc
)

target_link_libraries(voicestick_windows_tests PRIVATE voicestick_core winhttp bcrypt)
# golden fixtures（ATVV 采集）相对仓库根解析，不依赖测试进程工作目录
target_compile_definitions(voicestick_windows_tests PRIVATE
    VOICESTICK_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")

enable_testing()
add_test(NAME voicestick_windows_tests COMMAND voicestick_windows_tests)

# L1 ASR 集成测试：真实火山 AsrClientWin + FakeBleCentral 注入语料 ogg，断言 CER。
# 需 %APPDATA%\VoiceStick\config.toml 配 volcengine_api_key；无 key 时 skip（return 77）。
# asr_client_win.cc 不在 voicestick_core（属外壳层），此处单独编入。
add_executable(voicestick_integration_tests
    tests/integration_tests.cc
    src/asr_client_win.cc
)
target_link_libraries(voicestick_integration_tests PRIVATE voicestick_core winhttp bcrypt)
add_test(NAME voicestick_integration_tests COMMAND voicestick_integration_tests
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set_tests_properties(voicestick_integration_tests PROPERTIES SKIP_RETURN_CODE 77)
