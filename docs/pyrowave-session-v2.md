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
| Availability | Build `APOLLO_ENABLE_PYROWAVE=ON`; runtime `experimental_pyrowave = enabled`, `capture = kms`. Defaults remain disabled. Advertisement does not establish device readiness. |
| HTTP `/serverinfo` | `ApolloPyrowaveVersion=2`, `ApolloPyrowavePin` equals the exact pin. Conventional codec capability bits remain unchanged. |
| RTSP DESCRIBE / ANNOUNCE | `a=x-apollo-pyrowave-version:2` and `a=x-apollo-pyrowave-pin:5e4a98f807dddd2498824e3b55ef2fe1845bcc59`. ANNOUNCE requires both exact values with `a=x-nv-vqos[0].bitStreamFormat:3`; reject missing/mismatched/v1/disabled selection with 400, without fallback. |
| Client format | Client-local `VIDEO_FORMAT_PYROWAVE=0x10000`; ANNOUNCE wire format `3`. Select only with explicit opt-in and matching version/pin. |
| Output | Exactly 1920x1080, 2560x1440 (1440p / requested "2K") or 3840x2160, 60 fps, `encodingFramerate=60000`, SDR full BT.709 4:2:0: CSC 3, chroma 0, dynamic range 0, one slice, no intra refresh or input-only mode. Host and client must agree on the exact dimensions. |
| Capture / scaling | Uncropped 16:9 primary framebuffer, 1920x1080 through 3840x2160. Scale to negotiated output; log source/output geometry. A 2560x1440 source at 2560x1440 output is native 1440p; at 3840x2160 output it is scaled 4K. Unknown SDR metadata, HDR, alpha, format/geometry changes, multiple noncursor planes, crops/rotation/plane scaling, modifier/import/fence errors fail closed. |

## Optional Linux session display policy

An application may explicitly require Pyrowave and give the session ownership of
display preparation and recovery. Add this object to that app in `apps.json`:

```json
{
  "name": "Pyrowave Desktop",
  "exclude-global-prep-cmd": true,
  "exclude-global-state-cmd": true,
  "allow-client-commands": false,
  "terminate-on-pause": false,
  "session-display": {
    "codec": "pyrowave",
    "prepare": "your-display-prepare-command",
    "recover": "your-conditional-display-recovery-command",
    "timeout-ms": 30000
  }
}
```

Both commands are required trusted shell commands. `timeout-ms` defaults to 30000
and accepts integers from 100 to 60000. Helpers must remain in their process group
and must finish all display work before exiting; they must not daemonize or use
`setsid`. Apollo terminates and reaps remaining group members, including background
descendants, before recovery or reconnect. Group settlement adds at most two
seconds to each command's bound. Commands receive a copied app environment and
working directory. Resume and same-app launch preserve app policy and create a
new immutable snapshot with the requesting client's dimensions and identity.

These entries must omit app prep/state commands and virtual displays, exclude
both global hook lists, and keep client commands and terminate-on-pause disabled.
Put any display undo exclusively in `recover`; other app entries retain their
ordinary launch/probe behavior. A detached application may still use its
`detached` launcher: disconnect restores displays without terminating the game.
Apollo cannot observe the exit of an untracked detached game.

HTTP launch/resume skips conventional encoder probing for these explicitly
Pyrowave-only entries. A conventional ANNOUNCE fails visibly; existing v2 codec,
output and bitrate checks still apply. Rejected or abandoned handshakes never run
display commands. After valid negotiation the exclusive capture gate is acquired,
rollback is armed, preparation completes, and only then does the factory capture.
Failures settle partial transport and capture before recovery. Normal stop joins
video, drains broadcaster packets, destroys capture/GPU resources, and joins the
remaining transport before synchronous recovery and gate release. App exit,
cancel and direct termination cannot restore ahead of that session cleanup.

Recovery failure retains the helper's recovery state and blocks further capture
and legacy display commands until host restart and successful external recovery.
A helper group that cannot settle also blocks further work; Apollo withholds
recovery while a late prepare could still mutate displays. GPU cleanup keeps its
existing ten-second fatal watchdog, separate from display helper timeouts. Fatal
host termination needs external service-start recovery; configure that with the
same conditional helper before starting Apollo. App hooks cannot replace it.

CPU verification is registered as CTest `pyrowave-session-display` when
`BUILD_TESTS=ON` on Linux. It injects command, capture and transport I/O into the
production session startup/stop/join paths; no display or GPU access is required.

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
