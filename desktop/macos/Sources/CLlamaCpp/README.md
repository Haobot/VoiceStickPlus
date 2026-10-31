# CLlamaCpp（header-only 桥接）

- `include/`：上游 [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp)
  **b10868** 的 C API 头原样拷贝（MIT，上游许可头保留），与 Windows 端
  `desktop/windows/CMakeLists.txt` 的 llama.cpp FetchContent 同版本锁。
  升级双端必须同步换版本（pin 见 `scripts/fetch-llama-cpp.sh` 的 `PIN`）。
  头集合为 llama.h 的 include 闭包（llama/ggml/ggml-alloc/ggml-backend/
  ggml-cpu/ggml-opt/gguf 七个，全 C）；其余 ggml 头（ggml-cpp.h C++ RAII 包装、
  ggml-openvino.h 等含 C++ 标准库 include）会被 SwiftPM 的 C 伞模块解析炸掉，
  **不得**加入。
- 仅提供编译期类型定义；`libvoicestick-llama.dylib` **不入库**，由
  `scripts/fetch-llama-cpp.sh` 用 cmake 预编译 llama+ggml 静态库后
  `-force_load` 合成单 dylib，安装到
  `~/Library/Application Support/VoiceStick/models/llama/lib/`，运行时由
  `LlamaCppEngine` dlopen（搜索序 env `VOICESTICK_LLAMA_DIR` → app bundle
  Frameworks → Application Support）。仓库克隆后无需任何二进制即可 `swift build`。
- Windows 端为静态链入（FetchContent），macOS 选择 dylib 形态的原因：
  SwiftPM manifest 的运行时条件 target 探测不可靠（沙箱/缓存行为），
  dlopen 是仓库既有模式（CSherpaOnnx 同款），且允许发布包把 dylib 放
  Frameworks 或运行时按需安装。
