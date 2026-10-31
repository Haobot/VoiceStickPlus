# 小米遥控器 StickS3 网关 Phase 0 spike（一次性验证程序，不并入正式固件）
# 设计依据：Doc/Plan/xiaomi-remote-stick-gateway.md §6 Phase 0 spike 清单
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaomi_gateway_poc)
