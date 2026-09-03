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
            dependencies: ["COpus"],
            path: "Sources/VoiceStickCore"
        ),
        .target(
            name: "CZlib",
            path: "Sources/CZlib",
            publicHeadersPath: "."
        ),
        // 无框架测试 runner（本机无 Xcode，XCTest/swift-testing 均不可用）：
        // main.swift 顶层驱动 + 极简断言，移植 Windows tests/core_tests.cc 的
        // 小米 ATVV 链路用例。运行：swift run VoiceStickTests。
        .executableTarget(
            name: "VoiceStickTests",
            dependencies: ["VoiceStickCore", "COpus"],
            path: "Tests/VoiceStickTests"
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
