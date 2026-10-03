/** @brief Actual GBM plane export and Vulkan validation, injectable without creating a GPU device. */
#pragma once

#include "pyrowave_wayland_state.h"

#include <gbm.h>
#include <unistd.h>

namespace pyrowave_wl {
  using validator_t = std::function<void(const pyrowave_diag::layout_t &)>;

  struct gbm_api_t {
    decltype(&gbm_bo_create_with_modifiers2) create;
    decltype(&gbm_bo_destroy) destroy;
    decltype(&gbm_bo_get_width) width;
    decltype(&gbm_bo_get_height) height;
    decltype(&gbm_bo_get_format) format;
    decltype(&gbm_bo_get_modifier) modifier;
    decltype(&gbm_bo_get_plane_count) plane_count;
    decltype(&gbm_bo_get_fd_for_plane) fd;
    decltype(&gbm_bo_get_stride_for_plane) stride;
    decltype(&gbm_bo_get_offset) offset;
  };

  struct gbm_destination_t final: destination_t {
    gbm_destination_t(gbm_bo *bo, gbm_api_t api, std::shared_ptr<void> device):
        bo(bo),
        api(api),
        device(std::move(device)) {}

    ~gbm_destination_t() {
      for (int fd : layout.fds) {
        if (fd >= 0) {
          close(fd);
        }
      }
      api.destroy(bo);
    }

    gbm_bo *bo;
    gbm_api_t api;
    std::shared_ptr<void> device;  // Keeps device/node alive even through failed GPU cleanup.
  };

  inline std::shared_ptr<destination_t> allocate_destination(gbm_device *device, const std::shared_ptr<void> &lifetime, const gbm_api_t &api, const std::vector<uint64_t> &modifiers, uint32_t format, uint32_t width, uint32_t height, const validator_t &validate) {
    std::string rejection = "no explicit compositor modifiers";
    for (const auto modifier : modifiers) {
      if (modifier == DRM_FORMAT_MOD_INVALID) {
        continue;
      }
      auto *bo = api.create(device, width, height, format, &modifier, 1, GBM_BO_USE_RENDERING);
      if (!bo) {
        rejection = "GBM allocation failed";
        continue;
      }
      auto result = std::make_shared<gbm_destination_t>(bo, api, lifetime);
      auto &l = result->layout;
      try {
        l.width = api.width(bo);
        l.height = api.height(bo);
        l.fourcc = api.format(bo);
        l.modifier = api.modifier(bo);
        const int planes = api.plane_count(bo);
        if (planes <= 0 || planes > 4) {
          throw std::runtime_error("Invalid GBM plane count");
        }
        for (int plane = 0; plane < planes; ++plane) {
          l.fds[plane] = api.fd(bo, plane);
          if (l.fds[plane] < 0) {
            throw std::runtime_error("GBM plane FD export failed");
          }
          l.pitches[plane] = api.stride(bo, plane);
          l.offsets[plane] = api.offset(bo, plane);
        }
        // Enumerate every plane before rejecting unrepresented auxiliary planes.
        // Never guess plane 0, offset 0, linear modifier, or the first DRM node.
        pyrowave_diag::validate_layout(l, true);
        if (l.width != width || l.height != height || l.fourcc != format || l.modifier != modifier) {
          throw std::runtime_error("GBM changed requested layout/modifier");
        }
        validate(l);  // Real Vulkan import on the feedback-selected device, actual FD masks/dedicated allocation.
        return result;
      } catch (const std::exception &e) {
        rejection = e.what();
      }
    }
    throw std::runtime_error("No same-device compositor/GBM/Vulkan capture destination: " + rejection);
  }
}  // namespace pyrowave_wl
