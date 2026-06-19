# Voice Stick — Agent Guide

This document summarizes the Voice Stick project for AI coding agents. Voice Stick turns an M5Stack StickS3 (ESP32-S3) into a Bluetooth push-to-talk input device for macOS and Windows. Hold the front button to record speech; the desktop app sends the audio to ASR, shows recognized text, and pastes it into the focused input field after a short confirmation countdown.

---

## Project Overview

- **Repository**: `78/voicestick`
- **Current version**: see `VERSION` (also mirrored in `firmware/version.txt` and `website/package.json`)
- **Hardware target**: M5Stack StickS3 / ESP32-S3-PICO-1-N8R8
- **Firmware**: ESP-IDF v5.5.1, C, FreeRTOS
- **macOS app**: Swift Package, macOS 12+, menu-bar accessory
- **Windows app**: Win32/C++20, CMake, WiX MSI packaging
- **Website**: Vue 3 + Vite + `vue-i18n`, deployed to GitHub Pages
- **ASR providers**: direct Volcengine ASR or VoiceStick Cloud relay

The runtime audio path is:

```text
StickS3 mic -> ES8311/I2S -> 16 kHz PCM -> Opus -> BLE -> desktop -> Ogg Opus -> ASR WebSocket -> paste
```

The desktop app never decodes Opus back to PCM; it forwards Ogg Opus directly to ASR.

---

## Repository Layout

```text
firmware/                  ESP-IDF firmware for StickS3
  main/main.c              Application task loop, button/power/deep-sleep logic
  components/
    stick_s3_board/        Board init, I2C, PMIC, battery, pin definitions
    audio_pipeline/        I2S mic -> Opus encode (uses esp_codec_dev, esp-opus)
    voice_ble/             NimBLE peripheral, GATT service, OTA receiver
    ui_status/             LVGL-based screen state and sprites
desktop/
  macos/                   Swift Package app (Package.swift)
  windows/                 Win32/C++20 CMake app
  linux/                   Placeholder workspace, not implemented yet
website/                   Vue + Vite homepage and Sparkle/WinSparkle appcast
  public/appcast.xml       Generated update feed consumed by both desktop apps
  src/                     Vue single-file components, i18n JSONs
docs/                      Protocol, ASR, and release documentation
scripts/                   Build helpers, sprite conversion, appcast updater
.github/workflows/         GitHub Actions release and website deploy
```

---

## Technology Stack

### Firmware

- **Framework**: ESP-IDF v5.5.1
- **Language**: C (C99/C11 style)
- **RTOS**: FreeRTOS (tasks, queues, timers, tickless idle)
- **Bluetooth**: NimBLE peripheral, single connection, MTU 247
- **Audio**: ES8311 codec via `esp_codec_dev`, Opus via `78/esp-opus`
- **Display**: LVGL 9.2.0 on ST7789P3 135×240 portrait LCD
- **Power**: AXP2101 PMIC, deep-sleep wake on front button GPIO11

Managed ESP-IDF components (declared in `idf_component.yml` files):

- `espressif/button` (main)
- `espressif/esp_codec_dev` (audio_pipeline)
- `78/esp-opus` (audio_pipeline)
- `lvgl/lvgl` (ui_status)

### macOS Desktop

- **Build system**: Swift Package Manager (`Package.swift`)
- **Language**: Swift 5.9+
- **Minimum OS**: macOS 12
- **Key dependencies**:
  - Sparkle 2.x (auto-update)
  - TOMLKit (config parsing)
  - CZlib target (Ogg CRC)
- **Output**: `VoiceStickApp` executable, packaged as `VoiceStick-<version>.app`

### Windows Desktop

- **Build system**: CMake + Ninja
- **Language**: C++20
- **UI**: Win32/GDI/Direct2D, tray icon, overlay/subtitle windows
- **Dependencies**:
  - WinSparkle 0.9.2 (auto-update, fetched by CMake)
  - cJSON and tomlplusplus (vendored in `third_party/`)
- **Output**: `VoiceStick.exe`, WiX MSI `VoiceStick_<version>.msi`

### Website

- **Framework**: Vue 3, Vite 7
- **I18n**: `vue-i18n` (zh-CN, en-US; auto-detects `zh`)
- **ESP flashing**: `esptool-js` browser flasher
- **Hosted on**: GitHub Pages at `https://78.github.io/voicestick/`

---

## Build and Test Commands

### Firmware

Requires ESP-IDF v5.5.1 (adjust path to your checkout):

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

If the Python venv is missing, run `install.sh` once:

```sh
"$HOME/esp/v5.5.1/esp-idf/install.sh" esp32s3
```

The firmware uses a custom OTA partition table (`partitions_ota.csv`). To switch from the old single-app layout, erase and reflash once:

```sh
idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor
```

### macOS App

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

Release build (local ad-hoc signature unless Developer ID certificate is present):

```sh
SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
scripts/make-dmg.sh
```

Outputs in `build/`:

- `VoiceStick-<version>.app`
- `VoiceStick-<version>.zip`
- `VoiceStick-<version>.signature`
- `VoiceStick-<version>.dmg`

### Windows App

From a PowerShell prompt with Visual Studio installed:

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S desktop\windows -B desktop\windows\build-x64 -G Ninja -DCMAKE_MAKE_PROGRAM="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"'
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C desktop\windows\build-x64'
```

Run tests:

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir desktop\windows\build-x64 --output-on-failure'
```

Run the app:

```powershell
desktop\windows\build-x64\VoiceStick.exe
```

Signed MSI build (requires a code-signing certificate thumbprint):

```powershell
$env:SIGNING_SHA1 = "YOUR_CERT_THUMBPRINT"
scripts\build-msi.bat
```

### Website

```sh
cd website
npm install
npm run dev      # local dev server
npm run build    # outputs to website/dist
```

The production build expects:

```text
VITE_FIRMWARE_MANIFEST_URL=https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/latest/manifest.json
```

---

## Code Organization

### Firmware Components

| Component | Responsibility |
| --- | --- |
| `main/main.c` | FreeRTOS app event queue, button state machine, deep-sleep/power logic, OTA UI handling |
| `stick_s3_board` | Pin definitions, I2C bus init, PMIC/AXP2101 battery and charging APIs |
| `audio_pipeline` | I2S mic read, Opus encode, session lifecycle |
| `voice_ble` | NimBLE GATT service, audio/state notifications, control/OTA RX, device naming (`VS-XXXX`) |
| `ui_status` | LVGL screens: pairing, ready, recording, thinking, battery, OTA progress, error |

### macOS App (`desktop/macos/Sources/VoiceStickApp`)

| File | Responsibility |
| --- | --- |
| `AppDelegate.swift` | Menu-bar app lifecycle, status item, settings |
| `BleCentral.swift` / `BleProtocol.swift` | CoreBluetooth scanning, GATT I/O, frame parsing |
| `VoiceStickCoordinator.swift` | Central state machine: recording, ASR, confirmation countdown, paste |
| `ASRWebSocketClient.swift` | Volcengine / VoiceStick Cloud WebSocket client |
| `OggOpusMuxer.swift` / `OggCRC.swift` | Wraps Opus packets into Ogg Opus |
| `InputInjector.swift` | Simulated `Cmd+V` and optional Return |
| `LLMTranslationClient.swift` | OpenAI-compatible LLM translation |
| `AppConfig.swift` | TOML config load/save, per-device overrides |
| `FirmwareManifest.swift` / `FirmwareUpdateWindowController.swift` | OTA manifest fetch, BLE OTA transfer |
| `OverlayController.swift` / `SubtitleController.swift` | Floating overlay and subtitle window UI |
| `PairDeviceWindowController.swift` / `OnboardingWindowController.swift` | Pairing and onboarding flows |
| `DebugAudioRecorder.swift` | Optional Ogg Opus session cache |

### Windows App (`desktop/windows/src`)

| File | Responsibility |
| --- | --- |
| `main.cc` / `win32_app.cc` | Win32 tray app shell, message loop |
| `ble_central_win.cc` / `ble_protocol.cc` | Windows BLE scan/connect and protocol framing |
| `voice_stick_coordinator.cc` | Shared core state machine (mirrors macOS behavior) |
| `asr_client_win.cc` / `asr_protocol.cc` | ASR WebSocket and binary protocol framing |
| `ogg_opus_muxer.cc` / `ogg_crc.cc` | Ogg Opus muxing |
| `input_injector_win.cc` | Clipboard paste + `SendInput` |
| `llm_translation_client.cc` | Translation via LLM |
| `app_config.cc` | TOML config parsing |
| `overlay_window.cc` / `subtitle_window.cc` | Overlay and subtitle UI |
| `firmware_manifest.cc` / `firmware_update_dialog.cc` | OTA manifest and update prompt |
| `tests/core_tests.cc` | Unit/integration tests for protocol, muxer, ASR framing, coordinator behavior |

---

## Development Conventions

### Firmware

- C code uses `snake_case` for functions/variables, `SCREAMING_SNAKE_CASE` for macros.
- Public component headers live in `components/<name>/include/`.
- ESP_LOG tags use short module names (`voice_stick`, `voice_ble`, etc.).
- All FreeRTOS resources are created in `app_main` or dedicated `init_*` helpers.
- Power management is explicit: CPU-frequency locks during recording and OTA, tickless idle/light sleep otherwise.
- Button events are debounced by `espressif/button`; the app owns business semantics.

### macOS

- Swift code follows standard Swift naming (`PascalCase` types, `lowerCamelCase` members).
- Swift concurrency is mixed with traditional delegate/callback patterns where CoreBluetooth requires it.
- UI strings and config keys prefer English identifiers; user-facing copy is localizable.

### Windows

- Google C++ style: `snake_case` file names, `CapWords` types, `MixedCase()` methods, `snake_case` variables, 4-space indentation.
- All Win32-specific code is isolated in `*_win.cc` files; protocol/muxer/config code is intended to be platform-neutral.
- Third-party code is vendored under `third_party/` and not edited.

### General

- Version is the single source of truth in `VERSION`. `firmware/version.txt` must match it for OTA detection.
- Git tags are `v<version>` (e.g., `v0.3.4`) and must match `VERSION`.
- Do not commit API keys, signing certificates, or Sparkle private keys.

---

## Testing Strategy

- **Firmware**: no automated test suite in this repository; validation is via `idf.py monitor`, hardware-in-the-loop, and manual BLE/protocol checks.
- **macOS**: no unit-test target in the Swift package; test via `swift run` and manual end-to-end recording/paste flows.
- **Windows**: `tests/core_tests.cc` runs through CTest. It covers:
  - BLE device-ID normalization and frame parsing
  - Ogg Opus muxer output
  - ASR protocol frame construction/parsing
  - `AppConfig` defaults and output-profile lookup
  - Firmware manifest parsing and version comparison
  - `VoiceStickCoordinator` state-machine behavior (recording, cancel, confirmation pause, subtitle mode, click-to-talk, multi-device)

Run Windows tests after every core protocol or coordinator change.

---

## Configuration

### macOS

Copy the example config and edit:

```sh
mkdir -p "$HOME/Library/Application Support/VoiceStick"
cp desktop/macos/Config/config.example.toml "$HOME/Library/Application Support/VoiceStick/config.toml"
```

### Windows

Config path:

```text
%APPDATA%\VoiceStick\config.toml
```

### Key Config Fields

| Field | Description |
| --- | --- |
| `asr_provider` | `volcengine` or `voicestick_cloud` |
| `volcengine_api_key` | Direct Volcengine API key |
| `voicestick_api_key` / `voicestick_cloud_url` | VoiceStick Cloud relay credentials |
| `llm_base_url` / `llm_api_key` / `llm_model` | OpenAI-compatible translation LLM |
| `interaction_mode` | `hold_to_talk` or `click_to_talk` |
| `paired_device_ids` | Comma-separated 4-digit hex IDs |
| `device_theme_colors` | Optional per-device overlay colors |
| `device_overlay_positions` | Optional per-device overlay positions |
| `auto_enter` | Press Return after paste |
| `debug_audio_cache` | Save each session as Ogg Opus |
| `[output]` | `target` (`focused_app`/`subtitle`), `transform` (`original`/`translate`), `translation_target` |
| `[device.<id>.output]` | Per-device output overrides |

---

## BLE Protocol

Custom GATT service:

```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```

Characteristics:

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `...01` | StickS3 → Desktop | notify |
| `state_tx` | `...02` | StickS3 → Desktop | notify |
| `control_rx` | `...03` | Desktop → StickS3 | write without response |
| `ota_rx` | `...04` | Desktop → StickS3 | write / write without response |
| `ota_tx` | `...05` | StickS3 → Desktop | notify |

The device advertises as `VS-XXXX`, where `XXXX` is the last two bytes of the eFuse MAC. The desktop app only connects to paired IDs.

Frame formats, OTA protocol, state machines, and ASR request/response details are documented in `docs/protocol.md`, `docs/volcengine-asr.md`, and `docs/release.md`.

---

## Release and Deployment

Releases are coordinated across three artifacts:

1. macOS app (signed, notarized DMG/ZIP)
2. StickS3 firmware (OTA + merged binary)
3. Windows MSI (signed locally)

### Version Files

Update before tagging:

- `VERSION`
- `firmware/version.txt`

Tag must be `v<VERSION>`.

### GitHub Actions

- `.github/workflows/release.yml`
  - Triggers on `v*` tags or manual dispatch.
  - Builds firmware with ESP-IDF v5.5.1 and uploads to Aliyun OSS under `voicestick/firmwares/<version>/` and `voicestick/firmwares/latest/`.
  - Builds and signs the macOS app, creates DMG, uploads assets to the matching GitHub Release.
  - Regenerates `website/public/appcast.xml` and triggers website deploy.

- `.github/workflows/deploy-website.yml`
  - Runs on `website/**` changes or manual dispatch.
  - Reads the latest GitHub Release and live appcast, regenerates `appcast.xml`, builds the Vue site, and deploys `website/dist` to GitHub Pages.

### Windows Signing

The Windows MSI is built and signed on a local machine because the certificate is local hardware/USB key state:

```bat
scripts\build-msi.bat
```

Upload the resulting MSI to the matching GitHub Release, then re-run the website deploy workflow so `appcast.xml` includes it.

### Required Repository Secrets/Variables

Release workflow expects:

- `SPARKLE_PUBLIC_ED_KEY`
- `SPARKLE_PRIVATE_ED_KEY`
- `MACOS_CERTIFICATE_P12`
- `MACOS_CERTIFICATE_PASSWORD`
- `APPLE_ID`
- `APPLE_TEAM_ID`
- `APPLE_APP_SPECIFIC_PASSWORD`
- `ALIYUN_OSS_ACCESS_KEY_ID`
- `ALIYUN_OSS_ACCESS_KEY_SECRET`
- `ALIYUN_OSS_ENDPOINT`
- `ALIYUN_OSS_BUCKET`
- Variable `ALIYUN_OSS_PUBLIC_BASE_URL`
- Variable `ALIYUN_OSS_PREFIX` (defaults to `voicestick/firmwares`)

### Post-Release Verification

Confirm these endpoints return `200`:

- `https://78.github.io/voicestick/appcast.xml`
- `https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/latest/manifest.json`
- GitHub Release assets for macOS ZIP/DMG, Windows MSI, and firmware binaries

See `docs/release.md` for the full standard, Windows-first, and Windows-afterward flows.

---

## Security Considerations

- **API keys**: Volcengine, VoiceStick Cloud, and LLM keys live in the local TOML config file. Never commit them; the example config contains placeholders only.
- **Firmware updates**: the desktop app downloads a JSON manifest over HTTPS, verifies `ota_size` and `ota_sha256`, then performs BLE OTA. The manifest is hosted on Aliyun OSS; the app does not accept unsigned or mismatched firmware images.
- **macOS updates**: Sparkle EdDSA-signed ZIP packages. The public key is embedded at build time via `SPARKLE_PUBLIC_ED_KEY`; the private key stays in CI secrets or the release maintainer's keychain.
- **Windows updates**: WinSparkle uses the same `appcast.xml` feed. The MSI is code-signed with a hardware-protected certificate before upload.
- **Input injection**: macOS requires Accessibility permission for simulated `Cmd+V`; Windows uses clipboard + `SendInput`. Both are local-only, user-initiated actions.
- **BLE pairing**: the desktop app only connects to explicitly paired `VS-XXXX` device IDs; unpaired devices are ignored when any paired IDs exist.

---

## Useful References

- `README.md` — full user-facing setup, pairing flow, and config reference.
- `docs/protocol.md` — BLE frame formats, state machines, OTA protocol.
- `docs/volcengine-asr.md` — Volcengine ASR endpoint and request details.
- `docs/release.md` — step-by-step release and verification process.
- `firmware/components/stick_s3_board/include/stick_s3_board.h` — hardware pin map.
