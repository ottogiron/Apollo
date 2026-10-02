---
type: reference
title: Apollo Pyrowave v2 preserves complete codec records
date: 2026-10-03
version: 2
---

## Overview

This is the current coordinated host/client contract. Version 2 replaces
[v1](./pyrowave-session-v1.md), whose 1200-byte codec-record limit rejected
valid live encodes during RTSP ANNOUNCE. The pinned codec uses its packet size
as a packing target and emits larger blocks intact. Each envelope record
contains complete codec blocks; existing Apollo RTP/FEC fragments the entire
envelope independently of these records. Host and client must both select v2.
Audio, input, control, encryption and ping association use existing session paths.

## Negotiation

| Element | Contract |
| --- | --- |
| Codec pin | `89f7e47d4abbf650c91fae766728af866c5e32a0`, unchanged; supply its clean source checkout and corresponding shared library. |
| Availability | Build `APOLLO_ENABLE_PYROWAVE=ON`; runtime `experimental_pyrowave = enabled`, `capture = kms`. Defaults remain disabled. Advertisement does not establish device readiness. |
| HTTP `/serverinfo` | `ApolloPyrowaveVersion=2`, `ApolloPyrowavePin` equals the exact pin. Conventional codec capability bits remain unchanged. |
| RTSP DESCRIBE / ANNOUNCE | `a=x-apollo-pyrowave-version:2` and `a=x-apollo-pyrowave-pin:89f7e47d4abbf650c91fae766728af866c5e32a0`. ANNOUNCE requires both exact values with `a=x-nv-vqos[0].bitStreamFormat:3`; reject missing/mismatched/v1/disabled selection with 400, without fallback. |
| Client format | Client-local `VIDEO_FORMAT_PYROWAVE=0x10000`; ANNOUNCE wire format `3`. Select only with explicit opt-in and matching version/pin. |
| Output | Exactly 1920x1080 or 3840x2160, 60 fps, `encodingFramerate=60000`, SDR full BT.709 4:2:0: CSC 3, chroma 0, dynamic range 0, one slice, no intra refresh or input-only mode. |
| Capture / scaling | Uncropped 16:9 primary framebuffer, 1920x1080 through 3840x2160. Scale to negotiated output; log source/output geometry. A 2560x1440 source at 3840x2160 output is scaled 4K. Unknown SDR metadata, HDR, alpha, format/geometry changes, multiple noncursor planes, crops/rotation/plane scaling, modifier/import/fence errors fail closed. |

## Envelope

One complete video decode unit holds the envelope after the existing eight-byte
Sunshine short header is removed. Envelope integers are unsigned big endian;
codec bytes retain their pinned encoding. No native struct serialization,
record splitting, alignment or trailing bytes are permitted.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `PWR2` |
| 4 | 2 | Version `2` |
| 6 | 2 | Header size `32` |
| 8 | 4 | Nonzero Apollo frame number, starting at 1 per connection, matching RTP/NV frame index |
| 12 | 4 | Total envelope size, including every record length |
| 16 / 18 | 2 each | Negotiated width / height, matching renderer setup exactly |
| 20 | 2 | Record count `1..1024` |
| 22 | 2 | Flags `1`: independent full frame |
| 24 | 4 | Color profile `1`: nonlinear sRGB SDR, full-range BT.709 YCbCr, centered 4:2:0, midpoint `128/255`; linear filtering, no dithering |
| 28 | 4 | Reserved `0` |
| 32 onward | Variable | Exactly the declared records, each `u32 length` then `length` codec bytes; length `1..65536` (64 KiB) |

## Bounds and recovery

| Element | Contract |
| --- | --- |
| Absolute cap | 1 MiB including envelope. The actual frame must also fit the smaller negotiated transport/bandwidth cap. The 64 KiB record maximum is **not** a minimum frame budget. |
| Packing target | Host uses `min(65536, available_frame_bytes - 32 - 4)` for both codec count and packetization. Blocks remain complete. Validate exact mapped metadata extent, every raw offset/length, actual codec byte sum, count and contiguous complete packetizer output before queueing. Rate control is a target; oversized actual frames fail closed. |
| Transport | Packet size `1024..1392`, FEC `1..80%`, minimum parity `0..2`, adjusted video bitrate `10000..200000 Kbps`. Requested/configured bitrate also stays <=200000. Complete nonnegative decimal session integers must fit signed 32 bits. |
| Frame budget | Padded RTP/FEC/encryption wire bytes <= `floor(adjusted_video_kbps*1000/8/60)`. Each data shard holds `packetSize-16` frame bytes; include the eight-byte short header. Wire shard size is `packetSize+16`, plus 32 for encryption. Match broadcaster alignment, at most four RS blocks, each data + parity <=255; no oversized-frame FEC-disable fallback. Same budget at 1080p and 4K. |
| Startup / cleanup | First capture/import/snapshot/GPU alpha validation/encode/envelope finishes before ANNOUNCE succeeds. Recoverable failure returns 500 after cleanup and capture-lease release. A joined ten-second watchdog covers initialization and constructor-unwind GPU cleanup; a hang terminates Apollo under the existing fatal policy. Preserve captured FDs and GPU resources until cleanup finishes or the process exits. |
| Lifetime / buffering | Exclusive capture ownership; at most two pending broadcaster frames plus one snapshot/encode operation. Drop oversized later independent frames. Shutdown discards queued frames and waits for in-flight broadcast tickets before releasing session pointers. |
| Client decode | Reassemble the whole decode unit, trim FEC padding, validate all fields/lengths/caps and exact consumption, clear decoder state, push complete records in order, then require whole-frame readiness. Every frame uses short-header IDR status. Never split a codec block or pass RTP fragments to the decoder. |
| Reconnect / color | Drain pending GPU/decode work, reset frame tracking, allocate from newly negotiated dimensions; stop before frame-number wrap. Color profile 1 is authoritative over default codec color fields. Convert full-range BT.709 YCbCr to nonlinear RGB, apply sRGB EOTF for an sRGB render target. HDR remains unsupported. |

## Validation

Build through `heavy cmake --build build` and run
`heavy ctest --test-dir build -R '^pyrowave-live-' --output-on-failure`.
The factory regression injects a complete block larger than 1200 bytes through
production startup packetization, and checks record-cap, frame-budget and raw
metadata rejection at both output sizes. These CPU checks do not establish
physical capture throughput or TV playback. Verify a real 1080p connection
first, then scaled/native 4K60, audio/input and reconnect with a matching v2
client; HDR and cursor composition are outside this contract.
