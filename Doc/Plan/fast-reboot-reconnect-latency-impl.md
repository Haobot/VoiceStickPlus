# 快速重启回连时长压缩 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 Stick 设备快速重启后的 Windows 回连时长从 ~6.7s 压到 ~3.5-4s（安定窗 4.5s→1.5s + 首订阅 2.5s 应用层超时 + 僵尸场景免退避立即重试），并实测固件 boot→广播耗时决定是否需要固件侧提速。

**Architecture:** 全部 Windows 改动集中在 `desktop/windows/src/ble_central_win.{cc,h}` 的连接编排层：缩短僵尸链路安定窗，给链上首个 ATT 操作（state 订阅）加 `winrt::when_any` 超时兜底，对"经安定窗路径放行"的连接打 zombie_suspect 标记、失败时免 5s 退避立即重试。固件侧只做测量（串口日志已有时间戳），达到阈值才另行设计提速。

**Tech Stack:** C++20 / C++/WinRT（Windows 端），ESP-IDF C（固件，仅测量），CMake + Ninja + MSVC 2022（构建），CTest（回归）。

**设计文档:** `Doc/Plan/fast-reboot-reconnect-latency.md`（已批准并提交，commit 9328915）

**测试说明:** `BleCentralWin` 依赖 WinRT 真机 BLE 栈，`voicestick_core` 单测不覆盖它，本计划不新增单元测试（TDD 在此不适用）；验证手段是 `ctest -R voicestick_windows_tests` 保持全绿 + 真机重启实测（Task 3/4）。

**环境备忘（执行时必须遵守）:**
- Git Bash 里 `cmd /c` 只打横幅不执行，构建用 `powershell -NoProfile -Command "cmd /c build_win.bat"`。
- `build_win.bat` 会自动杀 VoiceStick.exe 并重建 `desktop/windows/build-x64`；历史上出现过链接失败仍报成功，构建后必须核对 `desktop/windows/build-x64/VoiceStick.exe` 时间戳已更新。
- `desktop/windows/` 被 `.gitignore` 整体忽略，提交必须 `git add -f`。
- 串口采集用 `python scripts/e2e_test/read_serial.py COM19 <秒>`；**不要**用 `idf_cli.py -s`（DTR 复位会把设备踹进下载模式）。设备重启/USB 重枚举后 pyserial 句柄失效，需重开。
- 桌面端日志在 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`。

---

### Task 1: 安定窗缩短 + zombie_suspect 免退避重试（Windows）

**Files:**
- Modify: `desktop/windows/src/ble_central_win.cc`（常量区 ~:68，`try_claim_connect` ~:763-781，`fail` lambda ~:979-1010，成功路径 ~:1556-1560，`RestartForResume` ~:323-329）
- Modify: `desktop/windows/src/ble_central_win.h:155` 附近（新增成员）

- [ ] **Step 1: 缩短安定窗常量**

`desktop/windows/src/ble_central_win.cc:65-69` 区域，把：

```cpp
constexpr std::chrono::milliseconds kReconnectSettleDelay{4500};
```

改为：

```cpp
constexpr std::chrono::milliseconds kReconnectSettleDelay{1500};
```

同时更新 :65 行注释为：

```cpp
// 快速重启场景，延迟 kReconnectSettleDelay 等 OS 埋掉僵尸链路再连。
// 判出僵尸时已主动 Close gatt_session/ble_device（栈立即发 LL_TERMINATE），
// 不需要等被动监督超时的 3.5-4s；撞未死僵尸由 kSubscribeTimeout 兜底（见订阅处）。
```

- [ ] **Step 2: 头文件新增 zombie_suspect 集合**

`desktop/windows/src/ble_central_win.h:155` 的 `reconnect_settle_until_` 声明之后追加：

```cpp
    // 经安定窗路径放行的地址：其连接失败多为僵尸链路尚未拆完，
    // fail 时免 5s 退避，让下一条广播立即触发重试。
    std::set<std::uint64_t> zombie_suspect_addresses_;
```

（该头文件已用 `std::set`/`std::map`，无需新 include。）

- [ ] **Step 3: try_claim_connect 放行时打标记**

`desktop/windows/src/ble_central_win.cc:765-771`，把：

```cpp
        auto settle = reconnect_settle_until_.find(bluetooth_address);
        if (settle != reconnect_settle_until_.end()) {
            if (std::chrono::steady_clock::now() < settle->second) {
                return false; // 僵尸链路安定窗内，等 OS 拆除旧链路
            }
            reconnect_settle_until_.erase(settle);
        }
```

改为：

```cpp
        auto settle = reconnect_settle_until_.find(bluetooth_address);
        if (settle != reconnect_settle_until_.end()) {
            if (std::chrono::steady_clock::now() < settle->second) {
                return false; // 僵尸链路安定窗内，等 OS 拆除旧链路
            }
            reconnect_settle_until_.erase(settle);
            // 经安定窗放行的连接：失败时免退避快速重试（见 fail lambda）。
            zombie_suspect_addresses_.insert(bluetooth_address);
        }
```

- [ ] **Step 4: fail lambda 免退避**

`desktop/windows/src/ble_central_win.cc:981-989`，把：

```cpp
        {
            std::lock_guard lock(mutex_);
            // 连接失败后设置 5 秒退避期，防止扫描→立即重试→再失败的 tight-loop。
            // 5 秒足以让 Windows BLE 栈从异常状态中恢复，同时用户感知的延迟可接受。
            connect_cooldown_until_[bluetooth_address] =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            connecting_addresses_.erase(bluetooth_address);
            cancelled_device_ids_.erase(device_id);
        }
```

改为：

```cpp
        bool zombie_suspect = false;
        {
            std::lock_guard lock(mutex_);
            zombie_suspect = zombie_suspect_addresses_.erase(bluetooth_address) > 0;
            // 连接失败后设置 5 秒退避期，防止扫描→立即重试→再失败的 tight-loop。
            // 5 秒足以让 Windows BLE 栈从异常状态中恢复，同时用户感知的延迟可接受。
            // 例外：经安定窗放行的 zombie_suspect 连接失败多为僵尸链路未拆完，
            // 此时退避只会拖延回连，下一条广播（20-30ms 一条）立即重试即可。
            if (!zombie_suspect) {
                connect_cooldown_until_[bluetooth_address] =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            connecting_addresses_.erase(bluetooth_address);
            cancelled_device_ids_.erase(device_id);
        }
```

并在 `LogBleLine("connect failed VS-" + ...)` 的 message 尾部追加（便于真机日志确认兜底路径）：

```cpp
        LogBleLine("connect failed VS-" + device_id + " address=" +
                   FormatBluetoothAddress(bluetooth_address) + " reason=" + message +
                   (zombie_suspect ? " [zombie-suspect: no cooldown, immediate retry]"
                                   : ""));
```

（即替换原有那条 `LogBleLine("connect failed ...")`。）

- [ ] **Step 5: 成功路径与 RestartForResume 清标记**

`desktop/windows/src/ble_central_win.cc:1556-1560` 成功注册会话处，把：

```cpp
        {
            std::lock_guard lock(mutex_);
            sessions_by_device_id_[device_id] = session;
            connecting_addresses_.erase(bluetooth_address);
        }
```

改为：

```cpp
        {
            std::lock_guard lock(mutex_);
            sessions_by_device_id_[device_id] = session;
            connecting_addresses_.erase(bluetooth_address);
            zombie_suspect_addresses_.erase(bluetooth_address);
        }
```

`desktop/windows/src/ble_central_win.cc:323-329` `RestartForResume` 的清理块，把：

```cpp
        connecting_addresses_.clear();
        cancelled_device_ids_.clear();
        connect_cooldown_until_.clear();
        reconnect_settle_until_.clear();
```

改为：

```cpp
        connecting_addresses_.clear();
        cancelled_device_ids_.clear();
        connect_cooldown_until_.clear();
        reconnect_settle_until_.clear();
        zombie_suspect_addresses_.clear();
```

- [ ] **Step 6: 构建并核对产物时间戳**

Run: `powershell -NoProfile -Command "cmd /c build_win.bat"`
Expected: 构建成功；随后 `ls -l --time-style=full-iso desktop/windows/build-x64/VoiceStick.exe` 确认时间戳为刚刚（防历史误报成功）。

- [ ] **Step 7: 跑 Windows 单测回归**

Run: `powershell -NoProfile -Command "ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests"`
Expected: 全绿（约 15s）。

- [ ] **Step 8: Commit**

```bash
git add -f desktop/windows/src/ble_central_win.cc desktop/windows/src/ble_central_win.h
git commit -m "feat: 僵尸链路安定窗 4.5s→1.5s + 安定窗路径连接失败免退避立即重试"
```

---

### Task 2: state 订阅应用层超时（Windows）

**Files:**
- Modify: `desktop/windows/src/ble_central_win.cc`（常量区 ~:68，订阅段 ~:1522-1529）

- [ ] **Step 1: 新增超时常量**

`desktop/windows/src/ble_central_win.cc:68` 附近（`kReconnectSettleDelay` 之后）追加：

```cpp
// 链上首个 ATT 操作（state 订阅）的应用层超时。正常几十 ms 完成；撞上未死
// 僵尸链路时 OS 要 ~3.5-4s 才宣告断连，这里 2.5s 提前取消并走失败路径，
// 配合 zombie_suspect 免退避把最坏回连压在 ~5.5s 而不是 ~10s。
constexpr std::chrono::milliseconds kSubscribeTimeout{2500};
```

- [ ] **Step 2: state 订阅改为带超时的 when_any**

`desktop/windows/src/ble_central_win.cc:1522-1529`，把：

```cpp
        LogBleLine("subscribing state notifications VS-" + device_id);
        log_stage("state_subscribe_begin");
        auto state_subscribe = co_await session->state_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        LogBleLine("state subscribe VS-" + device_id +
                   " status=" + GattStatusName(state_subscribe));
        log_stage("state_subscribe_done", "status=" + GattStatusName(state_subscribe));
```

改为：

```cpp
        LogBleLine("subscribing state notifications VS-" + device_id);
        log_stage("state_subscribe_begin");
        auto state_op = session->state_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        co_await winrt::when_any(state_op, WaitMs(kSubscribeTimeout));
        if (state_op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
            try { state_op.Cancel(); } catch (...) {}
            fail("state subscribe timeout after " +
                 std::to_string(kSubscribeTimeout.count()) + "ms");
            co_return;
        }
        const auto state_subscribe = state_op.GetResults();
        LogBleLine("state subscribe VS-" + device_id +
                   " status=" + GattStatusName(state_subscribe));
        log_stage("state_subscribe_done", "status=" + GattStatusName(state_subscribe));
```

注意：
- `WaitMs` 是本文件 :834 已有的辅助函数；`winrt::when_any` 来自 `winrt/base.h`（文件已通过 pch/直接包含引入，若编译报未定义再补 `#include <winrt/base.h>`）。
- audio 订阅**不**加超时：state 订阅成功即证明链路活着，audio 不会再挂（YAGNI）。
- 原代码 `auto state_subscribe` 非 const，下方 :1545-1548 的失败检查引用不变。

- [ ] **Step 3: 构建并核对产物时间戳**

Run: `powershell -NoProfile -Command "cmd /c build_win.bat"`
Expected: 构建成功；`ls -l --time-style=full-iso desktop/windows/build-x64/VoiceStick.exe` 时间戳为刚刚。

- [ ] **Step 4: 跑 Windows 单测回归**

Run: `powershell -NoProfile -Command "ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests"`
Expected: 全绿。

- [ ] **Step 5: Commit**

```bash
git add -f desktop/windows/src/ble_central_win.cc
git commit -m "feat: state 订阅加 2.5s 应用层超时，撞僵尸提前取消走快速重试"
```

---

### Task 3: 真机验证（快速重启回连时长）

**Files:** 无代码改动；读 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`。

- [ ] **Step 1: 启动新版 VoiceStick.exe 并确认已连接**

Run: `powershell -NoProfile -Command "Start-Process desktop\windows\build-x64\VoiceStick.exe"`
等托盘出现、设备连上（设备显示 ready）。若设备当前未连，等其自动回连。

- [ ] **Step 2: 记录日志基线行数**

Run: `wc -l "$LOCALAPPDATA/VoiceStick/VoiceStickApp.log"`（Git Bash 下 `$LOCALAPPDATA` 可用；若为空则用 `/c/Users/cross/AppData/Local/VoiceStick/VoiceStickApp.log`）
记下行数 N，后续只分析 N 之后的新日志。

- [ ] **Step 3: 请用户快速重启设备 5 次**

提示用户：短按复位（与烧录后重启相同的操作），每次等设备屏幕显示已连接/ready 后再进行下一次，间隔约 30s。每次重启后记录主观感受的连接耗时。

- [ ] **Step 4: 分析日志，统计回连链路耗时**

Run: `tail -n +$((N+1)) "$LOCALAPPDATA/VoiceStick/VoiceStickApp.log" | grep -E "reconnect settle|link-layer connected|state subscribe|connect failed|connected VS-|zombie-suspect"`

对每次重启确认：
- 出现 `reconnect settle VS-XXXX: delaying 1500ms`；
- 成功尝试 `link-layer connected ... polls>=1`（polls=0 说明撞了僵尸，应伴随 `state subscribe timeout` + `[zombie-suspect: no cooldown, immediate retry]`）；
- 从 settle 日志到 `connected VS-` 的时间差。

- [ ] **Step 5: 判定与调参**

- 5 次全部一次成功（无 subscribe timeout）且总回连 ≤4s：达标，可把终值写定。
- 出现撞僵尸（timeout + fast retry）：说明 1.5s 安定窗偏短，把 `kReconnectSettleDelay` 上调到 2000ms，回到 Task 1 Step 6 重新构建并重复本 Task。
- 若 2000ms 仍偶发撞僵尸：保持 2000ms（兜底已把最坏压在 ~5.8s），在结果记录中注明。

---

### Task 4: 固件 boot→广播实测（只测量，不改固件）

**Files:** 无代码改动；串口日志来自 `voice_ble.c:691` 的 `advertising as ... ts=`。

- [ ] **Step 1: 采集重启串口日志**

请用户准备重启设备。先在 Git Bash 启动采集（设备重启后 USB JTAG 会重枚举，pyserial 句柄可能失效；若读到 0 字节就退出重开）：

Run: `python scripts/e2e_test/read_serial.py COM19 30`

启动后立即请用户短按重启设备，采满 30s。

- [ ] **Step 2: 计算 boot→adv 耗时**

在输出中找 boot 首行（`boot reset_reason=...`，`main.c:2129`）的 ts 与首条 `advertising as VS-XXXX mode=fast ... ts=` 的 ts，差值即 boot→广播耗时。

- [ ] **Step 3: 决策**

- 差值 <1s：固件侧无优化空间，本计划固件部分到此为止。
- 差值 ≥1s：**不在本计划内改固件**。把实测数据追加到 `Doc/Plan/fast-reboot-reconnect-latency.md` 末尾，向用户报告并建议另起小设计排查 `init_power_management` / `stick_s3_board_init` 延时与 bootloader 配置。

---

### Task 5: 结果记录与文档收尾

**Files:**
- Modify: `Doc/Expe/ble-zombie-link-reboot-reconnect.md`（末尾追加本轮结果）
- Modify: `Doc/Plan/fast-reboot-reconnect-latency.md`（如安定窗终值与 1500 不同，同步更新）

- [ ] **Step 1: 追加经验记录**

在 `Doc/Expe/ble-zombie-link-reboot-reconnect.md` 末尾追加一节，包含：安定窗终值、是否出现过撞僵尸兜底、Task 3 实测回连时长分布（N 次、最短/最长/中位）、Task 4 的 boot→adv 实测值。

- [ ] **Step 2: 同步设计文档终值**

若 Task 3 Step 5 把 `kReconnectSettleDelay` 定成了非 1500 的值，把 `Doc/Plan/fast-reboot-reconnect-latency.md` 中的 1500ms 改为终值并注明实测依据。

- [ ] **Step 3: Commit**

```bash
git add Doc/Expe/ble-zombie-link-reboot-reconnect.md Doc/Plan/fast-reboot-reconnect-latency.md
git commit -m "docs: 记录快速重启回连压缩实测结果（安定窗终值 + 回连时长分布）"
```
