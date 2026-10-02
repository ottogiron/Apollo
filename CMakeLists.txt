cmake_minimum_required(VERSION 3.20)
# `CMAKE_CUDA_ARCHITECTURES` requires 3.18
# `set_source_files_properties` requires 3.18
# `cmake_path(CONVERT ... TO_NATIVE_PATH_LIST ...)` requires 3.20
# todo - set this conditionally

project(Apollo VERSION 0.0.0
        DESCRIPTION "Self-hosted game stream host for Artemis"
        HOMEPAGE_URL "https://github.com/ClassicOldSong/Apollo")

set(PROJECT_LICENSE "GPL-3.0-only")

set(PROJECT_FQDN "dev.lizardbyte.app.Sunshine")

set(PROJECT_BRIEF_DESCRIPTION "GameStream host for Artemis")  # must be <= 35 characters

set(PROJECT_LONG_DESCRIPTION "Offering low latency, cloud gaming server capabilities with support for AMD, Intel, \
and Nvidia GPUs for hardware encoding. Software encoding is also available. You can connect to Apollo from any \
Artemis client on a variety of devices. A web UI is provided to allow configuration, and client pairing, from \
your favorite web browser. Pair from the local server or any mobile device.")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# set the module path, used for includes
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# export compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# set version info for this build
include(${CMAKE_MODULE_PATH}/prep/build_version.cmake)

# cmake build flags
include(${CMAKE_MODULE_PATH}/prep/options.cmake)

# initial prep
include(${CMAKE_MODULE_PATH}/prep/init.cmake)

# configure special package files, such as sunshine.desktop, Flatpak manifest, Portfile , etc.
include(${CMAKE_MODULE_PATH}/prep/special_package_configuration.cmake)

# Exit early if END_BUILD is ON, i.e. when only generating package manifests
if(${END_BUILD})
    return()
endif()

# project constants
include(${CMAKE_MODULE_PATH}/prep/constants.cmake)

# load macros
include(${CMAKE_MODULE_PATH}/macros/common.cmake)

# load dependencies
include(${CMAKE_MODULE_PATH}/dependencies/common.cmake)

# setup compile definitions
include(${CMAKE_MODULE_PATH}/compile_definitions/common.cmake)

# target definitions
option(APOLLO_ENABLE_PYROWAVE "Enable experimental negotiated Linux KMS Pyrowave sessions" OFF)
if(APOLLO_ENABLE_PYROWAVE)
    include(${CMAKE_MODULE_PATH}/targets/pyrowave_session.cmake)
endif()
include(${CMAKE_MODULE_PATH}/targets/common.cmake)

# Isolated experiment: never linked into or selected by the normal host.
option(APOLLO_BUILD_PYROWAVE_DIAGNOSTIC "Build the Linux KMS/Pyrowave diagnostic" OFF)
if(APOLLO_BUILD_PYROWAVE_DIAGNOSTIC)
    include(${CMAKE_MODULE_PATH}/targets/pyrowave_diagnostic.cmake)
endif()

# Focused protocol/ownership tests do not need capture access or a GPU.
if(BUILD_TESTS OR APOLLO_ENABLE_PYROWAVE)
    enable_testing()
    add_executable(pyrowave-live-tests tests/pyrowave_live.cc)
    target_include_directories(pyrowave-live-tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(pyrowave-live-tests PRIVATE Threads::Threads)
    set_target_properties(pyrowave-live-tests PROPERTIES CXX_STANDARD 17)
    add_test(NAME pyrowave-live-contracts COMMAND pyrowave-live-tests)
endif()

if(APOLLO_ENABLE_PYROWAVE AND NOT APOLLO_BUILD_PYROWAVE_DIAGNOSTIC)
    add_executable(pyrowave-live-import-tests
            src/platform/linux/pyrowave_diagnostic_tests.cpp
            src/platform/linux/pyrowave_diagnostic_import_tests.cpp)
    target_include_directories(pyrowave-live-import-tests PRIVATE ${Vulkan_INCLUDE_DIRS})
    set_target_properties(pyrowave-live-import-tests PROPERTIES CXX_STANDARD 23)
    add_test(NAME pyrowave-live-import-contracts COMMAND pyrowave-live-import-tests)
endif()

if(APOLLO_ENABLE_PYROWAVE)
    # Execute the real factory/constructor/wait/destructor with injected device
    # calls and an intercepted fatal endpoint; no capture access or process kill.
    add_executable(pyrowave-live-startup-tests tests/pyrowave_startup.cc
            src/platform/linux/pyrowave_session.cpp
            src/platform/linux/pyrowave_diagnostic_vulkan.cpp)
    add_dependencies(pyrowave-live-startup-tests pyrowave-alpha-shader)
    target_include_directories(pyrowave-live-startup-tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(pyrowave-live-startup-tests PRIVATE ${SUNSHINE_DEFINITIONS})
    target_link_libraries(pyrowave-live-startup-tests PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
    set_target_properties(pyrowave-live-startup-tests PROPERTIES CXX_STANDARD 23)
    foreach(symbol IN ITEMS
            _ZN13pyrowave_diag5gpu_t4initEPKN5platf21kms_diagnostic_info_tEbii
            _ZN13pyrowave_diag5gpu_t6importERKNS_8layout_tE
            _ZN13pyrowave_diag5gpu_t8snapshotEPKSt6vectorIhSaIhEE
            vkWaitSemaphores vkDeviceWaitIdle vkDestroyDevice vkDestroyImage
            pyrowave_encoder_encode_gpu_scaled_synchronous pyrowave_encoder_compute_num_packets
            pyrowave_encoder_get_mapped_raw_bitstream pyrowave_encoder_packetize)
        target_link_options(pyrowave-live-startup-tests PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
    add_test(NAME pyrowave-live-startup-recovery COMMAND pyrowave-live-startup-tests)
    set_tests_properties(pyrowave-live-startup-recovery PROPERTIES TIMEOUT 30)
endif()

# packaging
include(${CMAKE_MODULE_PATH}/packaging/common.cmake)
