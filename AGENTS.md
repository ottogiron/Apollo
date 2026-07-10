# Apollo Agent Guidance

## Project Overview

Apollo is a self-hosted desktop streaming host for Artemis/Moonlight clients. It is a C++/CMake application with a Vue/Vite web UI, platform packaging, bundled third-party dependencies, and gtest-based tests.

Current work should stay focused on Apollo behavior, build/install reliability, streaming service integration, Linux display/audio paths, web UI configuration, and tests or docs that directly support the change.

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

## Key Directories

- `src/`: application source.
- `src_assets/common/assets/web/`: web UI pages, Vue components, locale JSON, and Vite assets.
- `tests/`: gtest unit tests; the test executable is `test_sunshine`.
- `cmake/`: build options, dependency loading, target definitions, and packaging glue.
- `packaging/`: platform package manifests and install/service assets.
- `docs/`: user and contributor documentation.
- `third-party/`: vendored dependencies; avoid changing these unless the task is explicitly dependency work.

## Build, Test, And Run

- Configure: `cmake -B build -G Ninja -S . -DBUILD_TESTS=ON`
- Build: `cmake --build build`
- Run tests: `./build/tests/test_sunshine`
- Web UI watch build: `npm run dev`
- Web UI bundle build: `npm run build`

If an existing local build directory is already being used for the task, continue with it instead of creating another one. Report the exact commands run and any command that could not be run.

## Coding Rules

- Follow the existing style and `.clang-format` for C and C++ changes.
- Keep platform-specific logic in the existing platform modules and CMake includes.
- For web UI work, use the existing Vue, Vite, Bootstrap, EJS, and locale patterns.
- Add or update tests when behavior changes. If hardware, GPU, display, or service behavior cannot be covered in CI, document the manual verification performed or the remaining gap.
- Do not commit generated localization templates or compiled translation files unless the task explicitly requires release-generated artifacts.
- Keep host-specific service, monitor, display switching, and capability setup notes outside tracked files unless they apply generally to Apollo users.
