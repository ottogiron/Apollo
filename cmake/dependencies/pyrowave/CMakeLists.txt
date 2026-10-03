cmake_minimum_required(VERSION 3.20)
project(apollo_pyrowave LANGUAGES C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/source.cmake")
pyrowave_check_source("${APOLLO_PYROWAVE_SOURCE}")
include(GNUInstallDirs)
find_package(Threads REQUIRED)

# Match the reviewed standalone shipping backend. No shader regeneration,
# sandbox, GPU tests, renderer, FFmpeg or optional diagnostic executables.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 99)
foreach(flag IN ITEMS GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER
        GRANITE_SHADER_COMPILER_OPTIMIZE GRANITE_VULKAN_SPIRV_CROSS
        GRANITE_VULKAN_SYSTEM_HANDLES GRANITE_RENDERER GRANITE_VULKAN_FOSSILIZE
        GRANITE_FFMPEG GRANITE_FFMPEG_VULKAN GRANITE_SHARED GRANITE_INSTALL_TARGETS
        GRANITE_TOOLS GRANITE_TARGET_NATIVE GRANITE_FAST_MATH
        PYROWAVE_DEVEL PYROWAVE_UTILS PYROWAVE_FP32_STORAGE)
    set(${flag} OFF CACHE BOOL "Pinned minimal Pyrowave backend" FORCE)
endforeach()
foreach(flag IN ITEMS GRANITE_POSITION_INDEPENDENT GRANITE_SHIPPING PYROWAVE_FP32_MATH)
    set(${flag} ON CACHE BOOL "Pinned minimal Pyrowave backend" FORCE)
endforeach()
set(GRANITE_PLATFORM "null" CACHE STRING "Pinned minimal Pyrowave backend" FORCE)
add_subdirectory("${APOLLO_PYROWAVE_SOURCE}/Granite" Granite EXCLUDE_FROM_ALL)
add_subdirectory("${APOLLO_PYROWAVE_SOURCE}" codec EXCLUDE_FROM_ALL)

# Upstream only defines its C ABI shared target when built at the top level.
# Reproduce that target here so upstream's unconditional diagnostic install
# rules never enter the Apollo package. The source files remain byte identical.
add_library(pyrowave-shared SHARED
    "${APOLLO_PYROWAVE_SOURCE}/pyrowave_c.cpp"
    "${APOLLO_PYROWAVE_SOURCE}/Granite/video/scaler.cpp")
target_link_libraries(pyrowave-shared PRIVATE pyrowave granite-vulkan granite-math)
target_include_directories(pyrowave-shared PRIVATE
    "${APOLLO_PYROWAVE_SOURCE}/shaders" "${APOLLO_PYROWAVE_SOURCE}/Granite/video")
target_compile_definitions(pyrowave-shared PRIVATE PYROWAVE_EXPORT_SYMBOLS)
get_target_property(pyrowave_flags pyrowave COMPILE_OPTIONS)
target_compile_options(pyrowave-shared PRIVATE ${pyrowave_flags})
set_target_properties(pyrowave-shared PROPERTIES VERSION 0.6.0 SOVERSION 0)
if(UNIX AND NOT APPLE)
    target_link_options(pyrowave-shared PRIVATE "LINKER:--version-script=${APOLLO_PYROWAVE_SOURCE}/link.T")
endif()
target_link_libraries(pyrowave-shared PRIVATE Threads::Threads)
install(TARGETS pyrowave-shared LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${APOLLO_PYROWAVE_SOURCE}/pyrowave.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pyrowave")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/apollo-toolchain.json" CONTENT
    "{\"cmake\": \"${CMAKE_VERSION}\", \"compiler\": \"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\", \"build_type\": \"${CMAKE_BUILD_TYPE}\"}")
