# CSherpaOnnx（header-only 桥接）

- `include/sherpa-onnx/c-api/c-api.h`：上游 [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)
  **v1.13.7** 的 C API 头文件原样拷贝（MIT，上游许可头保留），与 Windows 端
  `third_party/sherpa-onnx`（CMakeLists sherpa-onnx 段）同版本锁。
  升级双端必须同步换版本。
- 仅提供编译期类型定义；`libsherpa-onnx-c-api.dylib` **不入库**，运行时由
  `SherpaSenseVoiceEngine` dlopen（详见其注释），因此仓库克隆后无需任何
  二进制即可 `swift build`。
