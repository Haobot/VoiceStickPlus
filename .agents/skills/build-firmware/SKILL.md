---
name: build-firmware
description: >-
  ESP-IDF 固件构建流程。当用户说"构建固件 / 编译固件 / build firmware / idf.py build" 等，或在改完 firmware/ 下 C 源码或 sdkconfig 后需要验证编译时使用。
  目标板 ESP32-S3（M5Stack StickS3），工具链 ESP-IDF v5.5.1。
---

# 固件构建（ESP-IDF）

在 ESP-IDF v5.5.1 环境下构建 Stick S3 固件，目标平台 `esp32s3`。

## 前置环境

- ESP-IDF v5.5.1（安装在 `C:\Espressif\frameworks\esp-idf-v5.5.1` 或可自定义）
- Python 3.11（通过 ESP-IDF 安装器自带）
- CMake + Ninja（通过 ESP-IDF 安装器自带）
- Xtensa ESP32-S3 交叉编译工具链（通过 ESP-IDF 安装器自带）

## 一键构建

```powershell
# 通过 idf_cli.py（Windows 上不便直接用 idf.py 时的便捷入口）
python scripts/idf_cli.py -c
```

`idf_cli.py` 自动探测 ESP-IDF 环境并执行 `idf.py set-target esp32s3` + `idf.py build`。

## 分步构建（调试用）

### 在 cmd 中（推荐 — ESP-IDF 不支持 MSys/Mingw）

```bat
call C:\Espressif\idf_cmd_init.bat
cd firmware
idf.py set-target esp32s3
idf.py build
```

### 直接调用 idf.py（绕过 export 脚本问题）

当 `export.sh`/`Initialize-Idf.ps1`/`idf_cmd_init.bat` 均不可用时，直接设置环境变量后调用：

```powershell
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.1"
$env:PATH = "C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;$env:PATH"
& "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" "C:\Espressif\frameworks\esp-idf-v5.5.1\tools\idf.py" -C firmware build
```

### 在 Git Bash 中

⚠️ **ESP-IDF 不支持 MSys/Mingw 环境**，`export.sh` 会报 `MSys/Mingw is not supported`。Git Bash 下请用 `idf_cli.py` 或切换到 cmd/PowerShell。

## 构建产物

| 产物 | 路径 | 说明 |
|---|---|---|
| `voice_stick.bin` | `firmware/build/voice_stick.bin` | 应用固件镜像 |
| `bootloader.bin` | `firmware/build/bootloader/bootloader.bin` | Bootloader |
| `partition-table.bin` | `firmware/build/partition_table/partition-table.bin` | 分区表 |

分区表 `partitions_ota.csv` 定义两个 3 MB OTA app slot 加 ~1984 KB `storage` 分区。

## 分区表变更时的首次烧录

设备从旧单应用分区表升级到当前 OTA 分区表时，需要擦除后重刷：

```sh
idf.py -p COMxx erase-flash flash monitor
```

同样需要长按前面板按键进入 Boot 模式。

## 关键 sdkconfig 配置

修改 `firmware/sdkconfig` 后必须重新构建（增量编译即可，idf.py 会自动检测依赖变化）：

| 配置 | 值 | 说明 |
|---|---|---|
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | 1 | 最大 BLE 连接数 |
| `CONFIG_BT_NIMBLE_MAX_BONDS` | 3 | 最大 bond 存储数 |
| `CONFIG_BT_NIMBLE_NVS_PERSIST` | y | Bond 持久化到 NVS（跨重启保留） |
| `CONFIG_BT_NIMBLE_SECURITY_ENABLE` | y | 启用 SMP 安全配对 |
| `CONFIG_BT_NIMBLE_SM_SC` | y | 启用 LE Secure Connections |
| `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` | 4096 | BLE Host 任务栈大小 |

## 常见构建陷阱

| 现象 | 原因 | 处理 |
|---|---|---|
| `MSys/Mingw is not supported` | Git Bash 被 ESP-IDF 拒绝 | 用 cmd / PowerShell / idf_cli.py |
| `"cmake" must be available on the PATH` | cmake 未加入 PATH | 将 `C:\Espressif\tools\cmake\<ver>\bin` 加入 PATH |
| `ESP_ROM_ELF_DIR is not defined` | 环境变量缺失 | 正常执行 export/idf_cmd_init 后构建 |
| `IDF_PATH` 指向 v5.5.3 但实际安装 v5.5.1 | 残留环境变量 | 手动 `$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.1"` |
| sdkconfig 变更未生效 | sdkconfig 被 sdkconfig.defaults 覆盖 | 确认 sdkconfig 文件修改后是最终目标值 |
