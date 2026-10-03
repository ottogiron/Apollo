if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT LIBDRM_FOUND OR NOT LIBCAP_FOUND)
    message(FATAL_ERROR "Live Pyrowave requires Linux KMS/libdrm/libcap")
endif()
if(NOT WAYLAND_FOUND)
    message(FATAL_ERROR "Live Pyrowave requires Wayland/GBM for private screencopy")
endif()
include(${CMAKE_MODULE_PATH}/dependencies/pyrowave.cmake)
find_program(PYROWAVE_GLSLANG glslangValidator REQUIRED)
set(pyro_alpha_header "${CMAKE_BINARY_DIR}/generated-src/pyrowave_alpha_spv.h")
add_custom_command(OUTPUT "${pyro_alpha_header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated-src"
        COMMAND "${PYROWAVE_GLSLANG}" -V --vn pyrowave_alpha_spv
                -o "${pyro_alpha_header}" "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_alpha.comp"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_alpha.comp"
        VERBATIM)
add_custom_target(pyrowave-alpha-shader DEPENDS "${pyro_alpha_header}")
list(APPEND SUNSHINE_TARGET_DEPENDENCIES pyrowave-alpha-shader)
list(APPEND SUNSHINE_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_session.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_wayland.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_diagnostic_vulkan.cpp")
list(APPEND SUNSHINE_DEFINITIONS APOLLO_ENABLE_PYROWAVE
        APOLLO_PYROWAVE_PIN="${APOLLO_PYROWAVE_PIN}"
        APOLLO_PYROWAVE_LIBRARY_SHA256="${APOLLO_PYROWAVE_LIBRARY_SHA256}")
include_directories(SYSTEM "${APOLLO_PYROWAVE_SOURCE}" ${Vulkan_INCLUDE_DIRS})
include_directories("${CMAKE_BINARY_DIR}/generated-src")
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES Vulkan::Vulkan "${APOLLO_PYROWAVE_LIBRARY}")
