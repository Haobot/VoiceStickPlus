cmake_minimum_required(VERSION 3.24)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" VOICESTICK_VERSION_STRING LIMIT_COUNT 1)
string(STRIP "${VOICESTICK_VERSION_STRING}" VOICESTICK_VERSION_STRING)
project(VoiceStickWindows VERSION ${VOICESTICK_VERSION_STRING} LANGUAGES C CXX RC)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/utf-8)
endif()

include(FetchContent)

# 默认从 github 拉 WinSparkle；网络不可达时可设 VOICESTICK_WINSPARKLE_URL 指向本地 zip：
#   用户环境变量 VOICESTICK_WINSPARKLE_URL=C:/path/to/WinSparkle-0.9.2.zip
# 或 cmake -DVOICESTICK_WINSPARKLE_URL=C:/path/... -S desktop\windows -B ...
set(_winsparkle_default_url "https://github.com/vslavik/winsparkle/releases/download/v0.9.2/WinSparkle-0.9.2.zip")
if(DEFINED ENV{VOICESTICK_WINSPARKLE_URL})
    set(_winsparkle_url "$ENV{VOICESTICK_WINSPARKLE_URL}")
elseif(DEFINED VOICESTICK_WINSPARKLE_URL)
    set(_winsparkle_url "${VOICESTICK_WINSPARKLE_URL}")
else()
    set(_winsparkle_url "${_winsparkle_default_url}")
endif()
message(STATUS "WinSparkle source: ${_winsparkle_url}")
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

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

add_library(voicestick_core
    src/app_config.cc
    src/asr_protocol.cc
    src/ble_protocol.cc
    src/debug_audio_recorder.cc
    src/firmware_manifest.cc
    src/llm_chat_client.cc
    src/llm_translation_client.cc
    src/llm_refinement_client.cc
    src/localization.cc
    third_party/cjson/cJSON.c
    src/log.cc
    src/ogg_crc.cc
    src/ogg_opus_muxer.cc
    src/pair_device_helper.cc
    src/voice_stick_coordinator.cc
)

target_include_directories(voicestick_core PUBLIC src third_party/cjson third_party/tomlplusplus)
target_compile_definitions(voicestick_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(voicestick_core PUBLIC winhttp)
target_link_libraries(voicestick_core PUBLIC bcrypt)

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
    src/hotkey_settings_dialog.cc
    resources/VoiceStick.rc
)

target_include_directories(VoiceStickApp PRIVATE resources ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(VoiceStickApp PRIVATE
    VOICESTICK_APPCAST_URL="https://78.github.io/voicestick/appcast.xml"
)

set_target_properties(VoiceStickApp PROPERTIES OUTPUT_NAME VoiceStick)

# 嵌入管理员权限清单（requireAdministrator），让 exe 启动即提升到 High 完整性，
# 绕过 UIPI 向微信 4.0 等高完整性窗口粘贴文本。CMake 的 vs_link_exe 包装器会拦截
# /MANIFEST* 链接标志并嵌入默认 asInvoker 清单，因此改用 POST_BUILD 步骤以 mt.exe
# 把自定义清单写入资源 ID 1（CREATEPROCESS_MANIFEST_RESOURCE_ID），直接覆盖默认清单。
# VoiceStick.rc 不声明 RT_MANIFEST，避免与 mt.exe 重复嵌入冲突。
# 详见 Doc/Plan/windows-uipi-elevated-injection.md。
if(MSVC)
    add_custom_command(TARGET VoiceStickApp POST_BUILD
        COMMAND "${CMAKE_MT}" -nologo
                -manifest "${CMAKE_CURRENT_SOURCE_DIR}/resources/VoiceStick.manifest"
                "-outputresource:$<TARGET_FILE:VoiceStickApp>;#1"
        COMMENT "Embedding requireAdministrator manifest into VoiceStick.exe")
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
    windowscodecs
    comctl32
    ole32
    advapi32
    taskschd
)

add_custom_command(TARGET VoiceStickApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PROPERTY:winsparkle_lib,IMPORTED_LOCATION>"
        "$<TARGET_FILE_DIR:VoiceStickApp>"
)

add_executable(voicestick_windows_tests
    tests/core_tests.cc
    src/asr_client_tencent.cc
    src/tencent_asr_vocab_client.cc
)

target_link_libraries(voicestick_windows_tests PRIVATE voicestick_core winhttp bcrypt)

enable_testing()
add_test(NAME voicestick_windows_tests COMMAND voicestick_windows_tests)
