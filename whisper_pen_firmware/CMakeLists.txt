cmake_minimum_required(VERSION 3.16)

# Whisper Pen firmware entry. ESP-IDF v5.5.1, target esp32s3.
# ICS-41351 + bare I2S (no codec layer) + NimBLE + Opus.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(whisper_pen)
