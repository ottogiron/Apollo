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

The current codec pin is a local reviewed revision based on
`89f7e47d4abbf650c91fae766728af866c5e32a0`, not fetchable from upstream. The
[tracked preparation and build recipe](./building.md#experimental-pyrowave-linux-host)
reconstructs its exact source identity from that public base and the reviewed patch.
Use the matching codec revision on the client.
It fixes the missing stride-256 rate-control scan stage and updates the embedded
Vulkan/Metal encoder shaders. At matching budgets, the reviewed synthetic images
decoded identically before and after this fix; improved game quality is unproven.
The selected/requested bitrate and total video UDP-payload ceiling are now
500000 Kbps for bounded higher-bitrate comparisons. Output modes, wire v2,
codec pin and transport accounting remain unchanged.

## Negotiation

| Element | Contract |
| --- | --- |
| Codec pin | `5e4a98f807dddd2498824e3b55ef2fe1845bcc59`, local reviewed scan fix; supply its clean source checkout and corresponding shared library. |
| Availability | Build `APOLLO_ENABLE_PYROWAVE=ON`; runtime `experimental_pyrowave = enabled`; private `pyrowave_capture_source = wayland` (default). Conventional `capture = kms` stays independent. The experiment remains disabled by default. Advertisement does not establish device readiness. |
| HTTP `/serverinfo` | `ApolloPyrowaveVersion=2`, `ApolloPyrowavePin` equals the exact pin. Conventional codec capability bits remain unchanged. |
| RTSP DESCRIBE / ANNOUNCE | `a=x-apollo-pyrowave-version:2` and `a=x-apollo-pyrowave-pin:5e4a98f807dddd2498824e3b55ef2fe1845bcc59`. ANNOUNCE requires both exact values with `a=x-nv-vqos[0].bitStreamFormat:3`; reject missing/mismatched/v1/disabled selection with 400, without fallback. |
| Client format | Client-local `VIDEO_FORMAT_PYROWAVE=0x10000`; ANNOUNCE wire format `3`. Select only with explicit opt-in and matching version/pin. |
| Output | Exactly 1920x1080, 2560x1440 (1440p / requested "2K") or 3840x2160, 60 fps, `encodingFramerate=60000`, SDR full BT.709 4:2:0: CSC 3, chroma 0, dynamic range 0, one slice, no intra refresh or input-only mode. Host and client must agree on the exact dimensions. |
| Capture / scaling | Wayland: entire untransformed native 1920x1080 or 2560x1440 output, cursor included, opaque SDR sRGB RGB pixels. Fractional logical geometry maps input only. Existing negotiated stream sizes remain unchanged: 1440p source at 4K stream size is scaled 4K, not native4K acceptance. Explicit `kms-diagnostic` retains earlier KMS limits and producer-race/cursor omissions. Unsupported layouts, nonopaque alpha, geometry changes, transform/y-invert, protocol/device/import/copy/encode failures end the session without fallback. |

### Private Wayland source

`pyrowave_output_name` is a connector name; empty requires the sole active output
after preparation. Multiple outputs require an explicit name. Global `capture =
kms` and conventional `output_name = 0` stay independent. A per-app conventional
display override must match the selected connector, otherwise startup refuses it.
This capture slice does not invoke display helpers; prepare/restore transactions
are integrated separately. Logical input includes fractional scale and desktop
offsets, without changing conventional input coordinates.

Requires screencopy v3, linux-dmabuf v4 device/modifier feedback, xdg-output v3 and
advertised native explicit sync (`wp_linux_drm_syncobj_manager_v1`). Apollo resolves
the feedback GPU to its render node and requires Vulkan on the same device. Every
GBM plane is exported; only represented single-plane packed RGB is accepted after
actual-FD memory-type intersection and dedicated Vulkan import validation. Actual
modifiers, strides and offsets are retained; there is no implicit layout guess.

One client-owned GBM destination is handed off by screencopy `ready`. GBM/FDs remain
alive through Apollo's snapshot copy fence; the snapshot remains alive through
encode completion. Only then can the destination be requested again. FOREIGN
queue-family transitions are preserved. One-second protocol/GPU waits fail closed;
frame/async-params proxies are cancelled before listener data is destroyed. Startup
cleanup remains inside the ten-second watchdog, with GPU resources retained until
idle or the existing process-fatal policy. No SHM, KMS or codec fallback is taken.

At [Hyprland 0.56.2](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/managers/screenshare/ScreenshareFrame.cpp),
non-color-management-aware screencopy renders back to sRGB. Alpha is checked on
the GPU before encode. HDR/other color contracts and native4K remain deferred.
The [repo screencopy ready contract](../third-party/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml)
and pinned [Screencopy.cpp](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/protocols/Screencopy.cpp)/[GLRenderer.cpp](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/GLRenderer.cpp)
define normal completion. [ProtocolManager.cpp](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/managers/ProtocolManager.cpp)
and [OpenGL.cpp](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/OpenGL.cpp)
connect sync advertisement to native EGL fences. Exceptional compositor false-ready
or fence-export failure cannot be detected reliably by these client protocols.
Reservation fences do not prove readiness for a specific capture. Destination
ownership and the protocol handoff replace that bridge, without mandatory CPU
readback. CPU tests establish no latency, quality or physical acceptance.

Permission/timeout failures explain the compositor permission prerequisite for the
exact resolved versioned Apollo executable. Permission installation and live
acceptance remain operator actions. `pyrowave_capture_source = kms-diagnostic`
explicitly retains the diagnostic KMS path and logs its synchronization/cursor
limitations.

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
| Transport | Packet size `1024..1392`, FEC `1..80%`, minimum parity `0..2`, video wire budget `10000..500000 Kbps`. Requested/configured and fallback maximum bitrate must also stay <=500000 before host caps or reservations; both numeric validation and transport use the shared `pyrowave::max_bitrate_kbps`. Complete nonnegative decimal session integers must fit signed 32 bits; malformed or overflowing values fail closed. |
| RTSP budget | Select configured bitrate (fall back to maximum bitrate), apply the host ceiling and existing framerate handling, then reserve audio (`256` Kbps/channel HQ, `96` normal, capped at 20%) and another `500` Kbps for control/overhead (capped at 10% of the remainder). Pyrowave passes this total video wire budget to frame cost without the conventional encoder's FEC discount. FEC, RTP and encryption are charged once by actual frame cost. Accepted Pyrowave sessions still require both requested 60 FPS and encoding 60000; warp/fractional modes remain rejected. Conventional codec bitrate arithmetic is unchanged. |
| Frame budget | Padded RTP/FEC/encryption wire bytes <= `floor(video_wire_kbps*1000/8/60)`. Each data shard holds `packetSize-16` frame bytes; include the eight-byte short header. Wire shard size is `packetSize+16`, plus 32 for encryption. These are UDP payload bytes; UDP/IP/link headers are outside the cost model. Match broadcaster alignment, at most four RS blocks, each data + parity <=255; no oversized-frame FEC-disable fallback. Retain the conservative frame cap, and check every actual frame with `cost().fits` because parity rounding is nonmonotonic. Same budget at 1080p, 1440p and 4K. |
| Startup / cleanup | First capture/import/snapshot/GPU alpha validation/encode/envelope finishes before ANNOUNCE succeeds. Recoverable failure returns 500 after cleanup and capture-lease release. A joined ten-second watchdog covers initialization and constructor-unwind GPU cleanup; a hang terminates Apollo under the existing fatal policy. Preserve captured FDs and GPU resources until cleanup finishes or the process exits. |
| Lifetime / buffering | Exclusive capture ownership; at most two pending broadcaster frames plus one snapshot/encode operation. Drop oversized later independent frames. Shutdown discards queued frames and waits for in-flight broadcast tickets before releasing session pointers. |
| Host counters | About every five seconds during capture, log interval frames emitted, capture/encode attempts, budget/backpressure drops, capture+encode mean/max milliseconds (including import/snapshot/packing), PWR2 payload bytes and Mbps. After broadcast tickets drain, log whole-session totals. Startup validation is excluded. Emission is counted after all frame sends succeed locally, rather than at queue insertion; payload includes the envelope but excludes RTP/FEC/encryption. Host emission FPS is measured separately from requested 60 FPS; it does not establish client delivery or TV presentation. Reports may wait for a synchronous capture/encode operation to finish. Startup logs state source/output geometry, wire budget and codec target. |
| Client decode | Reassemble the whole decode unit, trim FEC padding, validate all fields/lengths/caps and exact consumption, clear decoder state, push complete records in order, then require whole-frame readiness. Every frame uses short-header IDR status. Never split a codec block or pass RTP fragments to the decoder. |
| Reconnect / color | Drain pending GPU/decode work, reset frame tracking, allocate from newly negotiated dimensions; stop before frame-number wrap. Color profile 1 is authoritative over default codec color fields. Convert full-range BT.709 YCbCr to nonlinear RGB, apply sRGB EOTF for an sRGB render target. HDR remains unsupported. |

## Validation

### Selected bitrate versus codec payload

The coordinated client can select 200/250/300/400/500 Mbps for a
2560x1440/60 SDR image-quality comparison. The existing 150 Mbps setting is
unchanged. Selection is a ceiling for the whole stream budget, not the codec
payload rate: `max_bitrate` can lower the request, audio/control are reserved,
and padded RTP, FEC and optional encryption consume video UDP-payload bytes.
UDP/IP/link headers are outside this cost model. No adaptive bitrate, new output
mode, or quality/throughput guarantee follows from the higher ceiling.

With no host cap, stereo HQ reserves 512 Kbps audio and 500 Kbps control.
For FEC30, MTU1392, minimum parity 0 (also identical for 1/2 here), the
production budgets and targets are:

| Selected Mbps | Video wire Kbps | Codec target bytes/frame, plain | Codec target bytes/frame, encrypted | UDP bytes/frame at cap, plain | UDP bytes/frame at cap, encrypted |
| --- | --- | --- | --- | --- | --- |
| 150 | 148988 | **228408** | 222904 | 309760 | 309600 |
| 200 | 198988 | 306840 | 298584 | 413952 | 411840 |
| 250 | 248988 | 383896 | 375640 | 518144 | 518400 |
| 300 | 298988 | 463704 | 452696 | 622336 | 622080 |
| 400 | 398988 | 617816 | 604056 | 829312 | 829440 |
| 500 | 498988 | 776056 | 756792 | 1039104 | 1036800 |

Each envelope cap is the corresponding codec target plus 4128 bytes: the
32-byte v2 header and a conservative reservation for 1024 four-byte record
lengths. The encoder target is not an actual-size guarantee. Every actual
envelope must fit the scalar cap **and** pass its own wire-cost check before
queueing and again before broadcasting.

Bandwidth and conservative packet/FEC rounding bind in all these table rows;
the 1 MiB complete-frame and four-block RS limits do not bind first. The table
uses one to three RS blocks, each with at most 255 data + parity shards. The
conservative search stops at the first rejected complete-shard endpoint; it
does not maximize later endpoints. Around block splits a larger frame can cost
less than a smaller one because parity rounds differently. At MTU1024/FEC11,
692488 envelope bytes cost 765 wire shards, while 692489 bytes cost 764. A
scalar cap alone cannot guarantee that every smaller frame fits. None of these
bounds, the 64 KiB record cap, or the bounded queue is relaxed at higher rates.

### CPU and mock coverage

With `BUILD_TESTS=ON`, `pyrowave-wayland-cpu-protocol` runs an isolated fake
compositor through the production adapter/listeners/polling, with DRM/GBM and
Vulkan calls mocked. It checks versions, device/modifier feedback, plane export,
actual FD imports, cursor, native/logical geometry, ambiguity/overrides, timeout,
cancellation, late events, disconnect/output removal, teardown and reconnect.
Production startup tests also exercise owned-copy FOREIGN transitions, bounded
Vulkan failure cleanup, opaque-alpha and session input mapping.


Build through `heavy cmake --build build` and run
`heavy ctest --test-dir build -R '^pyrowave-live-' --output-on-failure`.
The factory regression injects a complete block larger than 1200 bytes through
production startup packetization, and checks record-cap, frame-budget and raw
metadata rejection at all three output sizes, including native 1440p startup.
Multi-record 1440p factory cases exercise the real encoder target and actual
payload rejection at every selected rate, with device/codec calls mocked.
RTSP budget regressions cover the production strict numeric guard (500000
accepted; 500001, malformed and overflowing values rejected before host caps),
configured/fallback requests, host caps, audio/control reserves and unchanged
conventional arithmetic. All six selected rates are checked with and without
video encryption, including the unchanged 228408-byte 150 Mbps codec target. A separate
broadcaster size simulation cross-checks partial shards and FEC alignment
boundaries at MTU 1024/1200/1392, every FEC percentage 1..80, plain/encrypted and
minimum parity 0/2. Independent GF(256) capacity counting checks the four-block
and 255-shard bounds. A frozen three-to-four-block case checks actual envelope
rejection on the more expensive side of a nonmonotonic boundary.
These CPU checks do not establish
physical capture throughput or TV playback. The higher-bitrate milestone is
limited to a coordinated native 1440p60 SDR comparison, with audio/input and
reconnect using a matching v2 client; 4K/HDR development and cursor composition
are outside this milestone.
