# 跨端契约测试（golden-frame fixtures）

跟进评审 `Doc/Plan/architecture-review-followup-2026-10-07.md` 阶段 0 动作 0.1（上游 9-22
评审「最高性价比②」）：**同一份黄金字节，三端各自解析并对拍期望**，把「协议在
文档/固件/Swift/C++ 四处手写、零机器校验」变成 CI 断言。

## 结构

```text
tests/contract/
├── generate_fixtures.py        # 黄金字节生成器（人读源，独立于实现手搓）
├── fixtures/manifest.json      # 生成产物（提交入库），测试消费
└── README.md                   # 本文件
```

manifest.json 六组样本（共 48 条，规格出处 `Doc/Ref/protocol.md`）：

| 组 | 内容 | 比对方式 |
|---|---|---|
| `state_frames` | 14 个 state_tx JSON 事件（device_info/按键/编码器/电量/网关…） | 整帧字节解析 → 期望字段（键=线上字段名） |
| `power_mgmt_frames` | 供电态开关事件（走独立解析器） | 同上 |
| `ota_state_frames` | OTA ready/progress/done/error/aborted 五态 | 同上 |
| `binary_frames` | audio start/data/end 帧头 + motion（含 ±8000 边界） | 解析 → 期望字段 / payload 字节 |
| `control_payloads` | 18 个 desktop→firmware 控制帧构建 | 各端构建 → **对象语义**（字段集合+值，键序无关） |
| `ota_control_frames` | OTA begin/data/end/abort 二进制控制帧 | 构建 → **整帧字节相等** |

## 读者（三端）

| 端 | 实现 | 状态 |
|---|---|---|
| Windows | `desktop/windows/tests/core_tests.cc::TestContractFixtures` | ✅ 随 CI ctest 运行 |
| macOS | `desktop/macos/Tests/VoiceStickTests/ContractFixtureTests.swift` | ✅ 随 CI swift run 运行 |
| 固件 | `firmware/components/voice_ble/test/control_cmd_contract_test.c`（经 run_tests.py 目标 `firmware_control_cmd` 注入 REPO_ROOT 消费同一 manifest） | ✅ control_rx 18 样本 + 反向门用例，随 CI host-tests 运行 |

## 键序与字段集约定

- **JSON 键序不构成契约**：固件 cJSON 解析与键序无关；macOS `JSONSerialization`
  键序不保证，因此 control 组比对对象语义而非原始字符串（Windows 构建是确定性拼接，
  但不因此豁免）。
- **expect 只取两端公共字段**：已知单端缺口不在期望集中（避免把已知 backlog 变成红灯）：
  - Windows 无 `gateway_keymap` routes 解析（backlog D10）→ 该样本只断言 `event`；
  - Windows `StateEvent` 无 `buttons`/`ui_states` 数组（device_info 样本只断言公共三字段）；
  - macOS `InteractionMode` 枚举缺 `hold_to_talk_instant` → 该控制样本只测 `hold_to_talk`；
  - `gateway_keymap_get` / `gateway_select_target` / `usb_auto_off_get` 仅 Windows 有构建器 → 不入公共样本。

## 新增 fixture

1. 在 `generate_fixtures.py` 对应组追加（JSON 文本直接抄协议文档示例，二进制按
   协议结构函数构造——**不要从实现输出反推**）；
2. `python3 tests/contract/generate_fixtures.py` 重新生成并提交 manifest；
3. 两端测试按线上字段名映射（新增字段若某端结构体缺字段，先入上表缺口清单而非放松断言）。
