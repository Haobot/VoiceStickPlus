# Voice Stick

English | [简体中文](README.zh-CN.md)

Voice Stick turns an M5Stack StickS3 (ESP32-S3) into a desktop Bluetooth push-to-talk voice input device.

Project write-up: [Hackster.io — VoiceStick Plus](https://www.hackster.io/haobot2018/voicestick-plus-d56e05)

Hold the front button on the StickS3 to record. When you release it, the desktop app sends the audio to ASR, shows the recognized text, and pastes the final result into the currently focused input field after a short confirmation countdown. The recognized text can optionally be refined or translated by an LLM before output. macOS and Windows desktop clients are available; a browser-based flasher and update feeds live on the website.

## Features

- Two trigger styles: hold-to-talk (default) and click-to-talk.
- Double-click gestures (detected on the firmware side): double-click the front button to cancel the active session and inject Enter; double-click the side button to restore the last pending input.
- Three output targets: paste into the focused app (default), subtitle-only display, and a WeChat input-method mode that decodes Opus to PCM and renders it to a system virtual microphone (e.g. VB-CABLE) so apps like the WeChat input method can use it as an audio source (Windows).
- LLM translation and refinement; Volcengine hotword/replacement tables, selection-based hotword adding, and candidate hotword mining (Windows).
- Air mouse: BMI270 IMU-driven cursor control; while active the front button acts as left click and a side-button single click exits (Windows).
- Tap-to-arrow: IMU tap detection mapped to arrow keys (`tap_to_arrow`, Windows).
- MiniEncoderC encoder: its button mirrors the front button, rotation maps to arrow keys with slow (line-by-line) / fast (page) tiers, and button/rotation actions are customizable per device (Windows).
- Per-device overrides: output, device interaction (IMU wake / tap / air-mouse sensitivity), and encoder settings can be configured per device (tray device submenu, Windows).
- Multi-device pairing with a floating overlay/subtitles showing recognition progress.
- Firmware updates: cable-free BLE OTA, plus the VoiceStickFlash COM-port flashing tool on Windows as an unbrick fallback.

## Architecture

The device captures buttons and audio and reports them over BLE. The desktop app is the single source of truth: it owns the interaction state machine, ASR, text display, and text injection. The website hosts the landing page, browser-based USB firmware flashing, and the Sparkle/WinSparkle update feeds.

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
                                                              \-> Opus decode -> PCM -> virtual microphone (WeChat input method mode)
```

The ASR path does not decode Opus back to PCM; ASR and the debug audio cache both consume the same Ogg Opus stream. The exception is the WeChat input-method mode: the desktop app decodes Opus to PCM and renders it to a system virtual microphone instead of injecting ASR text.

## Project Layout

- `firmware/`: ESP-IDF C firmware for M5Stack StickS3 / ESP32-S3.
- `desktop/macos/`: SwiftPM/AppKit menu bar app, macOS 12+.
- `desktop/windows/`: C++20 / Win32 / C++/WinRT tray app, Windows 10 1903+.
- `desktop/linux/`: Linux desktop placeholder.
- `website/`: Vue 3 + Vite site with a Web Serial firmware flasher, bilingual landing page, and appcast pages.
- `Doc/`: BLE protocol, Volcengine ASR frame format, release process references (`Doc/Ref/`), and implementation RFCs (`Doc/Plan/`).

For build commands, code architecture, and contributor conventions, see [`CLAUDE.md`](CLAUDE.md) / [`AGENTS.md`](AGENTS.md).

## Hardware Target

- Board: M5Stack StickS3 / ESP32-S3-PICO-1-N8R8
- Front button: GPIO11, protocol `primary`, push-to-talk and deep-sleep wake
- Side button: GPIO12, protocol `secondary`, cancel or restore the last input confirmation
- PMIC IRQ: GPIO13
- IMU: BMI270 (air mouse and tap detection)
- MiniEncoderC encoder (optional): I2C @0x42 on the second I2C bus (top Hat header, SDA=G8 / SCL=G0); its button mirrors the front button, rotation maps to arrow keys, and its LED glows red while recording; it cannot wake the device from deep sleep
- Audio codec: ES8311 over I2S, 16 kHz / 16 bit / mono
- Display: 135 x 240 ST7789 portrait screen
- LCD backlight: GPIO38 PWM

Pin definitions live in `firmware/components/stick_s3_board/include/stick_s3_board.h`.

## Interaction Model

The firmware reports raw button facts (`button_down` / `button_up` with `primary` or `secondary`). The desktop app owns the interaction state machine and sends `ui_state` updates back to the firmware for the screen.

| State | Front button | Side button |
| --- | --- | --- |
| Unpaired / disconnected | No recording; screen shows `VS-XXXX` | No effective action |
| Connected idle | Hold to record (double-click injects Enter without recording) | Double-click restores the last pending input; single click does nothing |
| Recording | Release to finish recording | Single click does not cancel the active recording |
| Recognizing | New recording is ignored | Single click cancels the in-progress recognition |
| Confirmation countdown | Pause auto-paste, enter manual confirmation | Single click cancels pending text |
| Manual confirmation | Confirm paste | Single click cancels pending text |

Double-click detection happens on the firmware side; the desktop app handles `button_double_click` uniformly: a front-button double-click cancels any active recording/subtitle session before injecting Enter, and a side-button double-click restores the last pending input (ignored in air-mouse mode). The side-button single-click cancel semantics only apply while a recording/recognition/pending paste is active; in air-mouse mode a single click exits air mouse; when truly idle it does nothing.

Two interaction modes are supported: `hold_to_talk` (default) and `click_to_talk` (the WeChat input-method mode has its own `[wechat_input_method].trigger_mode`). Text output supports `focused_app` (default; pastes into the focused field and presses Return by default), `subtitle` (display only), and `wechat_input_method` (virtual microphone, no ASR text injection). Results can be refined or translated through an OpenAI-compatible LLM, and output settings can be overridden per device.

## BLE Protocol

GATT service UUID: `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`

| Name | UUID | Direction | Properties | Payload |
| --- | --- | --- | --- | --- |
| `audio_tx` | `…5101` | Device → Host | notify | Opus audio frames |
| `state_tx` | `…5102` | Device → Host | notify | Button events (incl. `button_double_click`), battery, firmware version, air-mouse motion frames |
| `control_rx` | `…5103` | Host → Device | write without response | `ui_state`, interaction/tap/air-mouse settings, `ota_commit` |
| `ota_rx` | `…5104` | Host → Device | write / write without response | BLE OTA control and data frames |
| `ota_tx` | `…5105` | Device → Host | notify | BLE OTA state frames |

See `Doc/Ref/protocol.md` for the full frame format. When changing BLE messages, update the firmware, macOS, Windows, and the docs together.

## Build

### Firmware (ESP-IDF v5.5.1, target `esp32s3`)

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

On Windows, `python scripts/idf_cli.py -cus -p COM17` wraps build + flash + monitor. Devices still on the old single-app partition table need one `erase-flash` before the OTA partition table takes effect. The partition table is two 3 MB OTA app slots plus a ~1984 KB `storage` partition.

### macOS desktop (SwiftPM, macOS 12+)

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

The app is a menu bar accessory and requests Bluetooth permission. Text injection simulates `Command-V` plus an optional Return, so grant Accessibility permission if macOS blocks the keystrokes.

### Windows desktop (CMake + Ninja + MSVC 2022 x64)

```bat
build_win.bat
```

Or manually, from a VS 2022 x64 developer environment:

```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
ctest --test-dir desktop\windows\build-x64 --output-on-failure
desktop\windows\build-x64\VoiceStick.exe
```

The `VoiceStickApp` CMake target outputs `VoiceStick.exe`. Firmware upgrades go over BLE OTA (desktop app) or USB serial flashing; there is no Wi-Fi/LAN OTA path.

### Website (Vue 3 + Vite, Node 22)

```sh
cd website
npm install
npm run dev      # local dev server
npm run build    # minimal verification
```

The browser flasher uses Web Serial to write the merged firmware image from the manifest at `VITE_FIRMWARE_MANIFEST_URL`.

## Configuration

Config paths:

- macOS: `~/Library/Application Support/VoiceStick/config.toml`
- Windows: `%APPDATA%\VoiceStick\config.toml`

Create it from the example at `desktop/macos/Config/config.example.toml`.

| Field | Description |
| --- | --- |
| `asr_provider` | `volcengine`, `voicestick_cloud`, or `tencent` |
| `volcengine_api_key` | Direct Volcengine API key (`X-Api-Key`) |
| `volcengine_boosting_table_id` / `volcengine_correct_table_id` | Volcengine self-learning platform hotword/replacement table IDs (created in console); sent as `corpus.boosting_table_id` / `corpus.correct_table_id` |
| `voicestick_api_key` / `voicestick_cloud_url` | VoiceStick Cloud relay key and WebSocket URL |
| `llm_base_url` / `llm_api_key` / `llm_model` | OpenAI-compatible LLM for translation and refinement |
| `refine_enabled` / `refine_prompt` | Refine ASR text with an LLM (trim filler spaces, fix punctuation, drop fillers); default `false` (must be enabled manually). Empty prompt uses the built-in default |
| `hotword_process_enabled` | Use the LLM to refine hotwords when adding from text selection (Windows); default `false`, reuses the `llm_*` settings |
| `hotword_mining_enabled` | After each session, asynchronously ask the LLM to extract candidate hotwords from the final text (Windows); candidates reaching the threshold (3) are offered via tray notification and Settings → Hotwords for manual confirmation; default `false` (one extra LLM call per session), reuses the `llm_*` settings |
| `hotword_process_prompt` | Override prompt for hotword refinement; empty uses the built-in default |
| `interaction_mode` | `hold_to_talk` or `click_to_talk` (trigger mode for focused_app/subtitle; wechat uses `[wechat_input_method].trigger_mode`) |
| `tap_to_arrow` / `tap_sensitivity` | IMU tap-to-arrow-keys mapping and its sensitivity (Windows); ignored in air-mouse mode |
| `[wechat_input_method]` | WeChat input-method mode: independent `trigger_mode`, virtual microphone endpoint name, global hotkeys, etc. (Windows); see `Doc/Ref/desktop-config.md` |
| `resource_id` | Volcengine resource ID |
| `asr_hotwords` | Comma-separated ASR hotwords; also passed to the LLM as terminology hints |
| `paired_device_ids` | Comma-separated 4-digit hex IDs, e.g. `C3D8,09AF` |
| `device_theme_colors` / `device_overlay_positions` | Optional per-device overlay color and position |
| `auto_enter` | Press Return after paste |
| `debug_audio_cache` / `debug_audio_dir` | Save debug Ogg Opus files and where (Windows default: `%LOCALAPPDATA%\VoiceStick\DebugAudio`) |
| `[output].target` | `focused_app`, `subtitle`, or `wechat_input_method` (virtual microphone, Windows) |
| `[output].transform` | `original` or `translate` |
| `[output].translation_target` | Target language code, e.g. `en` or `zh-Hans` |
| `[device.<id>.output]` | Per-device override of transform and translation target |
| `[device.<id>.interaction]` / `[device.<id>.encoder]` | Per-device overrides of interaction settings (IMU wake / tap / air-mouse sensitivity) and encoder settings (Windows, via the tray device submenu "Interaction Settings…" / "Encoder Settings…") |

Supported Volcengine `resource_id` values: `volc.seedasr.sauc.duration`, `volc.seedasr.sauc.concurrent`, `volc.bigasr.sauc.duration`, `volc.bigasr.sauc.concurrent`.

Do not commit API keys.

## Pairing

1. Flash and boot the StickS3. The screen shows `VS-XXXX`.
2. Start the desktop app.
3. Open `Pair Device…`, select the matching `VS-XXXX` in the scan list, and pair.
4. The app then scans for and connects to that device. Repeat to pair additional devices.

You can also edit `paired_device_ids` manually. When IDs are saved, the desktop app ignores nearby unpaired VoiceStick devices.

## Firmware Updates

- **BLE OTA**: the desktop app checks a signed-by-hash manifest on launch, device connect/reconnect, and manual refresh, then pushes the OTA image over BLE per connected device after verifying size and SHA-256.
- **VoiceStickFlash (Windows)**: a standalone COM-port firmware flashing tool shipped with the MSI and the portable package (self-contained esptool runtime, no system Python needed). It supports three modes — full image (merged bin @ 0x0), app partition only (@ 0x10000), and full erase then full image — as an unbrick/fallback path alongside BLE OTA. Entry points: the tray menu "Firmware Flash Tool…" or the firmware update dialog's "Advanced… COM flash" button.
- **USB serial flashing**: the browser flasher (or `idf.py flash`) writes the merged image at offset `0x0` over USB.

See `Doc/Ref/release.md` for the full release process.

## Debug Audio

Set `debug_audio_cache = true` to save each valid recognition session as a playable Ogg Opus file. Recordings shorter than 0.5 seconds are discarded and not sent to ASR.

## License

See [LICENSE](LICENSE).
