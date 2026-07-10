# Apollo — Agent Guidance

## Project Overview

Self-hosted desktop game-streaming host (fork of Sunshine) for Artemis/Moonlight clients.
Supports virtual display, HDR, dual-GPU, and hardware encoding (NVENC, VAAPI, VideoToolbox).

## Local Fork Patches

This is Otto's fork (`origin` = `ottogiron/Apollo`, `upstream` = `ClassicOldSong/Apollo`). `master` tracks upstream `master` plus a small set of local experiment patches that are **not** part of upstream. List them with:

```bash
git log --oneline upstream/master..master
```

Current local patches (canonical refs on `backup/fork-experiments-2026-04-27`, cherry-picked onto `master`):

- **Implement Linux thread priority (pthread/nice)** (`10b21461`) — `SCHED_RR` real-time scheduling for encode/capture threads on Linux, falling back to `nice`. Touches `src/platform/linux/misc.cpp`. Requires the `cap_sys_nice` capability on the installed binary.
- **Add optional Opus in-band FEC** (`82d38f60`) — optional in-band Forward Error Correction for the Opus audio stream, enabled via the `opus_fec_packet_loss_percent` config key. Touches `src/audio.cpp`, `src/config.{cpp,h}`. Off by default.

### Updating from upstream

After fetching upstream, `master` fast-forwards; the local patches must then be re-applied:

```bash
git fetch upstream
git checkout master
git merge --ff-only upstream/master
git cherry-pick 10b21461 82d38f60   # re-apply local patches; resolve if they conflict
```

Build, install, and PC setup (setcap incl. `cap_sys_nice`, `apollo` symlink, udev, service) are documented in the machine reference `~/agent-config/references/pc-cachyos-apollo.md` and the `apollo-build-install` skill — not repeated here.

## Module Map

- `src/` — Core C++ (audio, video, stream, nvhttp, input, network, display_device)
- `src/platform/` — OS-specific code (linux, macos, windows)
- `src/nvenc/` — NVIDIA encoding
- `src_assets/` — Vue/Vite web UI
- `cmake/` — Build modules
- `tests/` — Test suite
- `docs/` — Documentation
- `third-party/` — Vendored dependencies

## Build Commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build
```

## Code Style

- Follow `.clang-format` (LLVM-based, 2-space indent, pointer-right, BinPack off)
- C++17 standard
- Use `snake_case` for functions and variables, `PascalCase` for types

## Agent Expectations

- Run `cmake --build build` and `ctest` before submitting work
- Keep changes scoped — don't refactor unrelated code
- Platform-specific changes should only touch the relevant `src/platform/<os>/` directory
- Web UI changes go in `src_assets/`

## Review Chain

- Wait for the full review chain to complete before marking work done
- Consolidate all findings into one rework pass
- Record any reviewer bypass with reason
