# Linux KMS / Pyrowave diagnostic

This opt-in executable is a single-session, local capture/encode/decode experiment.
It is separate from `sunshine`, is not installed by packaging, and adds no encoder
selection, client negotiation, service, tvOS, or network behavior. Output is fixed
at 1920 x 1080, nominal 60 fps, SDR YUV420. The display mode is never changed.

The separate live host/client path is defined by the current
[Pyrowave v2 session contract](./pyrowave-session-v2.md), including complete
codec records independent of RTP fragmentation and negotiated 1080p/4K60 output.

The source dependency is pinned to Pyrowave
`89f7e47d4abbf650c91fae766728af866c5e32a0` (C API 0.6.0). Configuration rejects any
other HEAD or tracked changes, including dirty submodules. Supply a shared library
built from that checkout; the report records its configured SHA-256. That hash and
the runtime API version check identify the supplied binary but do not independently
prove its source provenance. Do not substitute a newer upstream build.
The executable hashes the actually loaded shared library before GPU initialization
and rejects a mismatch with the configured hash, including loader-path substitution.

## Build

Use the usual Apollo dependencies and initialize its build submodules at the
repository's recorded revisions. Do not update them to branch tips. Build in a
separate directory/worktree and leave the installed host alone.

```bash
git submodule update --init --recursive \
  third-party/Simple-Web-Server third-party/googletest third-party/inputtino \
  third-party/libdisplaydevice third-party/moonlight-common-c \
  third-party/nanors third-party/nv-codec-headers third-party/tray \
  third-party/wayland-protocols third-party/wlr-protocols

# Replace these with your pinned checkout and its corresponding build.
PYROWAVE_SOURCE=/path/to/pinned-pyrowave
PYROWAVE_LIBRARY=/path/to/pinned-pyrowave/build/libpyrowave-shared.so.0.6.0
git -C "$PYROWAVE_SOURCE" rev-parse HEAD

heavy cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DAPOLLO_BUILD_PYROWAVE_DIAGNOSTIC=ON \
  -DAPOLLO_PYROWAVE_SOURCE="$PYROWAVE_SOURCE" \
  -DAPOLLO_PYROWAVE_LIBRARY="$PYROWAVE_LIBRARY"
heavy --jobs-env CMAKE_BUILD_PARALLEL_LEVEL cmake --build build
heavy ctest --test-dir build --output-on-failure

# Apollo's existing suite is not registered with CTest. Run it explicitly,
# isolating its test files and appdata from the installed host's configuration.
APOLLO_TEST_DATA="$(mktemp -d /tmp/apollo-tests.XXXXXX)"
(
  cd build/tests  # the suite expects the build's copied fixtures here
  heavy env CONFIGURATION_DIRECTORY="$APOLLO_TEST_DATA" SUNSHINE_MIGRATE_CONFIG=0 \
    PULSE_SERVER="unix:$APOLLO_TEST_DATA/pulse-unavailable" \
    ./test_sunshine
)
```

Apollo's standard FFmpeg/Boost dependency options still apply. On machines with a
Vulkan loader but no development headers, add
`-DVulkan_INCLUDE_DIR="$PYROWAVE_SOURCE/Granite/third_party/khronos/vulkan-headers/include"`
and `-DVulkan_LIBRARY=/path/to/libvulkan.so` to configuration. The small diagnostic
CTest tests are registered even when Apollo's `BUILD_TESTS` is off. They cover
format/layout rejection, FD failure/consumption/reuse, color/channel semantics,
reference arithmetic, help and invalid CLI input. CPU mocks also execute the
live producer wait and native DMA-BUF allocation helpers: readable error fences,
failed/active/unknown status queries, compatible FD/image memory-type selection,
dedicated allocations and transactional image/memory/FD cleanup. These mocks
never initialize a GPU and do not validate an actual compositor DMA-BUF import.
Passing these three CTest tests does not establish that `test_sunshine` passed.
The isolated PulseAudio endpoint above prevents the suite from rerouting desktop
audio; its audio cases therefore do not validate hardware audio capture/encoding.

## Run

Keep reports and logs outside tracked source. No screen pixels or packet bytes are
written by this program; stdout may include Apollo's local monitor enumeration.
The JSON file contains metadata, timing samples and numeric reference errors.

```bash
heavy ./build/apollo-pyrowave-diagnostic --synthetic \
  --warmup 3 --frames 12 --reference-every 3 \
  --report /tmp/pyrowave-synthetic.json
```

Synthetic mode uploads an opaque 2560 x 1440 RGB fixture to Vulkan, scales it to
1080p, encodes, packetizes locally, decodes and reads back. It exercises the local
GPU codec/scaling/reference path; it does **not** exercise DMA-BUF import, KMS,
capture privileges, or live producer synchronization.
Use `--synthetic-10bit` instead to exercise packed ABGR2101010 SDR import semantics
on the owned upload image, including the opaque-alpha check and R16 YUV intermediates.
This is still an upload and does not exercise DMA-BUF image import.

For real capture, the operator must supply DRM node access and `CAP_SYS_ADMIN`
(permitted capability is sufficient: Apollo raises/drops effective capability
around KMS queries). After review, run this temporary launcher from a desktop-user
terminal. It returns to that UID/GID and initializes the user's supplementary
groups, grants only `CAP_SYS_ADMIN` across exec, and leaves no file capability:

```bash
umask 077
APOLLO_DIAG="$(realpath build/apollo-pyrowave-diagnostic)"
APOLLO_CAPTURE_OUT="$(mktemp -d /tmp/apollo-pyrowave-kms.XXXXXX)"
heavy sudo --preserve-env=HOME,WAYLAND_DISPLAY,XDG_RUNTIME_DIR,DISPLAY,XAUTHORITY \
  /usr/bin/setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups \
  --bounding-set=-all,+sys_admin --inh-caps=-all,+sys_admin \
  --ambient-caps=-all,+sys_admin --no-new-privs \
  /usr/bin/timeout --signal=TERM --kill-after=5s 130s "$APOLLO_DIAG" --display 0 \
  --warmup 30 --frames 120 --reference-every 60 --seconds 120 \
  --report "$APOLLO_CAPTURE_OUT/report.json" >"$APOLLO_CAPTURE_OUT/run.log" 2>&1
getcap "$APOLLO_DIAG"  # expected: no file capabilities
```

The launcher requires an operator's sudo authentication and a bounding set that
allows `CAP_SYS_ADMIN`; it has not been executed during nonprivileged validation.
If privilege setup fails, do not substitute a root capture process. No
`CAP_SYS_NICE` is requested; queues use ordinary priority. Do not restart the
host, run display preparation, or change modes to run this probe. Preserve the
desktop user's Wayland environment. Apollo's KMS numeric display selection is
reused without RAM or encoder fallback. `--help` lists all limits. Invalid layouts,
import failures, incomplete packets, budget overruns, deadline expiry and interrupts
return nonzero and produce a partial report after valid CLI parsing when a report
path was provided.

## What the path does

1. Reuse Apollo's KMS enumeration/selection and VRAM allocation/refresh helpers.
   Query primary-plane geometry each frame. Reject HDR, rotation, source crops,
   scanout scaling, active overlays, unknown HDR metadata and missing required
   geometry. No separate DRM capture stack.
2. Match the Vulkan device to the capture card by DRM primary major/minor or exact
   PCI domain/bus/device/function. Reject missing or ambiguous matches; never match
   by vendor/product alone. Use one owned Vulkan 1.3 device borrowed by Pyrowave.
3. Accept XRGB8888/XBGR8888 and packed X/ARGB2101010 or X/ABGR2101010 SDR. Map
   byte/bit order explicitly to BGRA/RGBA or A2R10G10B10/A2B10G10R10 UNORM.
   Alpha-bearing 10-bit formats require an opaque-alpha check of the owned snapshot
   **every frame**, before encode. Reject nonopaque pixel/global alpha, 8-bit alpha
   formats, YUV and missing modifiers. A 10-bit RGB framebuffer does not imply HDR.
   Preserve its 10-bit precision in the snapshot. Query
   the actual format/modifier's Vulkan support and reject auxiliary/multiple memory
   planes that KMS metadata cannot describe. Preserve exact pitch and offset.
4. Import the DMA-BUF into a caller-owned Vulkan image on the device borrowed by
   pinned Pyrowave. Query the actual modifier, copy usage and external-handle
   support, then preserve the explicit memory-plane pitch/offset. Query
   `vkGetMemoryFdPropertiesKHR` on the exact CLOEXEC duplicate subsequently imported.
   Select an ordinary memory type from the intersection of that FD mask and the
   native image requirements; reject an empty intersection or failed query.
   Always use a dedicated allocation, including modifiers that require it.
   The pinned library's image allocator cannot accept this intersection, so this
   path owns import allocation/binding and supplies the private snapshot through
   Pyrowave's borrowed-device image-view API. Vulkan consumes the duplicate only
   on successful allocation; subsequent bind failures destroy the image before
   freeing memory and preserve the original capture FD. Commit the import after
   binding and a successful writer wait. Keep the captured descriptor and imported
   image until encode completion; retain them during error unwinding until GPU cleanup has waited.
   Close unique GetFB/GetFB2 GEM handles after PRIME export in this executable.
5. Export the current DMA-BUF writer fence with `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`
   (`READ`) and wait up to two seconds. Readability also occurs on Linux fence
   errors: query `SYNC_IOC_FILE_INFO` before closing and accept only status `1`.
   Reject negative, active or unknown status and query/export/poll failures,
   closing the owned sync-file FD exactly once on every outcome. Perform
   FOREIGN acquire/release barriers around a GPU copy into an owned RGB snapshot.
   Export the copy's binary-semaphore SYNC_FD and publish a READ completion fence
   with `DMA_BUF_IOCTL_IMPORT_SYNC_FILE` before waiting for completion; fail on
   unavailable publication. Already-signaled SYNC_FD `-1` has no pending work.
   Require `VK_EXT_queue_family_foreign`; do not guess a different ownership model.
6. Sample the owned image using Pyrowave's scaled encode API. Choose gamma-domain
   bilinear scaling, sRGB SDR input/output, full-range BT.709 YCbCr, center 4:2:0
   chroma and 128/255 chroma midpoint; disable dither for repeatable comparisons.
   X bytes have no color meaning. Packed 10-bit input uses R16 UNORM YUV intermediates;
   8-bit input uses R8. Local CPU decoder readback is 8-bit in either case. The pinned
   scaler's header says "BT.701";
   its actual coefficients implement BT.709. Limited-range display-link settings
   are not treated as limited-range RGB framebuffer data. sRGB SDR source semantics
   are an explicit experiment assumption, not a compositor color-management audit.
7. Signal an encode completion timeline using Pyrowave's release synchronization;
   wait up to five seconds before querying/packetizing encoded output. Decode only
   complete locally delivered frames, then read back all three output planes.
8. On the first and every selected measured frame, read back the **same owned RGB
   snapshot** used for encoding. Compare against an independent CPU bilinear /
   BT.709 / 2x2 chroma reference and report per-plane MAE, MSE, maximum error and
   PSNR. This includes scaler rounding and lossy codec error; it is not a bit-exact
   oracle or an accepted visual-quality threshold. Zero MSE has `psnr_db: null`.

## Reading the evidence

`status: completed_with_open_gates` and exit zero mean the bounded local diagnostic
completed. They are not a Phase 3, quality, 60 fps, zero-copy, cursor, or ownership
pass. `phase3_gate` is always `not_claimed`; quality/fps verdicts remain unset.
Source tags and capture/import/encode/decode counts distinguish real and synthetic
work. Warmup and measured counts are separate. Samples include pipeline and full
diagnostic CPU wall times, with independent capture/import/producer-wait, snapshot
copy, reference/alpha GPU readback, CPU alpha check, scale/convert/encode/bitstream-wait, packetization,
local decode/readback and reference CPU comparison costs. These are not GPU kernel
times. The report summarizes nearest-rank p50/p95/p99/max and frame-period overruns.
Readback staging prefers cached coherent host memory; the actual flags are reported.
Live samples record the image-requirement mask, queried FD mask, chosen allocation
index and dedicated allocation. Synthetic samples have no import-memory evidence.
Decode readback and reference work perturb cadence; completed diagnostic
fps is not encoder-only or streaming fps.

The copied snapshot makes reference and encoded input stable relative to each
other. It **does not** establish exclusive ownership of the original compositor
buffer while copying. Exporting current reservation fences waits earlier writers;
KMS polling neither negotiates future reuse nor obtains a compositor buffer lease.
Publishing our read-completion fence bridges reservation-based implicit consumers,
but export/submit/publication is non-atomic and not a negotiated producer protocol.
Foreign barriers and GPU completion do not fix that producer protocol. The report
records this unresolved issue, and no absence-of-stalls claim follows from it.

Separate hardware cursor composition is deliberately unimplemented. Overlay
composition is also outside scope; active non-cursor overlays are rejected. Desktop
pixels, alpha semantics, modifier support
and actual same-GPU/live synchronization must be checked on real capture before any
phase gate can be discussed. This adapter makes an explicit RGB snapshot copy,
GPU conversion planes, bitstream staging and local decode/reference readbacks:
there is no end-to-end zero-copy claim.

The run has bounded frame counts and a between-frame wall-clock deadline. Copy,
producer-fence and encode-timeline waits have explicit timeouts. The pinned decoder
CPU readback API and device/destructor waits have no cancellation/timeout API; a
driver hang cannot be made bounded by this wrapper. The operator command adds an
external process deadline; forced termination cannot promise a final JSON report. SIGINT/SIGTERM request cleanup
at safe frame boundaries. FD counts before initialization and after cleanup are
observations, not proof of absence of driver/GPU leaks. Longer capture, GPU
contention, cursor work, and source synchronization remain separate gate items.

The import allocation rules follow the [Vulkan FD memory-type requirement](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryAllocateInfo.html)
and [FD ownership contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryFdInfoKHR.html).
The status check follows Linux's [sync-file implementation](https://github.com/torvalds/linux/blob/master/drivers/dma-buf/sync_file.c);
poll readiness alone is insufficient to establish successful producer completion.
