include_guard(GLOBAL)
set(APOLLO_PYROWAVE_SOURCE "" CACHE PATH "Checkout of the exact Pyrowave pin")
set(APOLLO_PYROWAVE_LIBRARY "" CACHE FILEPATH "Shared library built from that checkout")
include("${CMAKE_CURRENT_LIST_DIR}/pyrowave/verify-library.cmake")
# Verify again before dependent targets build and before installation, catching
# source/library mutations after configure rather than silently shipping them.
add_custom_target(pyrowave-verify
    COMMAND "${CMAKE_COMMAND}" "-DAPOLLO_PYROWAVE_SOURCE=${APOLLO_PYROWAVE_SOURCE}"
        "-DAPOLLO_PYROWAVE_LIBRARY=${APOLLO_PYROWAVE_LIBRARY}"
        "-DPYROWAVE_EXPECTED_HASH=${APOLLO_PYROWAVE_LIBRARY_SHA256}"
        -P "${CMAKE_CURRENT_LIST_DIR}/pyrowave/verify-library.cmake"
    VERBATIM)
list(APPEND SUNSHINE_TARGET_DEPENDENCIES pyrowave-verify)

# Private, pinned package dependency. Absolute install RUNPATH is deliberate:
# Linux file capabilities invoke secure execution, which ignores LD_LIBRARY_PATH
# and restricts $ORIGIN. Never depend on the developer's codec build directory.
set(APOLLO_PYROWAVE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/apollo/pyrowave/${APOLLO_PYROWAVE_PIN}")
set(APOLLO_PYROWAVE_INSTALL_FULL_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/apollo/pyrowave/${APOLLO_PYROWAVE_PIN}")
get_filename_component(pyro_library_dir "${APOLLO_PYROWAVE_LIBRARY}" DIRECTORY)
install(CODE "
    if(NOT CMAKE_INSTALL_PREFIX STREQUAL \"${CMAKE_INSTALL_PREFIX}\")
        message(FATAL_ERROR \"Pyrowave install prefix changed; reconfigure Apollo for its final absolute RUNPATH\")
    endif()
    set(APOLLO_PYROWAVE_SOURCE \"${APOLLO_PYROWAVE_SOURCE}\")
    set(APOLLO_PYROWAVE_LIBRARY \"${APOLLO_PYROWAVE_LIBRARY}\")
    set(PYROWAVE_EXPECTED_HASH \"${APOLLO_PYROWAVE_LIBRARY_SHA256}\")
    include(\"${CMAKE_CURRENT_LIST_DIR}/pyrowave/verify-library.cmake\")
")
install(FILES "${pyro_library_dir}/libpyrowave-shared.so.0.6.0"
    DESTINATION "${APOLLO_PYROWAVE_INSTALL_DIR}")
install(CODE "
    file(CREATE_LINK libpyrowave-shared.so.0.6.0
        \"\$ENV{DESTDIR}${APOLLO_PYROWAVE_INSTALL_FULL_DIR}/libpyrowave-shared.so.0\" SYMBOLIC)
")
install(FILES "${pyro_library_dir}/apollo-provenance.json"
    DESTINATION "${APOLLO_PYROWAVE_INSTALL_DIR}")
install(FILES "${APOLLO_PYROWAVE_SOURCE}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/apollo-pyrowave")
install(FILES "${APOLLO_PYROWAVE_SOURCE}/Granite/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/apollo-pyrowave" RENAME Granite-LICENSE)
install(FILES "${APOLLO_PYROWAVE_SOURCE}/Granite/third_party/volk/LICENSE.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/apollo-pyrowave" RENAME volk-LICENSE.md)
install(FILES "${APOLLO_PYROWAVE_SOURCE}/Granite/third_party/khronos/vulkan-headers/LICENSE.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/apollo-pyrowave" RENAME Vulkan-Headers-LICENSE.md)
install(DIRECTORY "${APOLLO_PYROWAVE_SOURCE}/Granite/third_party/khronos/vulkan-headers/LICENSES/"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/apollo-pyrowave/Vulkan-Headers")
find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h
    HINTS "${APOLLO_PYROWAVE_SOURCE}/Granite/third_party/khronos/vulkan-headers/include")
find_package(Vulkan REQUIRED)
