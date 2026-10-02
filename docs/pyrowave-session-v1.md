---
type: reference
title: Apollo experimental Pyrowave session contract
date: 2026-10-02
version: 1
---

## Overview

Version 1 replaces only video encoding in an authenticated Apollo session with
Pyrowave pinned to `89f7e47d4abbf650c91fae766728af866c5e32a0`. Output is
negotiated 1920x1080 or 3840x2160 at nominal 60 fps, SDR 4:2:0. Audio, input, control, video RTP, FEC,
encryption and ping association use the existing session paths. This is an
opt-in fork experiment; a working host build does not establish live Apple TV
playback. Separate hardware cursors are omitted. DMA-BUF reservation-fence
export/wait/copy/publication is non-atomic and does not establish a compositor
lease. Imported compositor images are only read; encoding reads an owned copy.

## Negotiation

| Field | Exact value / behavior |
| --- | --- |
| Build | `APOLLO_ENABLE_PYROWAVE=ON`, default OFF; Linux KMS/libdrm/libcap only. Supply `APOLLO_PYROWAVE_SOURCE` and `APOLLO_PYROWAVE_LIBRARY` from the pin; no automatic download. |
| Runtime | `experimental_pyrowave = enabled` and `capture = kms`; default disabled. Existing display selection applies. |
| HTTP `/serverinfo` | When build/runtime/KMS enabled, additional XML tags `ApolloPyrowaveVersion` = `1`, `ApolloPyrowavePin` = the exact pin. Conventional `ServerCodecModeSupport` bits are unchanged. |
| RTSP DESCRIBE | When enabled, `a=x-apollo-pyrowave-version:1` and `a=x-apollo-pyrowave-pin:89f7e47d4abbf650c91fae766728af866c5e32a0`. Advertisement indicates opt-in availability, not device readiness. |
| RTSP ANNOUNCE selection | Require `a=x-nv-vqos[0].bitStreamFormat:3` plus both exact advertised `x-apollo-pyrowave-version` and `x-apollo-pyrowave-pin` attributes. No fallback after selection. Missing/mismatched/disabled capability or capability with a conventional format returns 400. |
| Video parameters | `clientViewportWd` / `clientViewportHt` must be exactly `1920` / `1080` or `3840` / `2160`. Both require `maxFPS:60`, `videoEncoderSlicesPerFrame:1`, `encoderCscMode:3`, `dynamicRangeMode:0`, `x-ss-video[0].chromaSamplingType:0`, `x-ss-video[0].intraRefresh:0`. Existing display rate must yield `encodingFramerate=60000`; no fractional/warp/input-only mode. HDR requests return 400. |
| Capture geometry | Primary framebuffer must be uncropped 16:9, at least 1920x1080 and at most 3840x2160. Output scales to the negotiated dimensions; encoder/scaler allocations use that size. Startup logs source/output dimensions and whether scaled. A 2560x1440 desktop streamed at 3840x2160 is **scaled 4K**, not native 4K capture. Unsupported SDR format/modifier/layout or a mid-session geometry/format change fails closed. |
| Transport parameters | Existing `packetSize` = 1024..1392; host `fec_percentage` = 1..80; `minRequiredFecPackets` = 0..2; adjusted video bitrate = 10000..200000 Kbps. Requested/configured bitrate must also be <=200000 Kbps. Numeric session attributes must be complete nonnegative decimal integers fitting signed 32 bits. Standard encryption negotiation still applies. |
| Session exclusion | One Pyrowave session exclusively owns capture. Reject Pyrowave while any other capture session holds ownership, and reject conventional capture while Pyrowave holds ownership. Audio/input-only sessions also conservatively count. |
| Startup | First KMS selection, same-GPU import, geometry/SDR validation, snapshot and bounded encode run synchronously before ANNOUNCE succeeds. Recoverable failure returns 500 with a plain-text reason after GPU cleanup completes and capture ownership is released. A watchdog covers the entire initialization and constructor-unwind cleanup: if it has not finished in 10 seconds, Apollo fails the process using its existing fatal-hang policy. This closes client connections instead of promising a 500; restart requires a supervisor or the operator. Unsupported parameters return 400 with a reason. |

## Envelope

One complete Apollo video frame contains the following envelope, after the
existing 8-byte Sunshine short frame header has been removed. All envelope
integers and packet lengths are unsigned **big endian**. Pyrowave packet bytes
retain their codec-defined encoding. There is no native struct serialization.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `PWR1` |
| 4 | 2 | Envelope version = 1 |
| 6 | 2 | Fixed header size = 32 |
| 8 | 4 | Apollo frame number, starts at 1 on each connection; matches RTP/NV frame index |
| 12 | 4 | Total envelope byte count, including the header and every length record |
| 16 | 2 | Negotiated output width: 1920 or 3840 |
| 18 | 2 | Negotiated output height: 1080 or 2160, respectively |
| 20 | 2 | Packet count, 1..1024 |
| 22 | 2 | Flags = 1 (independent full frame); all other bits reserved zero |
| 24 | 4 | Color profile = 1 |
| 28 | 4 | Reserved = 0 |
| 32 onward | variable | Exactly `packet_count` records: `u32 length`, then `length` bytes of one Pyrowave packet; no alignment or trailing bytes. Each length is 1..1200. |

Color profile 1 is sRGB nonlinear SDR input/output, BT.709 YCbCr coefficients,
full range, center-sited 4:2:0, chroma midpoint `128/255`. Scale with linear
filtering, no dithering; the encoder may use R16 intermediates for packed 10-bit
SDR input. Metal rendering must use this full-range transform, replacing the
probe's limited-range fixture transform. It is not an HDR profile.
Recover nonlinear RGB with full-range BT.709 YCbCr coefficients and apply the
sRGB EOTF before writing linear RGB to an sRGB render target; replace the
probe's BT.709 transfer function as well as its limited-range offsets/scales.
The outer color profile is authoritative over the codec header's default fields.

## Limits and recovery

| Rule | Client / host behavior |
| --- | --- |
| Absolute frame cap | 1 MiB including envelope; actual negotiated frame cap is smaller. Validate total size/count/records before pushing packets into the decoder. |
| Transport budget | For packet size `S`, each data shard holds `S-16` frame bytes; the 8-byte short header counts. Wire shard size is `S+16`, plus 32 bytes when video encryption is on. At most four FEC blocks; each block's data plus `max(ceil(data*fec/100), minRequiredFecPackets)` parity must be <=255. Host matches the broadcaster's aligned splitting and never uses its oversized-frame FEC-disable fallback for Pyrowave. |
| Bandwidth | Each frame's padded RTP/FEC/encryption wire bytes must fit `floor(adjusted_video_kbps*1000/8/60)` before queueing. Apollo's existing audio/control adjustment precedes this check. UDP/IP/link overhead is covered by the existing transport reserve, not added to the frame envelope. The same caps apply at 1080p and 4K: 4K gets no automatic bitrate increase. At a fixed budget, four times as many output pixels means fewer encoded bits per pixel and potentially lower detail/quality. Complex frames can exceed rate-control targets and are dropped; 4K60 quality/performance within this budget requires device validation. The 200000 Kbps ceiling stays below a Gigabit link budget, without claiming measured throughput. |
| Buffering | At most two frames pending in the host broadcaster, plus one owned snapshot/encode operation. A full queue skips a capture interval. Every frame is independent; oversized later frames are dropped. Shutdown removes queued frames and waits for an in-flight broadcast before releasing session pointers. |
| Decode unit | Concatenate all `LENTRY` buffers of one `DECODE_UNIT` in order, respecting lengths. RTP/FEC fragments do not preserve Pyrowave packet boundaries; only envelope length records do. Reject a malformed/truncated unit as a whole. |
| Independent frames | Host marks **every** frame as IDR (`frameType=2` in the short header). Clear Pyrowave decoder state before each complete envelope, push all records in order, require whole-frame readiness, then decode. Loss of a frame needs no previous/reference frame; accept the next complete independent frame. Existing IDR requests are harmless. |
| Reconnect | Drop all pending decode work, drain Metal command buffers, recreate/clear decoder state and reset frame-number tracking. Allocate decoder and luma/chroma textures from the newly negotiated dimensions, not fixed 1080p fixture sizes. Never mix packets from different decode units or connections. Host ends a connection before 32-bit frame-number wrap. |
| Capture limitations | Fail closed on HDR/unknown metadata, multiple noncursor planes, rotation/crops/plane scaling, framebuffer extent/format changes, unknown modifiers, import/fence errors and nonopaque primary-plane alpha. No cursor-composition milestone is included. |

Startup recovery keeps captured FDs, imported/owned GPU resources and the
exclusive capture lease alive until cleanup finishes or the process fails.
It never frees in-flight resources or detaches a retry context. The startup
watchdog has its own joined monitor, independent of the shared task pool;
successful startup and recoverable cleanup cancel and join it. Healthy session
teardown retains Apollo's existing session-join watchdog. After a recoverable
startup failure, conventional or Pyrowave sessions may acquire capture again.
A fatal hang ends the host process and all connections; process restart resets
capture ownership, and clients must reconnect. This changes no wire fields.

## Client integration points

The baseline custom Moonlight client can add one unused `VIDEO_FORMAT` bit,
`VIDEO_FORMAT_PYROWAVE=0x10000`, selected only after matching both DESCRIBE
attributes and explicit user opt-in. This is a client-local format bit;
**3** is the ANNOUNCE wire format ID. Add selection before the AV1/HEVC branches
in `RtspConnection.c`, emit the selection attributes in `SdpGenerator.c`, and
route the new bit in the renderer setup/submit callbacks. Do not advertise it
through a conventional codec bit.

`VideoDepacketizer.c` already treats non-H264/HEVC formats as opaque picture
fragments, uses short-header IDR status, and trims FEC padding with
`lastPayloadLen`. Extend its IDR validation to accept Pyrowave PICDATA without
requiring SPS/PPS/VPS/AV1 configuration. Keep `RtpVideoQueue.c`, decryption,
loss handling and complete-unit assembly. The client renderer must bypass
Annex B/NAL/VideoToolbox paths and pass the envelope's individual packet
records to the existing Metal Pyrowave decoder. Check every fixed field,
reserved bit, length and complete consumption before decoder submission.
Require the envelope's dimension pair to match renderer setup/ANNOUNCE exactly;
reject mismatches instead of resizing mid-session. For 4K 4:2:0, luma is
3840x2160 and each chroma plane is 1920x1080. Keep decoder/output texture sizes
and independent-frame checks consistent across reconnects.

## HDR evolution

Version 1 advertises SDR only and rejects `dynamicRangeMode != 0`. Packed 10-bit
SDR capture and R16 intermediates preserve SDR precision; neither establishes HDR.
The pinned Vulkan scaler already accepts extended linear sRGB (80-nit reference)
and HDR10 ST2084 input/output, uses full-range BT.2020 NCL for PQ output, and
supports R16 UNORM intermediates. Pyrowave is a floating point codec;
`PYROWAVE_PRECISION=1` uses FP32 lifting math with FP16 pyramid storage, rather
than a negotiated fixed 10-bit video format. This makes HDR a possible extension,
not an implemented session mode.

An HDR extension must negotiate a new color profile/version with an explicit
PQ/BT.2020 transfer, range, chroma midpoint and luminance/static metadata contract.
The pinned packetizer leaves sequence-header color fields at their defaults,
so the outer negotiated profile must remain authoritative. Host capture must
identify actual source transfer/primaries and compositor color processing,
preserve HDR metadata, choose R16 intermediates, and validate snapshot/scale
precision instead of bypassing the current fail-closed SDR checks.

The existing Apple probe uses R8 UNORM planes, a limited-range BT.709 shader
and `BGRA8Unorm_sRGB` with an sRGB `CAMetalLayer`; these require changes even
for this session's full-range SDR profile. The Metal decoder API supports R16
UNORM/R32 float output planes. HDR additionally needs a PQ/BT.2020 conversion
and a higher-precision HDR/EDR presentation surface with suitable layer color
space and output metadata, verified on tvOS and the physical display. Current
R8/sRGB presentation must never silently render an HDR envelope as SDR.

First verify a changing desktop through the real 1080p60 Apollo session with
existing audio/input, then prioritize scaled/native 4K60 TV playback and
reconnect. Native 4K capture, physical 4K presentation and HDR remain operator
and device verification gates; host CPU checks do not complete these gates.

## Build

```sh
cmake -S . -B build -DAPOLLO_ENABLE_PYROWAVE=ON \
  -DAPOLLO_PYROWAVE_SOURCE=/path/to/pinned/pyrowave \
  -DAPOLLO_PYROWAVE_LIBRARY=/path/to/libpyrowave-shared.so \
  -DVulkan_INCLUDE_DIR=/path/to/vulkan-headers/include \
  -DVulkan_LIBRARY=/path/to/libvulkan.so
heavy cmake --build build
heavy ctest --test-dir build -R pyrowave-live --output-on-failure
```

The prebuilt library is operator-supplied; CMake verifies clean source HEAD and
records the library hash, which alone cannot attest binary provenance. Live
capture/device access and TV deployment remain operator validation gates.
