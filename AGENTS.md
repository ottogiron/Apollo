# Apollo Agent Guidance

## Project Overview

Apollo is a self-hosted desktop streaming host for Artemis/Moonlight clients. It is a C++/CMake application with a Vue/Vite web UI, platform packaging, bundled third-party dependencies, and gtest-based tests.

Current work should stay focused on Apollo behavior, build/install reliability, streaming service integration, Linux display/audio paths, web UI configuration, and tests or docs that directly support the change.

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
