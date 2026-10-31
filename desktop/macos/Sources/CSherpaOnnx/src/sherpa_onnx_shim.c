// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// CSherpaOnnx 是 header-only 桥接目标：只编译上游 c-api.h 提供 C 结构体/常量
// 定义（SenseVoice 配置结构体布局须与运行时 dylib 精确一致），不在链接期绑定
// libsherpa-onnx-c-api.dylib——动态库由 SherpaSenseVoiceEngine 在运行时 dlopen
//（搜索 env/Bundle/Application Support 三级），仓库与 CI 无 dylib 也能构建。
// 上游：k2-fsa/sherpa-onnx v1.13.7（与 Windows third_party 同版本锁），MIT。
int vs_sherpa_onnx_shim_version(void) { return 11307; }
