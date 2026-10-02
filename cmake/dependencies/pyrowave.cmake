include_guard(GLOBAL)
set(APOLLO_PYROWAVE_SOURCE "" CACHE PATH "Checkout of the exact Pyrowave pin")
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
