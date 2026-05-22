# Apollo — Agent Guidance

## Project Overview

Self-hosted desktop game-streaming host (fork of Sunshine) for Artemis/Moonlight clients.
Supports virtual display, HDR, dual-GPU, and hardware encoding (NVENC, VAAPI, VideoToolbox).

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
