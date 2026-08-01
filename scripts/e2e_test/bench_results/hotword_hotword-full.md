# 热词评测报告 hotword-full

- 热词语料：20 条；对照语料：31 条
- 热词库规模：500 词（目标热词 17 个）
- 轮次：1

| 配置 | 平台 | 成功率 | 热词命中率 | CER | 误触发率 | 尾延迟 p50 |
|---|---|---|---|---|---|---|
| baseline | volcengine | 51/51 | 16/22 (0.7273) | 0.0226 | 0.0968 | 937ms |
| baseline | tencent | 51/51 | 16/22 (0.7273) | 0.0173 | 0.0968 | 156ms |
| direct_small | volcengine | 51/51 | 19/22 (0.8636) | 0.0155 | 0.0968 | 985ms |
| direct_small | tencent | 51/51 | 21/22 (0.9545) | 0.0097 | 0.0968 | 141ms |
| direct_full | volcengine | 51/51 | 16/22 (0.7273) | 0.0226 | 0.0968 | 953ms |
| direct_full | tencent | 51/51 | 18/22 (0.8182) | 0.012 | 0.0968 | 156ms |
| tiered | volcengine | 51/51 | 19/22 (0.8636) | 0.0169 | 0.0968 | 953ms |
| tiered | tencent | 51/51 | 21/22 (0.9545) | 0.0097 | 0.0968 | 141ms |
| table | tencent | 51/51 | 21/22 (0.9545) | 0.0097 | 0.0968 | 141ms |
| no_nonstream | volcengine | 51/51 | 15/22 (0.6818) | 0.0198 | 0.0968 | 641ms |

## 逐词命中率

### baseline / volcengine

- AGENTS.md: 1/1 (1.0)
- BLE: 2/2 (1.0)
- CLAUDE.md: 0/1 (0.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 0/1 (0.0)
- 覃海洋: 0/2 (0.0)
- 逐玉: 0/1 (0.0)

### baseline / tencent

- AGENTS.md: 1/1 (1.0)
- BLE: 1/2 (0.5)
- CLAUDE.md: 1/1 (1.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 0/1 (0.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 0/1 (0.0)
- 覃海洋: 1/2 (0.5)
- 逐玉: 0/1 (0.0)

### direct_small / volcengine

- AGENTS.md: 1/1 (1.0)
- BLE: 2/2 (1.0)
- CLAUDE.md: 0/1 (0.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 0/1 (0.0)
- 覃海洋: 2/2 (1.0)
- 逐玉: 1/1 (1.0)

### direct_small / tencent

- AGENTS.md: 1/1 (1.0)
- BLE: 2/2 (1.0)
- CLAUDE.md: 1/1 (1.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 1/1 (1.0)
- 覃海洋: 2/2 (1.0)
- 逐玉: 1/1 (1.0)

### direct_full / volcengine

- AGENTS.md: 1/1 (1.0)
- BLE: 2/2 (1.0)
- CLAUDE.md: 0/1 (0.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 0/1 (0.0)
- 覃海洋: 0/2 (0.0)
- 逐玉: 0/1 (0.0)

### direct_full / tencent

- AGENTS.md: 1/1 (1.0)
- BLE: 2/2 (1.0)
- CLAUDE.md: 1/1 (1.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 0/1 (0.0)
- 覃海洋: 1/2 (0.5)
- 逐玉: 0/1 (0.0)

### tiered / volcengine

- AGENTS.md: 1/1 (1.0)
- BLE: 2/2 (1.0)
- CLAUDE.md: 0/1 (0.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 0/1 (0.0)
- 覃海洋: 2/2 (1.0)
- 逐玉: 1/1 (1.0)

### tiered / tencent

- AGENTS.md: 1/1 (1.0)
- BLE: 2/2 (1.0)
- CLAUDE.md: 1/1 (1.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 1/1 (1.0)
- 覃海洋: 2/2 (1.0)
- 逐玉: 1/1 (1.0)

### table / tencent

- AGENTS.md: 1/1 (1.0)
- BLE: 2/2 (1.0)
- CLAUDE.md: 1/1 (1.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 2/2 (1.0)
- 蜜制: 1/1 (1.0)
- 覃海洋: 2/2 (1.0)
- 逐玉: 1/1 (1.0)

### no_nonstream / volcengine

- AGENTS.md: 1/1 (1.0)
- BLE: 1/2 (0.5)
- CLAUDE.md: 1/1 (1.0)
- ESP-IDF: 1/1 (1.0)
- ESP32: 1/1 (1.0)
- ESP32-S3: 1/1 (1.0)
- OTA: 1/1 (1.0)
- Ogg: 0/1 (0.0)
- Opus: 2/2 (1.0)
- VB-CABLE: 1/1 (1.0)
- VoiceStick: 1/1 (1.0)
- WebSocket: 2/2 (1.0)
- WinSparkle: 1/1 (1.0)
- 疯四: 1/2 (0.5)
- 蜜制: 0/1 (0.0)
- 覃海洋: 0/2 (0.0)
- 逐玉: 0/1 (0.0)
