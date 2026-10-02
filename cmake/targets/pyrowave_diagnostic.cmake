if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT LIBDRM_FOUND OR NOT LIBCAP_FOUND)
    message(FATAL_ERROR "The Pyrowave diagnostic requires Linux KMS/libdrm/libcap")
endif()

set(APOLLO_PYROWAVE_SOURCE "" CACHE PATH "Checkout of the exact diagnostic Pyrowave pin")
set(APOLLO_PYROWAVE_LIBRARY "" CACHE FILEPATH "Shared library built from that checkout")
set(APOLLO_PYROWAVE_PIN "89f7e47d4abbf650c91fae766728af866c5e32a0")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${APOLLO_PYROWAVE_SOURCE}" rev-parse HEAD
        OUTPUT_VARIABLE pyro_head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE pyro_result)
if(NOT pyro_result EQUAL 0 OR NOT pyro_head STREQUAL APOLLO_PYROWAVE_PIN)
    message(FATAL_ERROR "APOLLO_PYROWAVE_SOURCE must be at ${APOLLO_PYROWAVE_PIN}")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${APOLLO_PYROWAVE_SOURCE}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE pyro_dirty RESULT_VARIABLE pyro_result)
if(NOT pyro_result EQUAL 0 OR NOT pyro_dirty STREQUAL "")
    message(FATAL_ERROR "Pinned Pyrowave checkout (including submodules) must have no tracked changes")
endif()
if(NOT EXISTS "${APOLLO_PYROWAVE_LIBRARY}" OR NOT EXISTS "${APOLLO_PYROWAVE_SOURCE}/pyrowave.h")
    message(FATAL_ERROR "Supply the pinned header checkout and its prebuilt shared library")
endif()
# A prebuilt binary is operator-supplied; HEAD alone cannot attest its provenance.
# Its hash is recorded in the build and report, and the guide requires rebuilding it from the pin.
file(SHA256 "${APOLLO_PYROWAVE_LIBRARY}" APOLLO_PYROWAVE_LIBRARY_SHA256)
find_package(Vulkan REQUIRED)

set(pyro_sources ${SUNSHINE_TARGET_FILES})
list(REMOVE_ITEM pyro_sources "${CMAKE_SOURCE_DIR}/src/main.cpp")
add_executable(apollo-pyrowave-diagnostic ${pyro_sources}
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_diagnostic.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_diagnostic_vulkan.cpp")
target_include_directories(apollo-pyrowave-diagnostic PRIVATE "${APOLLO_PYROWAVE_SOURCE}")
target_compile_definitions(apollo-pyrowave-diagnostic PRIVATE ${SUNSHINE_DEFINITIONS}
        APOLLO_PYROWAVE_DIAGNOSTIC
        APOLLO_PYROWAVE_PIN="${APOLLO_PYROWAVE_PIN}"
        APOLLO_PYROWAVE_LIBRARY_SHA256="${APOLLO_PYROWAVE_LIBRARY_SHA256}")
set_target_properties(apollo-pyrowave-diagnostic PROPERTIES CXX_STANDARD 23)
target_compile_options(apollo-pyrowave-diagnostic PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>
        $<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)
target_link_libraries(apollo-pyrowave-diagnostic PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES}
        ${EXTRA_LIBS} Vulkan::Vulkan "${APOLLO_PYROWAVE_LIBRARY}")
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(apollo-pyrowave-diagnostic ${dep})
endforeach()

# Small CPU tests can run without capabilities, a display, or Vulkan initialization.
enable_testing()
add_executable(pyrowave-diagnostic-tests
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_diagnostic_tests.cpp")
target_include_directories(pyrowave-diagnostic-tests PRIVATE "${APOLLO_PYROWAVE_SOURCE}" ${Vulkan_INCLUDE_DIRS})
set_target_properties(pyrowave-diagnostic-tests PROPERTIES CXX_STANDARD 23)
add_test(NAME pyrowave-diagnostic-contracts COMMAND pyrowave-diagnostic-tests)
add_test(NAME pyrowave-diagnostic-help COMMAND apollo-pyrowave-diagnostic --help)
add_test(NAME pyrowave-diagnostic-invalid-cli COMMAND apollo-pyrowave-diagnostic --frames 0)
set_tests_properties(pyrowave-diagnostic-invalid-cli PROPERTIES WILL_FAIL TRUE)
