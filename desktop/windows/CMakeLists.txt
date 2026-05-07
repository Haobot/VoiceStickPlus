cmake_minimum_required(VERSION 3.24)

project(VoiceStickWindows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(voicestick_core
    src/app_config.cc
    src/asr_protocol.cc
    src/ble_protocol.cc
    src/debug_audio_recorder.cc
    src/log.cc
    src/ogg_crc.cc
    src/ogg_opus_muxer.cc
    src/voice_stick_coordinator.cc
)

target_include_directories(voicestick_core PUBLIC src)
target_compile_definitions(voicestick_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(VoiceStickApp WIN32
    src/main.cc
    src/win32_app.cc
    src/asr_client_win.cc
    src/input_injector_win.cc
    src/ble_central_win.cc
    src/overlay_window.cc
    src/pair_device_dialog.cc
    src/settings_dialog.cc
)

target_link_libraries(VoiceStickApp PRIVATE
    voicestick_core
    windowsapp
    runtimeobject
    shell32
    user32
    d2d1
    dwrite
    gdi32
    gdiplus
    windowscodecs
    comctl32
    ole32
    advapi32
    winhttp
)

add_executable(voicestick_windows_tests
    tests/core_tests.cc
)

target_link_libraries(voicestick_windows_tests PRIVATE voicestick_core)

enable_testing()
add_test(NAME voicestick_windows_tests COMMAND voicestick_windows_tests)
