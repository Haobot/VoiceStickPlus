# Windows 桌面端"从本地文件更新固件"BLE OTA 入口

## 背景与目标

当前桌面端"更新固件"只走远程 manifest 下载链路：

```
托盘菜单"更新到 vX.Y.Z"
  -> VoiceStickCoordinator::UpdateFirmwareFromLatest   (coordinator.cc:275)
  -> client.DownloadOtaSync(manifest)                   (firmware_manifest.cc:233，WinHTTP)
  -> BleCentralWin::UpdateFirmware(image, ...)          (ble_central_win.cc:579，BLE OTA 推送)
```

底层 `UpdateFirmware(ByteVector image, ...)` 接受任意字节流，只校验 `image.size() <= 3*1024*1024`（OTA 分区上限）。OTA 协议发原始 app bin（`OtaBeginPayload(size)` -> 分块写 `ota_rx`），**无额外封装头**。远程下载返回的也是同样格式的 ESP-IDF app bin，所以本地编译的 `firmware/build/voice_stick.bin` 能直接喂底层。

目标：加一个"从本地文件更新固件..."托盘菜单项，读本地 bin 经 BLE OTA 推给已连接设备，复用底层 `UpdateFirmware` 与现有进度对话框，零改动 OTA 协议层。

## 改动文件与落点

| 文件 | 改动 |
|---|---|
| `desktop/windows/src/voice_stick_coordinator.h` | `UpdateFirmwareFromLatest`(218) 旁加 `UpdateFirmwareFromFile` 声明 |
| `desktop/windows/src/voice_stick_coordinator.cc` | 实现 `UpdateFirmwareFromFile`：读文件为 `ByteVector`，校验大小，调 `ble_->UpdateFirmware` |
| `desktop/windows/src/win32_app.cc` | 加菜单 ID 常量 `kMenuUpdateFirmwareFromFileBase/End`；菜单项常驻；命令分发；实现 `StartFirmwareUpdateFromFile`（`GetOpenFileNameW` 选 bin -> 弹 `FirmwareUpdateDialog` -> 调 coordinator） |
| `desktop/windows/src/win32_app.h` | 加 `StartFirmwareUpdateFromFile` 声明(95 旁) |
| `desktop/windows/src/localization.h` / `.cc` | 加 `kMenuUpdateFirmwareFromFile` 字符串；文件对话框标题 |
| `desktop/windows/tests/core_tests.cc` | `UpdateFirmwareFromFile` 单元测试（FakeBleCentral 捕获 image） |

`ByteVector` 定义在 `byte_utils.h`（`using ByteVector = std::vector<std::uint8_t>;`）。

## 实现细节

### 1. coordinator

`voice_stick_coordinator.h`，`UpdateFirmwareFromLatest` 后加：

```cpp
void UpdateFirmwareFromFile(const std::string& file_path,
                            const std::string& device_id,
                            std::function<void(FirmwareUpdateProgress)> progress,
                            std::function<void(bool, std::string)> completion);
```

`voice_stick_coordinator.cc` 实现（参照 `UpdateFirmwareFromLatest`，去掉网络下载，换成读本地文件）：

```cpp
void VoiceStickCoordinator::UpdateFirmwareFromFile(
    const std::string& file_path, const std::string& device_id,
    std::function<void(FirmwareUpdateProgress)> progress,
    std::function<void(bool, std::string)> completion) {
    std::ifstream f(file_path, std::ios::binary);
    if (!f) { completion(false, "Cannot open firmware file."); return; }
    ByteVector image((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (image.empty()) { completion(false, "Firmware file is empty."); return; }
    // 大小校验与底层 UpdateFirmware 一致（3MB），提前给友好错误。
    if (image.size() > 3 * 1024 * 1024) {
        completion(false, "Firmware file is larger than the OTA partition.");
        return;
    }
    ble_->UpdateFirmware(std::move(image), device_id,
                         std::move(progress), std::move(completion));
}
```

需要 `#include <fstream>`。读文件在调用线程同步执行（文件小，1.44MB，无需异步线程；`UpdateFirmwareFromLatest` 起线程是为网络下载，本地读不必）。

### 2. win32_app

**菜单 ID 常量**（`win32_app.cc:57` 后，Hotkey 之前）：现有段用到 5899，新增用 5900：

```cpp
constexpr UINT kMenuUpdateFirmwareFromFileBase = 5900;
constexpr UINT kMenuUpdateFirmwareFromFileEnd = 5999;
```

**菜单项**（`win32_app.cc:1153` 固件区块）：在现有"更新到 X"逻辑之外，只要 `connected` 就常驻追加一项，不依赖 `firmware.update_available`：

```cpp
if (connected) {
    AppendMenuW(submenu, MF_STRING,
                kMenuUpdateFirmwareFromFileBase + static_cast<UINT>(i),
                TrW(StringId::kMenuUpdateFirmwareFromFile, language).c_str());
}
```

**命令分发**（`win32_app.cc:816` 区块旁加 else-if）：

```cpp
} else if (cmd >= kMenuUpdateFirmwareFromFileBase && cmd <= kMenuUpdateFirmwareFromFileEnd) {
    std::size_t index = cmd - kMenuUpdateFirmwareFromFileBase;
    if (index < paired_device_ids_.size()) {
        StartFirmwareUpdateFromFile(paired_device_ids_[index]);
    }
}
```

**StartFirmwareUpdateFromFile**（参照 `StartFirmwareUpdate` 1665）：

```cpp
void Win32App::StartFirmwareUpdateFromFile(const std::string& device_id) {
    if (!coordinator_) return;
    // GetOpenFileNameW 选 .bin
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Firmware binary (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    // 标题用本地化字符串
    if (!GetOpenFileNameW(&ofn)) return;  // 用户取消
    std::string file_path = Utf8FromUtf16(path);

    firmware_update_dialog_ = std::make_unique<FirmwareUpdateDialog>(
        instance_, hwnd_, EffectiveUiLanguage(config_.ui_language), "local file");
    firmware_update_dialog_->on_cancel = [this] {
        if (coordinator_) coordinator_->CancelFirmwareUpdate();
    };
    firmware_update_dialog_->Show();
    coordinator_->UpdateFirmwareFromFile(
        file_path, device_id,
        [this](FirmwareUpdateProgress progress) {
            DispatchToUi([this, progress] {
                if (firmware_update_dialog_) firmware_update_dialog_->UpdateProgress(progress);
            });
        },
        [this](bool success, std::string message) {
            DispatchToUi([this, success, message] {
                if (firmware_update_dialog_) firmware_update_dialog_->Finish(success, message);
            });
        });
}
```

需确认 `Utf8FromUtf16` 存在（`Utf16` 的逆，见 `win32_app.h:100` 附近应有配对）。若命名不同按实际。

### 3. localization

`localization.h` `kMenuUpdateFirmware`(92) 旁加 `kMenuUpdateFirmwareFromFile`。
`.cc` 英文 `"Update Firmware from File..."`，中文 `"从本地文件更新固件..."`。

## TDD 测试

`core_tests.cc` 加 `UpdateFirmwareFromFile` 测试（FakeBleCentral 已捕获 `UpdateFirmware` 的 image/device_id）：

- 准备一个临时 bin 文件（如写入几个已知字节）
- 调 `coordinator.UpdateFirmwareFromFile(tmp_path, device_id, ...)`
- 断言 FakeBleCentral 收到的 `image` 内容与文件一致、`device_id` 正确
- 断言不存在文件 / 空文件 / 超 3MB 文件分别走 completion(false, ...)

先写测试跑红，再实现 coordinator。

## 验证步骤

1. **构建 Windows**：`build_win.bat`。注意假成功坑（[[windows-build-fake-success]]）——构建后必须核对 `desktop\windows\build-x64\VoiceStick.exe` 时间戳与体积。若 VoiceStick.exe 管理员进程在跑 kill 不掉（[[windows-build-admin-stale-cache]]），需提权 taskkill + `rm -rf build-x64`。
2. **CTest**：`ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests`。
3. **真机 BLE OTA**：
   - 确保设备已 BLE 连接（设备屏幕非 Pairing 闪烁态）。
   - 托盘菜单 -> 设备子菜单 -> "从本地文件更新固件..."。
   - 选 `firmware/build/voice_stick.bin`（当前 1.44MB，含 tap poll 宏关闭改动）。
   - 观察进度对话框到 100%，设备自动切分区重启。
   - 重启后串口采集：确认 `tap poll` 心跳消失（验证新固件生效）。
4. **git**：`desktop/windows/` 被 ignore，提交用 `git add -f`（[[windows-gitignore-and-signing]]）。

## 风险与确认点

- **bin 格式一致性**：已确认本地 `voice_stick.bin` 与远程 OTA bin 同为 ESP-IDF app bin，无封装头，底层直接接受。
- **版本号**：OTA 不校验版本升降级。当前 `VERSION`/`firmware/version.txt` 未改，推同版本号 bin 会正常写入切分区，设备报告版本号不变但内容为新固件。可接受（本就是验证本地构建链路）。
- **OTA commit**：复用同一 `UpdateFirmwareAsync`，结束时的 commit/切分区行为与远程路径一致，无需额外处理。
- **菜单 ID 段**：5900-5999 当前空闲（Hotkey 到 5899），无冲突。
- **设备未连接**：菜单项仅在 `connected` 时追加；底层 `UpdateFirmware` 也会兜底返回 "No VoiceStick is connected."。
