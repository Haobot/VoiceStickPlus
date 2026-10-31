# Windows 本地 LLM 精修落地五坑：NDEBUG 假绿 / CMake 缓存污染 / Debug 性能陷阱 / Git Bash→cmd 构建 / 改动落盘丢失

- 日期：2026-09-09
- 相关文件：`desktop/windows/CMakeLists.txt`、`desktop/windows/tests/core_tests.cc`、`desktop/windows/src/llama_cpp_engine.cc`、`desktop/windows/src/localization.cc`
- 相关 commit：`e4322d1e`（feat 主体）、`8155714e`（test 修复）、功能方案 `Doc/Plan/local-text-refinement.md`
- 时效声明：行号/API 语义为记录时点结论，引用前以当前源码为准。

## 坑 1：测试数据准备写进 assert → NDEBUG 假绿（最危险）

**症状**：`TestImaAdpcmDecoderGoldenFixtures` 在 RelWithDebInfo 下 19 个 session 全部
`sizes 0 vs N, 0 segments` FAIL 打印，但进程 exit 0；Debug 构建同测试全绿。全新
`build-x64-rel` 目录首次在 NDEBUG 下跑测试才暴露。

**判据**：Release 测试日志出现 FAIL 字样但 `echo $?`=0；或探针 fprintf 打在
被测函数内却零输出（函数根本没被调用）。

**根因**：`assert(Parse(...))`、`assert(exists(...))` 这类**承担数据准备的调用**在
NDEBUG 下整式展开为 `((void)0)`——不仅断言消失，**函数调用本身不执行**，
`gain_db/segments` 保持初值空，下游对拍必失败；而失败路径又只有 assert 兜底
（同样空操作），进程带着假 FAIL 正常退出。

**修复**：数据准备/前置校验提为显式 `if` + `failed` 计数 + 末尾
`failed > 0 → fprintf(stderr) + std::abort()`（保持"测试失败=进程异常终止"语义，
exit code 层面可见）。commit `8155714e`。

**经验**：assert 里只放**纯断言**（无副作用的表达式）；凡是给后续语句供数的
调用（解析/读文件/存在性检查）必须显式检查。Release 首跑测试必须读输出文本，
不能只看 exit code——本仓库 assert 风格测试在 NDEBUG 下结构性失效是系统性风险，
存量测试值得专项排查。

## 坑 2：BUILD_SHARED_LIBS 经 CMakeCache 回灌污染全树 add_library

**症状**：改 `desktop/windows/CMakeLists.txt` 后 regenerate，`opus`/
`voicestick_core` 悄悄变成 DLL：VoiceStickFlash 链接 LNK2019 缺 7 符号
（voicestick_core.lib 成了空导入库）、运行时还缺 opus.dll。

**判据**：LNK2019 大面积符号缺失 + 构建产物里冒出 `.dll`/导入库；
`CMakeCache.txt` 里 `BUILD_SHARED_LIBS:BOOL=ON`。

**根因**：llama.cpp/ggml 子项目 `option(BUILD_SHARED_LIBS "..." ON)` 首次配置把
默认 ON 写进 CMakeCache；后续任何触发 regenerate 的改动，缓存值回灌成全局默认，
把本项目所有**未显式指定类型**的 `add_library` 变成 SHARED。

**修复**：CMakeLists 顶部（任何 add_library/add_subdirectory 之前）
`set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)` 全树钉死 + `add_library(voicestick_core STATIC ...)` 显式类型双保险。commit `e4322d1e`。

**经验**：引入带 `option(BUILD_SHARED_LIBS)` 的第三方子项目时，本项目自己的
add_library 一律显式 STATIC/SHARED，免疫缓存污染。

## 坑 3：Debug 构建下 GGML 推理慢 20~40 倍，性能验证必须 Release

**症状**：llama.cpp 真模型 smoke 在 Debug 下 320 token 前缀 decode 耗时 221s
（~700ms/token）；同机 RelWithDebInfo 首句 1439ms（前缀一次性 1168ms）、
KV 前缀复用后稳态 304ms/句。llama-bench 复验机器基线正常，非环境问题。

**判据**：token 吞吐比预期低一个数量级以上 → 先看构建类型再看代码。

**修复**：smoke 测试加 `#ifndef NDEBUG` SKIP；真机性能验证用独立 RelWithDebInfo
目录 `desktop/windows/build-x64-rel`。

**经验**：ggml 的手写 SIMD/重排循环几乎吃满优化器，/Od 下退化一个数量级以上；
性能类断言（延迟上限）只在 Release 生效，Debug 下跑等于浪费 3 分钟等超时。

**附带 llama.cpp b10868 C API 三坑**（`llama_cpp_engine.cc` 注释有细节）：
`llama_tokenize` 缓冲不足返回**负的所需长度**（非错误，capacity 取 `-need`）；
`llama_sampler_sample` 的 idx 是 batch token 位（无 logits 直接 GGML_ABORT
弹 WerFault 卡死到测试超时），生成循环要传 `-1`（倒数第一个 output 行）；
MSVC 下 LU8 宏依赖 `__cplusplus` 恒 199711L 走错分支产裸 `u8""`，需给 llama
target 加 `/Zc:char8_t-`。

## 坑 4：Git Bash 调 cmd 构建的连环假成功

**症状**：`cmake --build ... | tail` 显示 exit 0 实则 `cmake: command not found`
（管道吃退出码）；`cmd /c '...'` 的 `/c` 被 MSYS 转成路径、双引号路径被加反斜杠；
Write 工具写出的 bat 是 LF 行尾，cmd 解析时 call 行截断（vcvars64 从未执行，
INCLUDE 空 → stdbool.h C1083）。

**判据**：构建命令"成功"后必验产物时间戳；bat 报"'xxx' 不是内部或外部命令"
且紧跟 C1083 = vcvars 根本没跑。

**修复配方**（本机验证可用）：
```bash
cmd //c 'call C:\PROGRA~2\MICROS~2\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul 2>&1 \
  && echo VCVARS_OK \
  && C:\PROGRA~2\MICROS~2\2022\BUILDT~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build <build-dir> --target <t>'
```
要点：`cmd //c` 双斜杠防 MSYS 转换；路径用 8.3 短名防引号转义；VS 的 cmake.exe
不在 vcvars PATH 里要全路径；先看 `VCVARS_OK` 再信后续输出；**链接期报 LNK1104
打不开 exe = 运行中的 exe 锁文件，先 `taskkill //IM VoiceStick.exe //F` 再链**。

**经验**：放弃 bat 中转直接 bash 内联；每次构建后 `ls -l --time-style` 验产物
时间戳，两秒换不来假绿。另：>10MB 的 VoiceStickApp.log 在 grep 下会被当二进制
**静默吞匹配**（无 "Binary file matches" 提示），必须 `grep -a`——本次排查
"装配日志缺失"假象即是此坑三犯。

## 坑 5：多文件改动的"落盘丢失"——提交前必须重新审计实际文件状态

**症状**：会话上下文记录"localization.h/.cc 中英文案表各 3 条已写"，实际磁盘
上 `localization.cc` 的表条目与 `kStringCount` **从未落盘**：枚举加了 3 个键、
表只算到旧尾，运行中的 exe 里设置对话框文案是数组读越界 UB。

**判据**：`grep -c <新键> <头文件> <实现文件>` 两边数量不一致；或
`git status` 里"应改的文件"不在改动列表。

**根因**：长会话/上下文重建期间单文件 Edit 的实际落盘状态与认知漂移。

**修复**：发现后补齐表条目 + kStringCount，重编双构建、重跑测试、重启 exe
验证（13:13 装配日志正常）。

**经验**：多文件功能在提交前跑一遍 `git diff --stat` 全量审计 + 新键/新符号
在头文件与实现文件两侧 grep 对数（本仓库本地化表还有完备性运行期 assert，
但 kStringCount 本身错了它也拦不住——计数基准与枚举尾必须同源）。

## 验证程度

- 单测：Debug（build-x64）与 RelWithDebInfo（build-x64-rel）双构建全量 exit 0、
  FAIL 计数 0；ATVV golden 19 session 双构建对拍全过。
- 真模型 smoke：Release 下 load 832ms、首句 1439ms（含前缀 1168ms 一次性）、
  第二句 304ms（KV 前缀复用），输出文本正确。
- 真机：Release VoiceStick.exe 装配日志 `Local mic runtime ready` +
  `Local refine engine ready`（0.84s）双引擎就绪；**真人语音精修效果
  （悬浮窗显示与最终注入文本）待用户实测，未验证不宣称**。

## 遗留/观察项

- 存量 assert 风格测试里可能还有 NDEBUG 短路的数据准备（本坑 1 的系统性排查
  未做，仅在 ATVV golden 一处修复）。
- llama.cpp FetchContent 默认走 ghfast.top 镜像，网络受限时
  `VOICESTICK_LLAMA_CPP_URL=file:///...zip` 或 `FETCHCONTENT_SOURCE_DIR_LLAMA_CPP`
  指本地源码；172MB 源码缓存绝不提交。
