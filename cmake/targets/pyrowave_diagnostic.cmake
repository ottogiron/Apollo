if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT LIBDRM_FOUND OR NOT LIBCAP_FOUND)
    message(FATAL_ERROR "The Pyrowave diagnostic requires Linux KMS/libdrm/libcap")
endif()

include(${CMAKE_MODULE_PATH}/dependencies/pyrowave.cmake)

set(pyro_sources ${SUNSHINE_TARGET_FILES})
list(REMOVE_ITEM pyro_sources "${CMAKE_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_diagnostic_vulkan.cpp")
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
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_diagnostic_tests.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/pyrowave_diagnostic_import_tests.cpp")
target_include_directories(pyrowave-diagnostic-tests PRIVATE "${APOLLO_PYROWAVE_SOURCE}" ${Vulkan_INCLUDE_DIRS})
set_target_properties(pyrowave-diagnostic-tests PROPERTIES CXX_STANDARD 23)
add_test(NAME pyrowave-diagnostic-contracts COMMAND pyrowave-diagnostic-tests)
add_test(NAME pyrowave-diagnostic-help COMMAND apollo-pyrowave-diagnostic --help)
add_test(NAME pyrowave-diagnostic-invalid-cli COMMAND apollo-pyrowave-diagnostic --frames 0)
set_tests_properties(pyrowave-diagnostic-invalid-cli PROPERTIES WILL_FAIL TRUE)
