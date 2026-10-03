# A local CPU fake compositor drives the actual client adapter. No live socket,
# DRM/GBM device, Vulkan instance or hardware probe is used.
pkg_check_modules(PYROWAVE_WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
foreach(protocol IN ITEMS linux-dmabuf-unstable-v1 xdg-output-unstable-v1 wlr-screencopy-unstable-v1)
    if(protocol STREQUAL "wlr-screencopy-unstable-v1")
        set(xml "${CMAKE_SOURCE_DIR}/third-party/wlr-protocols/unstable/${protocol}.xml")
    elseif(protocol STREQUAL "xdg-output-unstable-v1")
        set(xml "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/${protocol}.xml")
    else()
        set(xml "${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/${protocol}.xml")
    endif()
    execute_process(COMMAND wayland-scanner server-header "${xml}"
        "${CMAKE_BINARY_DIR}/generated-src/${protocol}-server.h"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
add_executable(pyrowave-wayland-tests tests/pyrowave_wayland.cc
        src/platform/linux/pyrowave_wayland.cpp
        "${CMAKE_BINARY_DIR}/generated-src/linux-dmabuf-unstable-v1.c"
        "${CMAKE_BINARY_DIR}/generated-src/xdg-output-unstable-v1.c"
        "${CMAKE_BINARY_DIR}/generated-src/wlr-screencopy-unstable-v1.c")
target_include_directories(pyrowave-wayland-tests PRIVATE "${CMAKE_SOURCE_DIR}" ${Vulkan_INCLUDE_DIRS})
set_target_properties(pyrowave-wayland-tests PROPERTIES CXX_STANDARD 23)
target_link_libraries(pyrowave-wayland-tests PRIVATE ${WAYLAND_LIBRARIES} PkgConfig::PYROWAVE_WAYLAND_SERVER gbm ${LIBDRM_LIBRARIES} Threads::Threads nlohmann_json::nlohmann_json Boost::headers)
foreach(symbol IN ITEMS drmGetDeviceFromDevId drmFreeDevice gbm_create_device gbm_device_destroy
        gbm_bo_create_with_modifiers2 gbm_bo_destroy gbm_bo_get_width gbm_bo_get_height gbm_bo_get_format
        gbm_bo_get_modifier gbm_bo_get_plane_count gbm_bo_get_fd_for_plane gbm_bo_get_stride_for_plane gbm_bo_get_offset)
    target_link_options(pyrowave-wayland-tests PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
add_test(NAME pyrowave-wayland-cpu-protocol COMMAND pyrowave-wayland-tests)
set_tests_properties(pyrowave-wayland-cpu-protocol PROPERTIES TIMEOUT 30)
