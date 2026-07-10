cmake_minimum_required(VERSION 3.16)

# Whisper Pen firmware entry. ESP-IDF v5.5.1, target esp32s3.
# StickS3 hardware: ES8311 codec + NimBLE(128-bit 互通协议) + Opus + ext1 wake.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(whisper_pen)
