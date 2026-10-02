/** @brief CPU contracts shared by the diagnostic and its focused tests. */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <drm_fourcc.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace pyrowave_diag {
  constexpr int output_width = 1920, output_height = 1080, fps = 60;
  using planes_t = std::array<std::vector<uint8_t>, 3>;

  struct layout_t {
    uint32_t width {}, height {}, fourcc {};
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    std::array<int, 4> fds {-1, -1, -1, -1};
    std::array<uint32_t, 4> pitches {}, offsets {};
  };

  inline VkFormat validate_layout(const layout_t &layout) {
    // X channels are ignored. Alpha-bearing packed 10-bit SDR is accepted only
    // with a per-frame opaque-alpha check on the owned snapshot before encode.
    VkFormat format;
    switch (layout.fourcc) {
      case DRM_FORMAT_XRGB8888:
        format = VK_FORMAT_B8G8R8A8_UNORM;
        break;
      case DRM_FORMAT_XBGR8888:
        format = VK_FORMAT_R8G8B8A8_UNORM;
        break;
      case DRM_FORMAT_ABGR2101010:
      case DRM_FORMAT_XBGR2101010:
        format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        break;
      case DRM_FORMAT_ARGB2101010:
      case DRM_FORMAT_XRGB2101010:
        format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        break;
      default:
        throw std::runtime_error("Unsupported fourcc (only opaque 8-bit or packed 10-bit RGB SDR)");
    }
    if (!layout.width || !layout.height || layout.width > 8192 || layout.height > 8192 || layout.fds[0] < 0 || layout.pitches[0] < uint64_t(layout.width) * 4) {
      throw std::runtime_error("Invalid framebuffer extent/FD/pitch");
    }
    if (layout.modifier == DRM_FORMAT_MOD_INVALID) {
      throw std::runtime_error("Framebuffer has no explicit modifier; refusing to guess linear");
    }
    for (int i = 1; i < 4; ++i) {
      if (layout.fds[i] >= 0) {
        throw std::runtime_error("Multiple framebuffer memory planes are not supported");
      }
    }
    return format;
  }

  inline bool packed_10bit(VkFormat format) {
    return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
  }

  inline bool requires_opaque_alpha(uint32_t fourcc) {
    return fourcc == DRM_FORMAT_ABGR2101010 || fourcc == DRM_FORMAT_ARGB2101010;
  }

  inline void validate_opaque_alpha(const std::vector<uint8_t> &pixels) {
    if (pixels.empty() || pixels.size() % 4) {
      throw std::runtime_error("Invalid packed alpha buffer");
    }
    for (size_t i = 0; i < pixels.size(); i += 4) {
      uint32_t packed;
      std::memcpy(&packed, pixels.data() + i, sizeof(packed));
      if ((packed >> 30) != 3) {
        throw std::runtime_error("Nonopaque primary-plane alpha: composition is unimplemented");
      }
    }
  }

  inline int bounded_number(const std::string &value, int low, int high) {
    size_t end = 0;
    int number = std::stoi(value, &end);
    if (end != value.size() || number < low || number > high) {
      throw std::runtime_error("CLI integer outside allowed bounds");
    }
    return number;
  }

  inline planes_t make_planes() {
    return {std::vector<uint8_t>(output_width * output_height), std::vector<uint8_t>(output_width * output_height / 4), std::vector<uint8_t>(output_width * output_height / 4)};
  }

  inline std::array<double, 3> bt709(double r, double g, double b) {
    // Full range, center-sited 4:2:0; 128/255 midpoint, matching the pinned scaler.
    return {0.2126 * r + 0.7152 * g + 0.0722 * b, -0.114572 * r - 0.385428 * g + 0.5 * b + 128.0, 0.5 * r - 0.454153 * g - 0.0458471 * b + 128.0};
  }

  inline uint8_t quantize(double x) {
    return uint8_t(std::clamp(std::lround(x), 0L, 255L));
  }

  // Approximate independent reference: gamma-domain bilinear scaling, BT.709,
  // 2x2 chroma average. It does not emulate half-float shader rounding or codec loss.
  inline planes_t reference(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height, VkFormat format) {
    if (!width || !height || rgba.size() != uint64_t(width) * height * 4 || (!packed_10bit(format) && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM)) {
      throw std::runtime_error("Invalid reference RGB buffer");
    }
    auto result = make_planes();
    auto sample = [&](double x, double y) {
      x = std::clamp(x, 0.0, double(width - 1));
      y = std::clamp(y, 0.0, double(height - 1));
      int x0 = int(x), y0 = int(y);
      int x1 = std::min(x0 + 1, int(width - 1)), y1 = std::min(y0 + 1, int(height - 1));
      std::array<double, 3> rgb {};
      for (int c = 0; c < 3; ++c) {
        int channel = format == VK_FORMAT_B8G8R8A8_UNORM ? 2 - c : c;
        auto pixel = [&](int px, int py) {
          auto offset = (size_t(py) * width + px) * 4;
          if (packed_10bit(format)) {
            uint32_t packed;
            std::memcpy(&packed, rgba.data() + offset, sizeof(packed));
            int shift = c * 10;
            if (format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
              shift = (2 - c) * 10;
            }
            return double((packed >> shift) & 1023) * 255 / 1023;
          }
          return double(rgba[offset + channel]);
        };
        rgb[c] = (1 - (y - y0)) * ((1 - (x - x0)) * pixel(x0, y0) + (x - x0) * pixel(x1, y0)) +
                 (y - y0) * ((1 - (x - x0)) * pixel(x0, y1) + (x - x0) * pixel(x1, y1));
      }
      return bt709(rgb[0], rgb[1], rgb[2]);
    };
    for (int y = 0; y < output_height; y += 2) {
      for (int x = 0; x < output_width; x += 2) {
        double cb = 0, cr = 0;
        for (int dy = 0; dy < 2; ++dy) {
          for (int dx = 0; dx < 2; ++dx) {
            auto v = sample((x + dx + 0.5) * width / output_width - 0.5, (y + dy + 0.5) * height / output_height - 0.5);
            result[0][size_t(y + dy) * output_width + x + dx] = quantize(v[0]);
            cb += v[1] * 0.25;
            cr += v[2] * 0.25;
          }
        }
        auto at = size_t(y / 2) * (output_width / 2) + x / 2;
        result[1][at] = quantize(cb);
        result[2][at] = quantize(cr);
      }
    }
    return result;
  }

  struct error_t {
    double mae {}, mse {};
    int maximum {};
  };

  inline error_t compare(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    if (a.empty() || a.size() != b.size()) {
      throw std::runtime_error("Mismatched reference and decoded plane lengths");
    }
    uint64_t absolute = 0, squared = 0;
    int maximum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
      int d = std::abs(int(a[i]) - int(b[i]));
      maximum = std::max(maximum, d);
      absolute += d;
      squared += d * d;
    }
    return {double(absolute) / a.size(), double(squared) / a.size(), maximum};
  }
}  // namespace pyrowave_diag
