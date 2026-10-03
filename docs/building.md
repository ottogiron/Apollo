# Building
Sunshine binaries are built using [CMake](https://cmake.org) and requires `cmake` > 3.25.

## Building Locally

### Compiler
It is recommended to use one of the following compilers:

| Compiler    | Version |
|:------------|:--------|
| GCC         | 13+     |
| Clang       | 17+     |
| Apple Clang | 15+     |

### Dependencies

#### Linux
Dependencies vary depending on the distribution. You can reference our
[linux_build.sh](https://github.com/LizardByte/Sunshine/blob/master/scripts/linux_build.sh) script for a list of
dependencies we use in Debian-based and Fedora-based distributions. Please submit a PR if you would like to extend the
script to support other distributions.

##### CUDA Toolkit
Sunshine requires CUDA Toolkit for NVFBC capture. There are two caveats to CUDA:

1. The version installed depends on the version of GCC.
2. The version of CUDA you use will determine compatibility with various GPU generations.
   At the time of writing, the recommended version to use is CUDA ~12.9.
   See [CUDA compatibility](https://docs.nvidia.com/deploy/cuda-compatibility/index.html) for more info.

> [!NOTE]
> To install older versions, select the appropriate run file based on your desired CUDA version and architecture
> according to [CUDA Toolkit Archive](https://developer.nvidia.com/cuda-toolkit-archive)

#### macOS
You can either use [Homebrew](https://brew.sh) or [MacPorts](https://www.macports.org) to install dependencies.

##### Homebrew
```bash
dependencies=(
  "boost"  # Optional
  "cmake"
  "doxygen"  # Optional, for docs
  "graphviz"  # Optional, for docs
  "icu4c"  # Optional, if boost is not installed
  "miniupnpc"
  "ninja"
  "node"
  "openssl@3"
  "opus"
  "pkg-config"
)
brew install "${dependencies[@]}"
```

If there are issues with an SSL header that is not found:

@tabs{
  @tab{ Intel | ```bash
    ln -s /usr/local/opt/openssl/include/openssl /usr/local/include/openssl
    ```}
  @tab{ Apple Silicon | ```bash
    ln -s /opt/homebrew/opt/openssl/include/openssl /opt/homebrew/include/openssl
    ```
  }
}

##### MacPorts
```bash
dependencies=(
  "cmake"
  "curl"
  "doxygen"  # Optional, for docs
  "graphviz"  # Optional, for docs
  "libopus"
  "miniupnpc"
  "ninja"
  "npm9"
  "pkgconfig"
)
sudo port install "${dependencies[@]}"
```

#### Windows
First you need to install [MSYS2](https://www.msys2.org), then startup "MSYS2 UCRT64" and execute the following
commands.

##### Update all packages
```bash
pacman -Syu
```

##### Install dependencies
```bash
dependencies=(
  "git"
  "mingw-w64-ucrt-x86_64-boost"  # Optional
  "mingw-w64-ucrt-x86_64-cmake"
  "mingw-w64-ucrt-x86_64-cppwinrt"
  "mingw-w64-ucrt-x86_64-curl-winssl"
  "mingw-w64-ucrt-x86_64-doxygen"  # Optional, for docs... better to install official Doxygen
  "mingw-w64-ucrt-x86_64-graphviz"  # Optional, for docs
  "mingw-w64-ucrt-x86_64-MinHook"
  "mingw-w64-ucrt-x86_64-miniupnpc"
  "mingw-w64-ucrt-x86_64-nsis"
  "mingw-w64-ucrt-x86_64-onevpl"
  "mingw-w64-ucrt-x86_64-openssl"
  "mingw-w64-ucrt-x86_64-opus"
  "mingw-w64-ucrt-x86_64-toolchain"
  "mingw-w64-ucrt-x86_64-nlohmann_json"
)
pacman -S "${dependencies[@]}"
```

##### Install Node.js
Install Node.js separately from [nodejs.org](https://nodejs.org/) (LTS or current) or via
[nvm-windows](https://github.com/coreybutler/nvm-windows). Don't install MSYS2's
`mingw-w64-ucrt-x86_64-nodejs` — it's compiled with the MSYS2 gcc-16 libstdc++ which has
a `std::bad_weak_ptr` regression that crashes Node during process init (see
[apache/arrow#49958](https://github.com/apache/arrow/issues/49958) for the upstream
toolchain trail). The official MSVC-built Node.js isn't affected.

Make sure `node.exe` is on `PATH` before running `cmake` — the `web-ui` CMake target
invokes `npm install` via `find_program(NPM npm)`, so the official Node's `npm` must be
visible to CMake.

### Clone
Ensure [git](https://git-scm.com) is installed on your system, then clone the repository using the following command:

```bash
git clone https://github.com/ClassicOldSong/Apollo.git --recurse-submodules
cd Apollo
mkdir build
```

### Build

```bash
cmake -B build -G Ninja -S .
ninja -C build
```

> [!TIP]
> Available build options can be found in
> [options.cmake](https://github.com/LizardByte/Sunshine/blob/master/cmake/prep/options.cmake).

### Experimental Pyrowave Linux host

Pyrowave stays off by default. Ordinary builds never include its preparation,
Vulkan lookup, library verification or installation rules. Enabling it preserves
the [v2 contract](./pyrowave-session-v2.md), conventional codecs, runtime opt-in and
existing capture limits. This procedure builds and stages packages; it does not
activate a streaming service or change displays.

The exact codec identity is `5e4a98f807dddd2498824e3b55ef2fe1845bcc59`, tree
`ad55a253c952a59bf180f9e52ead021b6de38f26`. This local commit is **not published
upstream**. The tracked `cmake/dependencies/pyrowave/scan-fix.patch` includes the
reviewed scan correction and generated Vulkan/Metal shader changes; the small raw
`scan-fix.commit` preserves the original author, timestamps, parent and message.
Preparation fetches only the public base, applies the patch, checks its tree, and
hashes that commit object to reconstruct the exact advertised identity. It never
requests the local SHA from upstream and needs no bundle or prebuilt codec binary.

| Public source | Pinned revision | Purpose |
| --- | --- | --- |
| [Pyrowave](https://github.com/Themaister/pyrowave) | `89f7e47d4abbf650c91fae766728af866c5e32a0` | Public codec base |
| [Granite](https://github.com/Themaister/Granite) | `1b2d1801d2910fb09ebcded2f0bb3a3a781103b5` | Shipping Vulkan backend |
| [volk](https://github.com/zeux/volk) | `47cddf7ed97b94118a08aacb548a411188e016cc` | Required Granite gitlink |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `6802bb4733b63ed5efd3adb308a6c885ef180ea1` | Required Granite gitlink and host headers |

The minimal backend does not use SPIRV-Cross (`d029329bd164a3f38338d95ab56c74f27128a029`
in Granite), shaderc, or other Granite submodules. Embedded shaders are used as
reviewed, with no regeneration. The source preparation script is reusable on a
client machine; the shared-library build/package procedure below targets Linux.

In addition to ordinary Apollo dependencies, install Git, CMake, Ninja, the Vulkan
loader development library, `glslangValidator`, and binutils (`readelf`). The pinned
headers are found automatically. On workstations with `heavy`, use the commands
below; elsewhere remove the `heavy` prefix while retaining the four-job limit.

Start with an Apollo revision containing this recipe (implementation baseline:
`46080a2da602b397ce74a01635c44f90d1211d59`). Set `APOLLO_REVISION` to the reviewed
revision you received; this does not assume that the experimental branch is public.

```bash
git clone https://github.com/ottogiron/Apollo.git Apollo
cd Apollo
git checkout "$APOLLO_REVISION"
git submodule update --init --recursive

PYROWAVE_SOURCE="$PWD/build/pyrowave-source"
PYROWAVE_BUILD_DIR="$PWD/build/pyrowave-codec"
cmake -DAPOLLO_PYROWAVE_SOURCE="$PYROWAVE_SOURCE" \
  -P cmake/dependencies/pyrowave/prepare.cmake
heavy cmake -DAPOLLO_PYROWAVE_SOURCE="$PYROWAVE_SOURCE" \
  -DPYROWAVE_BUILD_DIR="$PYROWAVE_BUILD_DIR" \
  -P cmake/dependencies/pyrowave/build.cmake
```

Preparation is idempotent and needs no network on a valid prepared checkout. It
refuses to overwrite a nonempty destination or repair dirty/mismatched inputs.
After a failed download, inspect and remove only that dedicated source directory
before retrying. Keep the codec build directory outside its source. The build
recipe creates only the shared C ABI library and its required static dependencies;
upstream sandbox, GPU tests and diagnostics are absent. It emits an unsigned local
`apollo-provenance.json` receipt with source/tree/Granite identities, recipe digest,
toolchain and library SHA-256. This records a trusted local build, not independent
or cryptographic proof against a forged receipt or malicious compiler.

Apollo verifies the receipt, exact clean sources, required submodules, library hash
and SONAME at configure, build and install. Unexpected files inside uninitialized
Granite submodules are rejected too. If the recipe or library changes, rebuild the
codec and reconfigure Apollo. Do not reuse a receipt from a different recipe.

Configure a non-system prefix to execute the staged binary without privileges.
Explicit metadata makes this candidate identifiable; use a new version for each
candidate. CUDA is optional and disabled here; DRM/KMS remains enabled.

```bash
APOLLO_PREFIX="$PWD/build/pyrowave-prefix"
APOLLO_STAGE="$PWD/build/pyrowave-stage"
export BUILD_VERSION=2026.1003.6-step1
export BRANCH="$(git branch --show-current)"
export COMMIT="$(git rev-parse --short HEAD)"
cmake -S . -B build/pyrowave-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
  -DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF \
  -DAPOLLO_ENABLE_PYROWAVE=ON -DAPOLLO_BUILD_PYROWAVE_DIAGNOSTIC=OFF \
  -DAPOLLO_PYROWAVE_SOURCE="$PYROWAVE_SOURCE" \
  -DAPOLLO_PYROWAVE_LIBRARY="$PYROWAVE_BUILD_DIR/libpyrowave-shared.so.0.6.0" \
  -DCMAKE_INSTALL_PREFIX="$APOLLO_PREFIX"
heavy cmake --build build/pyrowave-host --parallel 4
heavy ctest --test-dir build/pyrowave-host --parallel 4 --output-on-failure

# DESTDIR also contains Apollo's absolute udev/systemd package destinations.
# Copy only the non-system prefix for execution; no units/rules are activated.
DESTDIR="$APOLLO_STAGE" cmake --install build/pyrowave-host
mkdir -p "$APOLLO_PREFIX"
cp -a "$APOLLO_STAGE$APOLLO_PREFIX/." "$APOLLO_PREFIX/"
PYROWAVE_PIN=5e4a98f807dddd2498824e3b55ef2fe1845bcc59
readelf -d "$APOLLO_PREFIX/bin/sunshine" | rg 'NEEDED|RUNPATH|RPATH'
env -u LD_LIBRARY_PATH ldd "$APOLLO_PREFIX/bin/sunshine" | rg pyrowave
env -u LD_LIBRARY_PATH CONFIGURATION_DIRECTORY="$PWD/build/pyrowave-version-data" \
  SUNSHINE_MIGRATE_CONFIG=0 "$APOLLO_PREFIX/bin/sunshine" --version
readelf -d "$APOLLO_PREFIX/lib/apollo/pyrowave/$PYROWAVE_PIN/libpyrowave-shared.so.0.6.0" \
  | rg 'SONAME|NEEDED'
```

Keep those metadata variables exported for later builds: CMake may automatically
reconfigure, and Apollo's existing version module reads the environment again.

The package supplies `libpyrowave-shared.so.0.6.0` and a relative
`libpyrowave-shared.so.0` SONAME symlink under
`<libdir>/apollo/pyrowave/<codec-pin>`, with receipt and licenses. No optional
diagnostic is shipped. Enabled DEB/RPM dependency lists include the Vulkan loader.
`libdir` follows GNUInstallDirs and may be `lib64` on some distributions; use the
configured `CMAKE_INSTALL_LIBDIR` in the library inspection command in that case.

The host's **absolute install RUNPATH** points at the configured private library
directory. Linux secure execution with `cap_sys_admin,cap_sys_nice` ignores
`LD_LIBRARY_PATH` and restricts `$ORIGIN`, so neither is needed for this dependency.
For eventual deployment, configure the final prefix before building and make that
binary, library and all parent directories administrator-owned and unwritable by
unprivileged users before assigning capabilities. The staged checks above assign
no capabilities and do not claim a privileged capture acceptance test. Do not
override the prefix at install time or relocate this capability-enabled package:
its encoded lookup path must match the final installation. `DESTDIR` stages that
layout without changing the encoded path.

To check ordinary-build independence, configure a separate build with
`-DAPOLLO_ENABLE_PYROWAVE=OFF -DAPOLLO_BUILD_PYROWAVE_DIAGNOSTIC=OFF`; no Pyrowave
source/library arguments or codec downloads are needed. Apollo's usual dependency
downloads remain subject to the normal build prerequisites.

Apollo's older `test_sunshine` suite is not registered with CTest. To inspect its
CPU-only baseline without audio/input/encoder hardware probes:

```bash
APOLLO_TEST_DATA="$PWD/build/pyrowave-test-data"
mkdir -p "$APOLLO_TEST_DATA"
(
  cd build/pyrowave-host/tests
  heavy env CONFIGURATION_DIRECTORY="$APOLLO_TEST_DATA" SUNSHINE_MIGRATE_CONFIG=0 \
    PULSE_SERVER="unix:$APOLLO_TEST_DATA/pulse-unavailable" \
    ./test_sunshine --gtest_filter='-*AudioTest*:*MouseHIDTest*:*EncoderTest*'
)
```

Rollback this staging exercise by removing only its dedicated output directories:

```bash
cmake -E rm -rf "$APOLLO_PREFIX" "$APOLLO_STAGE"
```

The installed conventional host and its configuration remain the rollback for a
later integration exercise. Disabling the experimental runtime setting retains
conventional sessions; this build procedure does not enable that setting.

Validation on Linux with CMake 4.4.4/GCC 16.2.1 (2026-10-03) reconstructed the exact
commit/tree using public downloads. The codec SHA-256 was
`58193d6989f9b945006a33bd98c2aa31e69254419408c9a2bbebfd9bbca02ac3`, identical to
the reviewed library on that toolchain. Both complete Apollo builds passed;
enabled CTest passed 4/4 and disabled CTest 1/1. Staged installation, repeat install,
prefix-change rejection, SONAME and dependency lookup without `LD_LIBRARY_PATH`
passed. No capabilities or live capture were used for staging verification.
The legacy suite's CPU baseline was 195 passed/33 failed in both builds: 2 config
consistency cases, 28 display-config parsing/remapping cases and 3 mDNS name cases.
The disabled full legacy run also skipped 11 hardware cases and reported 3 failed
hardware suite setups. Those existing failures are recorded, not treated as a
passing full legacy suite or addressed by this packaging change.

### Package

@tabs{
  @tab{Linux | @tabs{
    @tab{deb | ```bash
      cpack -G DEB --config ./build/CPackConfig.cmake
      ```}
    @tab{rpm | ```bash
      cpack -G RPM --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{macOS | @tabs{
    @tab{DragNDrop | ```bash
      cpack -G DragNDrop --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{Windows | @tabs{
    @tab{Installer | ```bash
      cpack -G NSIS --config ./build/CPackConfig.cmake
      ```}
    @tab{Portable | ```bash
      cpack -G ZIP --config ./build/CPackConfig.cmake
      ```}
  }}
}

### Remote Build
It may be beneficial to build remotely in some cases. This will enable easier building on different operating systems.

1. Fork the project
2. Activate workflows
3. Trigger the *CI* workflow manually
4. Download the artifacts/binaries from the workflow run summary

<div class="section_buttons">

| Previous                              |                            Next |
|:--------------------------------------|--------------------------------:|
| [Troubleshooting](troubleshooting.md) | [Contributing](contributing.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
