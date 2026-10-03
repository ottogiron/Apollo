/** @brief CPU test injection for conventional WLR display/capture I/O. */
#pragma once

#if defined(SUNSHINE_TESTS) && defined(SUNSHINE_BUILD_WAYLAND)
  #include "src/platform/common.h"

namespace egl {
  struct surface_descriptor_t;
}

namespace wl::test {
  // Replaces Wayland/EGL initialization and frame acquisition I/O. Image
  // allocation, backend/device selection, geometry checks and conversion stay real.
  extern std::function<int(platf::display_t &)> init_display;
  extern std::function<platf::capture_e(egl::surface_descriptor_t &)> capture_frame;
}  // namespace wl::test
#endif
