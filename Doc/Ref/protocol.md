# Voice Stick Protocol

This document describes the protocol implemented by the current firmware and macOS desktop app.

## Goals

- Low-latency push-to-talk audio from StickS3 to macOS.
- Opus over BLE to keep wireless bandwidth low.
- Ogg Opus forwarding from macOS to either Volcengine ASR or the VoiceStick Cloud relay.
- Final ASR text insertion into the focused macOS input field after release and confirmation.

## BLE GATT

Device name: `VS-XXXX`, where `XXXX` is derived from the last two bytes of the device eFuse MAC.

Service UUID:

```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```

Characteristics:

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | StickS3 -> Mac | notify |
| `state_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | StickS3 -> Mac | notify |
| `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | Mac -> StickS3 | write without response |
| `ota_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104` | Mac -> StickS3 | write, write without response |
| `ota_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105` | StickS3 -> Mac | notify |

The desktop app scans for this service and only connects to devices whose `VS-XXXX` ID is present in the local paired-device list. Multiple paired devices may be connected at the same time; audio, state, control, and OTA handling are scoped by CoreBluetooth peripheral identity.

## Audio Frame

All multibyte fields are little-endian.

```text
struct AudioBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x01 audio
  uint16_t header_len;    // 16
  uint32_t session_id;
  uint32_t seq;
  uint8_t  flags;         // bit0=start, bit1=end
  uint8_t  reserved;      // currently 0
  uint16_t payload_len;
  uint8_t  payload[payload_len];
}
```

The payload contains one raw Opus packet when `payload_len > 0`. The firmware currently encodes 60 ms of 16 kHz mono audio per packet. When recording stops, the firmware also sends an end frame with `flags & 0x02` and an empty payload.

The macOS app wraps incoming Opus packets into an Ogg Opus stream before sending them to ASR. It does not decode Opus to PCM.

## State Event

All multibyte fields are little-endian.

```text
struct StateBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x10 state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

State events report device facts from the firmware to the app. They do not carry
business actions such as "cancel" or "confirm"; the app owns that interpretation.

Frame size budget: a state notification must fit the negotiated ATT MTU —
`4 (header) + payload_len ≤ att_mtu − 3`. On Windows links the MTU is typically
247, so the JSON budget is 240 bytes. Oversized frames get truncated on the air
and the peer drops them on the `payload_len` check (`device_info` once exceeded
this and was slimmed; new capabilities should use separate small frames such as
`encoder_status`). The firmware logs a warning when a state frame exceeds the
budget.

Currently emitted state events:

```json
{"event":"device_info","hardware":"stick_s3","firmware_version":"0.2.2","buttons":["primary","secondary"],"interaction_modes":["hold_to_talk","click_to_talk"],"ui_states":["ready","recording","thinking","pending_confirmation","error","air_mouse"]}
{"event":"encoder_status","present":true}
{"event":"button_down","button":"primary","session_id":1234}
{"event":"button_up","button":"primary","duration_ms":620,"session_id":1234}
{"event":"button_down","button":"secondary"}
{"event":"button_up","button":"secondary","duration_ms":90}
{"event":"button_double_click","button":"primary"}
{"event":"button_click","button":"primary","duration_ms":131,"source":"encoder"}
{"event":"tap","button":"double"}
{"event":"encoder_rotate","direction":"cw","steps":2}
```

`encoder_status` (added after v2.2.0) reports whether the firmware detected
the MiniEncoderC rotary encoder (I2C @0x42) at boot. It is a separate small
frame sent right after `device_info` on state subscription — `device_info`
itself is already near the BLE notification MTU limit and must not grow. The
firmware also pushes `encoder_status` when the encoder later degrades offline
(consecutive I2C failures); if the link is down at that moment the flag is
cached and the next connection reports the current state. Older firmware
never sends this event — desktops must treat its absence as "present" so the
encoder settings stay visible for old firmware. The Windows desktop uses it
to show/hide the encoder section in the settings dialog (any known device
reporting present keeps the section visible).

Buttons are named by role instead of physical placement. On StickS3, the front
button maps to `primary` and the side button maps to `secondary`. `session_id` is
included when a `primary` press starts or stops a local audio recording.

The button events (`button_down` / `button_up` / `button_click` /
`button_double_click`) carry an optional `source` field identifying the input
origin. Events from the MiniEncoderC encoder button (on the top Hat header)
include `"source":"encoder"`; physical-button and remote (hotkey) events omit
the field. Older desktops ignore the unknown field, so no migration is needed.

`button_double_click` is emitted when the firmware detects two consecutive short
presses of the primary button within 500 ms (each press < 300 ms). The desktop
responds by injecting an Enter key event. See `Doc/Plan/primary-button-double-click.md`.

The firmware also emits `button_double_click` with `button:"secondary"` when it
detects two consecutive short presses of the side button within 500 ms. To
distinguish single from double clicks, a single side press is deferred: the
`button_click` with `button:"secondary"` is sent only after the 500 ms
double-click window expires. The desktop maps a secondary single-click to
entering/leaving air-mouse mode and a secondary double-click to restoring the
last input confirmation. See `Doc/Plan/imu-air-mouse.md`.

`tap` is emitted when the firmware detects a double-tap on the device casing
(via the BMI270 accelerometer + gyroscope software state machine). The `button`
field carries the tap kind (`"double"`). The desktop responds by injecting a Down
arrow key event when `tap_to_arrow` is enabled. See `Doc/Plan/imu-tap-detection.md`.

`encoder_rotate` is emitted when the firmware's MiniEncoderC rotary encoder
(I2C @0x42, on the top Hat header, SDA=G8 / SCL=G0) accumulates rotation within one 10 ms poll window.
`direction` carries the raw physical direction (`"cw"` | `"ccw"`) and `steps`
the accumulated detents in that direction (>= 1). The firmware performs no
semantic mapping; the desktop maps rotation to arrow keys (default cw → Down,
ccw → Up, one key press per step, flippable via `encoder_rotation_invert`) and
only injects when idle — gated off while recording, recognizing, or in
air-mouse mode, mirroring the `tap_to_arrow` gating. The master switch is
`encoder_to_arrow`. The desktop additionally clamps `steps` to 64 per frame
as a defensive bound against malformed frames. Since `steps` is counted by
the firmware against its fixed 10 ms poll window, the desktop derives a
per-window detent speed (`steps * 100` detents/s, immune to BLE jitter) and
feeds it into an EWMA speed estimator (`EncoderRotateSpeedEstimator`,
alpha = 0.5 per event, cold-started from zero on each new gesture after a
>250 ms silence; see `desktop/windows/src/encoder_speed.h`). The smoothing
is required because a single 10 ms window quantizes speed to multiples of
100 detents/s — with a raw per-window compare, every threshold between
100 and 200 behaves identically and an occasional 2-step window during
normal rotation falsely triggers the fast tier. The desktop switches to
fast-tier keys (`encoder_rotate_cw_fast_key` /
`encoder_rotate_ccw_fast_key`, default PageDown/PageUp) when the smoothed
speed reaches `encoder_rotate_fast_threshold` (default 200 detents/s, slider
range 100–300 in the settings dialog). A fast
flick is treated as a single gesture: it injects the fast key once and
enters a spin-down lockout that suppresses all rotation output (including
deceleration-phase slow events and direction changes) until the encoder
comes to a stop — detected as a >250 ms silence with no rotation events —
after which slow/fast recognition resumes. To avoid mis-injecting the
acceleration phase of a fast flick as slow rotation, slow events are
deferred by a short decision window (`encoder_rotate_decide_window_ms`,
default 80 ms, 0 disables): pending slow steps are discarded if a fast
event arrives within the window, otherwise flushed (batched) on expiry.

### Motion Frame

Air-mouse motion is a high-rate stream (~50 Hz), so it uses a compact binary frame
on the same `state_tx` characteristic instead of JSON. Consumers dispatch on the
second byte (`type`): `0x10` is the JSON state event above, `0x11` is a motion frame.

All multibyte fields are little-endian.

```text
struct MotionBleFrame {
  uint8_t  version;   // 1
  uint8_t  type;      // 0x11 motion
  int16_t  dx;        // horizontal angular-rate, right positive (scaled dps)
  int16_t  dy;        // vertical angular-rate, down positive (scaled dps)
}
```

`dx`/`dy` are gyro-bias-corrected, dead-zoned angular-rate samples (dps) scaled by
`AIR_MOUSE_REPORT_GAIN` (=4.0, so 1 dps → 4 units, 0.25 dps resolution per integer
step) and clamped to ±`AIR_MOUSE_MAX_DELTA` (=8000, i.e. up to ~2000 dps before
saturation). They are **scaled angular rates, NOT cursor deltas**: the desktop owns
the gain/acceleration curve and integrates them into cursor motion. The int16 range
(±32767) is intentionally under-used to leave headroom for fast flicks.
Motion frames are emitted only while air-mouse mode is enabled
(see `air_mouse_enabled` control event). See `Doc/Plan/imu-air-mouse.md`.

Deprecated firmware-to-app events:

| Event | Replacement | Reason |
| --- | --- | --- |
| `press_start` | `button_down` with `button:"primary"` | The old name assumed the front button and implied recording semantics. |
| `press_end` | `button_up` with `button:"primary"` | The old name implied recording semantics and did not include a button role. |
| `cancel` | `button_down` / `button_up` with `button:"secondary"` | The old event encoded app meaning; the same button can cancel, restore, or be ignored depending on app state. |

## Control Event

The desktop app writes compact JSON to `control_rx`. Control events are authoritative UI
state from the app to the firmware display.

Current desktop events:

```json
{"event":"ui_state","state":"ready","text":""}
{"event":"ui_state","state":"recording","text":""}
{"event":"ui_state","state":"thinking","text":"partial text"}
{"event":"ui_state","state":"pending_confirmation","text":"final text"}
{"event":"ui_state","state":"error","text":"ASR timeout"}
{"event":"ui_state","state":"air_mouse","text":""}
{"event":"interaction_mode","mode":"hold_to_talk"}
{"event":"interaction_mode","mode":"hold_to_talk_instant"}
{"event":"interaction_mode","mode":"click_to_talk"}
{"event":"show_imu_debug","enabled":true}
{"event":"imu_wake_sensitivity","threshold":500}
{"event":"tap_enabled","enabled":true}
{"event":"tap_sensitivity","level":5}
{"event":"air_mouse_enabled","enabled":true}
{"event":"encoder_led_color","color":"red"}
{"event":"encoder_recording_gate","enabled":true}
```

| Event | Field | Direction | Meaning |
| --- | --- | --- | --- |
| `ui_state` | `state`: string, `text`: string | Mac -> StickS3 | Authoritative display state from the app to the firmware display. |
| `interaction_mode` | `mode`: string | Mac -> StickS3 | Controls the front-button behavior and idle screen hint. |
| `show_imu_debug` | `enabled`: boolean | Windows -> StickS3 | Toggles the on-screen IMU acceleration debug overlay. Default false. |
| `imu_wake_sensitivity` | `threshold`: integer (LSB) | Windows -> StickS3 | Sets the pick-up/shake-to-wake sensitivity threshold. Recommended range 50–2000 LSB; lower values are more sensitive. Default 800 LSB. |
| `tap_enabled` | `enabled`: boolean | Windows -> StickS3 | Enables/disables the double-tap on-device gesture detection. Default false. |
| `tap_sensitivity` | `level`: integer (1..10) | Windows -> StickS3 | Sets the double-tap detection sensitivity. 1=least sensitive (hardest tap), 10=most sensitive (lightest tap). Default 5. |
| `air_mouse_enabled` | `enabled`: boolean | Windows -> StickS3 | Enables/disables air-mouse mode. When enabled, the firmware calibrates the gyro zero-bias and starts emitting `motion` frames; when disabled, it stops the motion poll. The desktop pairs this with a `ui_state:air_mouse` so the device shows an air-mouse indicator — in this state the primary button acts as the left mouse button, not recording. Default false. |
| `encoder_led_color` | `color`: string | Windows -> StickS3 | Sets the MiniEncoderC LED color shown while recording. Presets: `red`, `green`, `blue`, `yellow`, `purple`, `cyan`, `white`, `off` (`off` keeps the LED dark even while recording). Unknown color names are ignored. Persisted in firmware NVS. Default `red`. |
| `encoder_recording_gate` | `enabled`: boolean | Windows -> StickS3 | Gates whether the MiniEncoderC button starts a recording session. When disabled, encoder presses never emit `button_down`/`button_up` and never start audio; they only feed the firmware double-click window, which emits `button_click`/`button_double_click` with `source:"encoder"`. The physical primary button and `remote_button_*` control events are not gated. Persisted in firmware NVS. Default true. |

For `ui_state`, the desktop helper always includes a `text` field; older firmware
can ignore it. Firmware may immediately render local physical feedback, such as
showing the recording cat when the primary button starts audio, but the app's
`ui_state` is the authoritative display state. Current StickS3 firmware does not
render recognition text on-device because the LVGL font set does not include
Chinese glyphs; `text` is used only to choose fixed English hints.

`interaction_mode` controls the front-button behavior and idle screen hint.
`hold_to_talk` starts audio on primary down and stops on primary up.
`hold_to_talk_instant` behaves like `hold_to_talk` but skips the 300ms hold
threshold — audio starts immediately on press, minimizing press-to-popup latency
in `wechat_input_method` mode. `click_to_talk` starts audio on the first primary
click and stops on the next primary click. Older firmware ignores unknown mode
values and keeps the previous mode.

`encoder_led_color` and `encoder_recording_gate` configure the MiniEncoderC
rotary encoder. Both are written by the Windows settings dialog and persisted
in firmware NVS (`save_encoder_settings_to_nvs`), so they survive reboots.
`encoder_led_color` accepts the presets `red`, `green`, `blue`, `yellow`,
`purple`, `cyan`, `white` and `off` — `off` means the encoder LED stays dark
even while recording. `encoder_recording_gate` derives from the desktop's
`encoder_press_action`: when the action is `key`, the desktop sends
`enabled:false` and the firmware treats an encoder press purely as a button
event source — it never emits `button_down`/`button_up` and never starts an
audio session; presses only feed the firmware double-click window, which
emits `button_click` on window expiry or `button_double_click` on a second
press within the window, both with `source:"encoder"` (a long knob hold
therefore produces no events until release). The gate
applies only to `APP_INPUT_SOURCE_ENCODER`: the physical primary button and
`remote_button_*` control events keep their recording semantics regardless,
which is how the desktop's double-click-recording toggle (sent as
`remote_button_down`/`remote_button_up`) still works when the gate is closed.
If the gate is flipped from on to off while an encoder-started recording is
already in progress, the release path is still allowed to stop it normally.

Deprecated app-to-firmware events:

| Event | Replacement | Reason |
| --- | --- | --- |
| `connected` | `ui_state:ready` | Connection is not a display state after pairing. |
| `partial` | `ui_state:thinking` with `text` | Partial text is display content for the thinking state. |
| `final` | `ui_state:pending_confirmation` with `text` | Final text is still cancellable until pasted. |
| `paste_done` | `ui_state:ready` | Once pasted, the device returns to ready. |
| `paste_cancelled` | `ui_state:ready` | Once cancelled, the device returns to ready. |
| `error` | `ui_state:error` with `text` | Errors are another UI state. |

## Power Log Export

The firmware's `power_log` component records power-mode residency and battery
voltage on-device (mode-switch events plus a 60 s periodic VBAT sample). The
log is exported over the existing `control_rx` / `state_tx` JSON channel; no
new GATT characteristic is added. The component is a pure observer: it never
changes power state-machine behavior. See `Doc/Plan/power-mode-energy-profiling.md`
for the design.

Requests (host -> StickS3, `control_rx`):

```json
{"power_log":{"cmd":"dump","offset":0,"max":160}}
{"power_log":{"cmd":"clear"}}
{"power_log":{"cmd":"time_anchor","epoch":1754042700}}
```

| Command | Fields | Meaning |
| --- | --- | --- |
| `dump` | `offset`: byte offset into the logical log stream to start from; `max`: per-fragment raw byte cap (clamped by the ATT MTU, ≤160) | Starts a one-shot streaming session: the device automatically sends `state_tx` fragments at a fixed interval from `offset` until EOF. A new `dump`, `clear`, or disconnect aborts the current session. |
| `clear` | — | Clears the log and bumps the header wrap counter. |
| `time_anchor` | `epoch`: uint32 epoch seconds | Records a time-anchor entry so analysis tooling can map relative uptime to wall clock. |

Responses (StickS3 -> host, `state_tx` JSON state frames, fragmented):

```json
{"power_log":{"seq":0,"offset":0,"total":1234,"eof":0,"data":"<base64>"}}
```

`data` is the base64 encoding of at most 160 raw bytes of the logical stream
(kept small to control BLE MTU pressure). `offset` is the stream position of
this fragment, `total` is the current total log size in bytes, `seq` increments
per fragment within one dump, and `eof:1` marks the final fragment.

The concatenated `data` payloads form the logical log stream, in chronological
order. All multibyte fields are little-endian.

```text
Header (16 bytes):
  uint8_t  magic[4];      // 'P','W','R','L'
  uint8_t  version;       // 1
  uint8_t  entry_size;    // 12
  uint8_t  reserved[2];   // 0
  uint32_t entry_count;   // valid entries following the header
  uint32_t wrap_count;    // incremented on clear and on ring wrap

Entry (12 bytes, packed; entry_count of them):
  uint32_t uptime_s;      // esp_timer uptime in seconds (relative time)
  uint16_t vbat_mv;       // battery voltage at record time
  uint8_t  mode;          // power_mode_t; 0xFF for time-anchor entries
  uint8_t  flags;         // see below
  uint8_t  reserved[4];   // time-anchor entries store uint32 epoch seconds here
```

`mode` values: `0` = S0 active (bright screen), `1` = S1 resting (dimmed),
`2` = S2 screen off, `3` = S3 power off, `4` = recording, `5` = advertising,
`6` = OTA, `0xFF` = time anchor.

`flags` bits: bit0 = charging, bit1 = USB powered, bit2 = periodic sample (not
a mode-switch event), bit3 = power-off segment recovery record, bit4 = time
anchor (with `mode` = `0xFF` and the epoch seconds in `reserved[0..3]`).

## BLE OTA

The firmware uses a custom OTA channel over the same Voice Stick service. The macOS app writes OTA `begin` and `end` frames with BLE write-with-response, and streams OTA `data` frames with write-without-response using CoreBluetooth flow control.
The device sends progress notifications roughly every 32 KB of accepted firmware data.

The macOS app starts OTA for one connected device at a time. It discovers updates from the latest firmware manifest, downloads the manifest `ota_url`, verifies byte size and SHA-256, then sends the verified app-slot image over BLE. The browser flasher uses the manifest `merged_url` instead because USB flashing writes a merged image at offset `0x0`.

The 8 MB flash layout uses two 3 MB OTA app slots and keeps the remaining flash as a reserved SPIFFS data partition:

| Name | Offset | Size |
| --- | ---: | ---: |
| `ota_0` | `0x10000` | 3 MB |
| `ota_1` | `0x310000` | 3 MB |
| `storage` | `0x610000` | 1984 KB |

All multibyte fields are little-endian.

```text
struct OtaBeginFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x20 begin
  uint16_t header_len;    // 12
  uint32_t image_size;
  uint32_t transfer_id;
}

struct OtaDataFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x21 data
  uint16_t header_len;    // 12
  uint32_t transfer_id;
  uint32_t offset;
  uint8_t  payload[];
}

struct OtaEndFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x22 end
  uint16_t header_len;    // 12
  uint32_t transfer_id;
  uint32_t image_size;
}

struct OtaAbortFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x23 abort
  uint16_t header_len;    // 8
  uint32_t transfer_id;
}
```

`ota_tx` sends a state frame:

```text
struct OtaStateFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x30 OTA state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

OTA state events include:

```json
{"event":"ready","transfer_id":1,"size":1385760,"partition":"ota_1"}
{"event":"progress","transfer_id":1,"written":32768,"size":1385760}
{"event":"done","transfer_id":1,"reboot_ms":500}
{"event":"error","code":"bad_offset","esp_err":258}
{"event":"aborted"}
```

On the device display, OTA switches the normal idle/recording UI into an update state:

- `Updating` with percentage while the image is being written.
- `Rebooting` after the new boot partition is selected.

While OTA is active, the device ignores push-to-talk input and pauses display dimming/deep sleep timers. After a successful transfer, the firmware waits about 500 ms after sending the `done` event and then calls `esp_restart()`.
The desktop updater can cancel an in-progress transfer by sending `OtaAbortFrame`; the device aborts the OTA handle and keeps booting the current firmware.

## Runtime State Machine

StickS3:

```text
boot -> advertising -> connected -> idle -> recording -> idle
```

The firmware dims the display after 10 seconds of idle time, turns the screen off after 20 seconds, and enters deep sleep after 5 minutes on battery power; while charging or USB powered it stays at the screen-off stage. The front button wakes the device from deep sleep. Picking up the device (detected via BMI270) wakes the display from the dimmed/screen-off states back to active.

macOS:

```text
needs_pairing -> scanning -> ready -> recording -> thinking -> pending_confirmation -> ready
```

During recognition and confirmation, the firmware keeps showing the thinking cat
until the app sends `ui_state:ready`. During pending confirmation, `primary`
confirms or pauses according to the app's internal countdown mode, and
`secondary` cancels. When idle, `secondary` restores the last recoverable input
confirmation. These meanings are app state-machine behavior, not firmware
protocol events.

Recordings shorter than 0.5 seconds are discarded locally and are not sent to ASR.

## ASR Transport

The desktop app can connect either directly to Volcengine or to VoiceStick Cloud. Both providers use the same WebSocket binary framing in the client, so request, audio, response, and error handling are shared.

Volcengine endpoint:

```text
wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
```

VoiceStick Cloud default endpoint:

```text
wss://api.xiaozhi.me/voicestick/asr/
```

The first request payload currently sent by the desktop app is:

```json
{
  "user": {"uid": "voice-stick-local"},
  "audio": {
    "format": "ogg",
    "codec": "opus",
    "rate": 16000,
    "bits": 16,
    "channel": 1
  },
  "request": {
    "model_name": "bigmodel",
    "enable_nonstream": true,
    "show_utterances": false,
    "enable_ddc": true
  }
}
```

The desktop app buffers Ogg chunks until the recording reaches 0.5 seconds, then starts ASR and flushes the buffered chunks. On button release, it sends the final Ogg chunk with the WebSocket last-packet flag and waits for the final response.

VoiceStick Cloud business errors should use the same error frame shape as Volcengine: message type `0x0f`, a four-byte big-endian error code, a four-byte big-endian message size, and a UTF-8 message. For quota or billing errors, the message should be JSON so the desktop app can surface an upgrade action:

```json
{
  "error": "quota_exceeded",
  "message": "Daily free quota has been used up.",
  "upgrade_url": "https://voicestick.app/account/billing"
}
```

See `docs/volcengine-asr.md` for the trimmed Volcengine API notes used by the desktop app.
