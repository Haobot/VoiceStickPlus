# Volcengine ASR Notes

This note keeps only the Volcengine ASR details that matter to Voice Stick.

## Endpoint

Voice Stick uses the optimized bidirectional streaming endpoint:

```text
wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
```

Other endpoints from the Volcengine document:

| Mode | Endpoint | Notes |
| --- | --- | --- |
| Bidirectional streaming | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel` | Older streaming path. |
| Streaming input / nostream result | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream` | Returns after audio is longer than 15s or after the final packet. |
| Optimized bidirectional streaming | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` | Preferred for Voice Stick. Returns only when the result changes. |

## Headers

Voice Stick uses the newer console auth style:

| Header | Value |
| --- | --- |
| `X-Api-Key` | Volcengine API key from the new console. |
| `X-Api-Resource-Id` | Resource ID, for example `volc.bigasr.sauc.duration`. |
| `X-Api-Request-Id` | UUID for the current ASR session. |
| `X-Api-Sequence` | `-1` during WebSocket connection. |

`X-Tt-Logid` from the WebSocket response headers is useful for Volcengine support
and server-side troubleshooting. The current Swift client does not surface it.

## Resource IDs

| Model | Duration billing | Concurrent billing |
| --- | --- | --- |
| Doubao streaming ASR 1.0 | `volc.bigasr.sauc.duration` | `volc.bigasr.sauc.concurrent` |
| Doubao streaming ASR 2.0 | `volc.seedasr.sauc.duration` | `volc.seedasr.sauc.concurrent` |

## First Request Payload

Voice Stick sends Ogg Opus audio, so the first client request should use:

```json
{
  "user": {
    "uid": "voice-stick-local"
  },
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

`enable_nonstream = true` enables the two-pass mode on the optimized
bidirectional streaming endpoint. It gives fast partial text first, then uses
the nostream model to re-recognize completed speech segments for better final
accuracy.

## Audio Packets

Volcengine recommends:

- Audio packet duration: 100-200 ms.
- Packet interval: 100-200 ms.
- For bidirectional streaming, 200 ms packets are recommended for best
  performance.

The StickS3 firmware currently encodes 60 ms Opus packets. The macOS app wraps
those packets into Ogg Opus chunks and forwards them after the local recording
duration reaches 0.5 seconds. On button release, it sends the final Ogg chunk
with the WebSocket binary protocol's last-packet flag.

## Pause And Finalization

`end_window_size` is the forced endpointing pause threshold.

| Parameter | Meaning |
| --- | --- |
| `end_window_size` | Silence duration in milliseconds before the server stops the current speech segment and outputs `definite: true`. |
| Default | `800` ms. |
| Minimum | `200` ms. |

When `enable_nonstream = true`, Volcengine enables VAD segmentation by default.
The default pause threshold is 800 ms, and it can be changed with
`end_window_size`.

If `end_window_size` is set, Volcengine does not use semantic segmentation for
that decision. It segments by silence duration instead, which is better for
low-latency scenarios where `definite: true` should arrive sooner.

Related parameter:

| Parameter | Meaning |
| --- | --- |
| `vad_segment_duration` | Semantic segmentation silence threshold. Default is 3000 ms. It does not decide when `definite: true` appears. It becomes ineffective after `end_window_size` is configured. |

Voice Stick does not expose `end_window_size` in user settings and does not send
it in the ASR request, so the service default is used.

## Response Fields We Use

Important response fields:

| Field | Meaning |
| --- | --- |
| `result.text` | Current recognized text. |
| `result.utterances[].definite` | Whether a segment is finalized. |
| `result.utterances[].start_time` / `end_time` | Segment timestamps in milliseconds. |

Voice Stick shows partial text while ASR is active and keeps the firmware display
in the thinking state after the push-to-talk session ends. Final text enters a
1.2 second confirmation countdown before paste. The primary button can pause and
then confirm that paste; the secondary button cancels in-progress recognition or
pending text.

## Common Errors

| Code | Meaning |
| --- | --- |
| `20000000` | Success. |
| `45000001` | Invalid request parameter, missing required field, invalid field value, or duplicate request. |
| `45000002` | Empty audio. |
| `45000081` | Timed out while waiting for packets. |
| `45000151` | Invalid audio format. |
| `55000031` | Server busy. |
