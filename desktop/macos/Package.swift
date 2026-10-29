// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "VoiceStick",
    platforms: [
        .macOS(.v12)
    ],
    products: [
        .executable(name: "VoiceStickApp", targets: ["VoiceStickApp"])
    ],
    dependencies: [
        .package(url: "https://github.com/sparkle-project/Sparkle", from: "2.6.0"),
        .package(url: "https://github.com/LebJe/TOMLKit.git", from: "0.6.0"),
    ],
    targets: [
        .executableTarget(
            name: "VoiceStickApp",
            dependencies: [
                "CZlib",
                "COpus",
                "VoiceStickCore",
                .product(name: "Sparkle", package: "Sparkle"),
                .product(name: "TOMLKit", package: "TOMLKit"),
            ],
            path: "Sources/VoiceStickApp",
            exclude: ["Info.plist"],
            resources: [
                // 按键映射对话框的遥控器照片：SwiftPM 随构建产物分发资源 bundle，
                // 裸二进制（swift build/run）与打包 .app 双形态都能找到。
                .process("Resources"),
            ],
            linkerSettings: [
                .unsafeFlags([
                    "-Xlinker", "-sectcreate",
                    "-Xlinker", "__TEXT",
                    "-Xlinker", "__info_plist",
                    "-Xlinker", "Sources/VoiceStickApp/Info.plist",
                ])
            ]
        ),
        // 纯逻辑 core 库（无 AppKit/CoreBluetooth 依赖）：BLE 协议结构、小米 ATVV
        // 协议/会话状态机、IMA-ADPCM 解码、PCM 后处理、Opus 编码。供 app 与测试
        // runner 共用；测试 runner 单独链接，不依赖 executable。
        .target(
            name: "VoiceStickCore",
            dependencies: [
                "COpus",
                "CRnnoise",
                // SenseVoice 本地 ASR 的 C 结构体定义（header-only，dylib 运行时 dlopen）。
                "CSherpaOnnx",
                // llama.cpp b10868 C API 头（header-only 桥接，dylib 运行时 dlopen，
                // 同 CSherpaOnnx 模式——仓库无 dylib 也能构建，见 Sources/CLlamaCpp/README.md）。
                "CLlamaCpp",
                // N1 根刀：AppConfig（TOML 读写）下迁 Core 需 TOMLKit（测试 runner 经 Core 传递获取，无需改 runner 声明）。
                .product(name: "TOMLKit", package: "TOMLKit"),
            ],
            path: "Sources/VoiceStickCore"
        ),
        // sherpa-onnx v1.13.7 C API 头（header-only 桥接，见 Sources/CSherpaOnnx/README.md）。
        .target(
            name: "CSherpaOnnx",
            path: "Sources/CSherpaOnnx",
            publicHeadersPath: "include"
        ),
        // llama.cpp b10868 C API 头（header-only 桥接；dylib 由
        // scripts/fetch-llama-cpp.sh 合成安装，运行时 dlopen）。
        .target(
            name: "CLlamaCpp",
            path: "Sources/CLlamaCpp",
            publicHeadersPath: "include"
        ),
        .target(
            name: "CZlib",
            path: "Sources/CZlib",
            publicHeadersPath: "."
        ),
        // 无框架测试 runner（本机无 Xcode，XCTest/swift-testing 均不可用）：
        // main.swift 顶层驱动 + 极简断言，移植 Windows tests/core_tests.cc 的
        // 小米 ATVV 链路用例。运行：swift run VoiceStickTests。
        // 诊断工具：回放 DebugAudio 会话 ogg 过真实 LocalAsrClient（不入 app）。
        .executableTarget(
            name: "AsrReplayCli",
            dependencies: ["VoiceStickCore"],
            path: "Tools/AsrReplayCli"
        ),
        // 诊断/复测工具：内嵌 llama.cpp 引擎的 chat 冒烟 + 精修/对齐评测复测
        //（D1 评测口径对拍；需先 scripts/fetch-llama-cpp.sh 安装 dylib）。
        .executableTarget(
            name: "LocalLlmCli",
            dependencies: ["VoiceStickCore"],
            path: "Tools/LocalLlmCli"
        ),
        .executableTarget(
            name: "VoiceStickTests",
            dependencies: ["VoiceStickCore", "COpus"],
            path: "Tests/VoiceStickTests"
        ),
        // vendored xiph/rnnoise（master 2026-10-14，BSD-2，见 Sources/CRnnoise/README.md）：
        // ASR 前桌面端人声增强/降噪（AudioEnhancer），48kHz/480 样本帧推理。
        .target(
            name: "CRnnoise",
            path: "Sources/CRnnoise",
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("src"),
            ]
        ),
        // vendored xiph/opus v1.5.2（浮点路径），供小米遥控器 ATVV 链路把
        // IMA-ADPCM 解码后的 PCM 编码为与固件一致的标准 Opus 帧。
        .target(
            name: "COpus",
            path: "Sources/COpus",
            publicHeadersPath: "include",
            cSettings: [
                .define("OPUS_BUILD"),
                .define("USE_ALLOCA"),
                .define("HAVE_ALLOCA_H"),
                .define("HAVE_LRINT"),
                .define("HAVE_LRINTF"),
                .headerSearchPath("."),
                .headerSearchPath("include"),
                .headerSearchPath("celt"),
                .headerSearchPath("silk"),
                .headerSearchPath("silk/float"),
                .headerSearchPath("src"),
            ]
        )
    ]
)
