#include "CZlib.h"

// CZlib 是系统 zlib 的头文件包装 target（无自身实现）。新版本 SwiftPM 的
// 显式模块构建会把 C target 的模块产物写进链接清单，纯头文件 target 没有产物
// 可链会报 "Build input file cannot be found: Products/Debug/CZlib.o"——
// 提供一个空翻译单元保证产物存在（内容为空，不引入任何符号）。
