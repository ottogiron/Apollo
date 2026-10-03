/** @brief Live format transitions retain the negotiated output and initial input geometry. */
#pragma once

#include "pyrowave_diagnostic_support.h"
#include "src/pyrowave_protocol.h"

namespace pyrowave_diag {
  // The pinned codec allocates scaler planes on its first call, then ignores
  // intermediate_plane_format. Always retain 10-bit input precision, including
  // when startup captured 8-bit content. Output remains SDR full BT.709 4:2:0.
  inline constexpr VkFormat live_intermediate_format = VK_FORMAT_R16_UNORM;

  inline bool same_layout(const layout_t &a, const layout_t &b) {
    if (a.width != b.width || a.height != b.height || a.fourcc != b.fourcc || a.modifier != b.modifier) {
      return false;
    }
    for (size_t i = 0; i < a.fds.size(); ++i) {
      if ((a.fds[i] >= 0) != (b.fds[i] >= 0) || (a.fds[i] >= 0 && (a.pitches[i] != b.pitches[i] || a.offsets[i] != b.offsets[i]))) {
        return false;
      }
    }
    return true;
  }

  inline VkFormat validate_live_layout(const layout_t &layout, const layout_t *previous) {
    const auto format = validate_layout(layout);
    if (!pyrowave::supported_capture({int(layout.width), int(layout.height)})) {
      throw std::runtime_error("Pyrowave live capture requires an uncropped 16:9 source between 1920x1080 and 3840x2160: " + describe_layout(layout));
    }
    if (previous && (layout.width != previous->width || layout.height != previous->height)) {
      throw std::runtime_error("Framebuffer extent changed; reconnect required to update input mapping: " + describe_layout(*previous) + " -> " + describe_layout(layout));
    }
    return format;
  }
}  // namespace pyrowave_diag
