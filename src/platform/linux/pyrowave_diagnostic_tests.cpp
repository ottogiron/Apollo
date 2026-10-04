/** @brief Focused format, color and error contracts; no GPU/display/privileges. */
#include "pyrowave_diagnostic_fd.h"
#include "pyrowave_diagnostic_support.h"

#include <functional>
#include <iostream>

namespace {
  using namespace pyrowave_diag;

  void require(bool value, const char *message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }

  void rejects(const std::function<void()> &run) {
    try {
      run();
    } catch (const std::exception &) {
      return;
    }
    throw std::runtime_error("Expected input rejection");
  }

  // The shader's per-texel predicate, compiled here from the same source text
  // that glslang compiles into pyrowave_alpha.comp.
  namespace alpha_shader {
    struct vec4 {
      float r, g, b, a;
    };

    using uint = uint32_t;
#include "pyrowave_alpha_texel.glsl"
  }  // namespace alpha_shader

  // What an unswizzled Vulkan view of `format` returns for one little-endian
  // packed texel: the snapshot's own channel order, UNORM-normalized.
  alpha_shader::vec4 fetch(VkFormat format, uint32_t texel) {
    const auto unorm = [](uint32_t value, uint32_t bits) {
      return float(value) / float((1u << bits) - 1);
    };
    switch (format) {
      case VK_FORMAT_B8G8R8A8_UNORM:
        return {unorm((texel >> 16) & 255, 8), unorm((texel >> 8) & 255, 8), unorm(texel & 255, 8), unorm(texel >> 24, 8)};
      case VK_FORMAT_R8G8B8A8_UNORM:
        return {unorm(texel & 255, 8), unorm((texel >> 8) & 255, 8), unorm((texel >> 16) & 255, 8), unorm(texel >> 24, 8)};
      case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return {unorm((texel >> 20) & 1023, 10), unorm((texel >> 10) & 1023, 10), unorm(texel & 1023, 10), unorm(texel >> 30, 2)};
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return {unorm(texel & 1023, 10), unorm((texel >> 10) & 1023, 10), unorm((texel >> 20) & 1023, 10), unorm(texel >> 30, 2)};
      default:
        throw std::runtime_error("Alpha model received a format without an alpha census");
    }
  }

  // The shader's main(): opaque texels are silent, the rest are counted.
  alpha_counts_t census(VkFormat format, const std::vector<uint32_t> &texels) {
    alpha_counts_t counts {texels.size(), 0, 0};
    for (const auto texel : texels) {
      const auto kind = alpha_shader::pyrowave_alpha_texel_class(fetch(format, texel));
      if (kind == alpha_shader::pyrowave_alpha_transparent_black) {
        ++counts.transparent_black;
      } else if (kind == alpha_shader::pyrowave_alpha_other) {
        ++counts.other_nonopaque;
      } else {
        require(kind == alpha_shader::pyrowave_alpha_opaque, "Shader predicate returned an unknown class");
      }
    }
    return counts;
  }

  void test_alpha_classification() {
    constexpr uint32_t n = 64;
    for (auto fourcc : {DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB2101010, DRM_FORMAT_ABGR2101010}) {
      layout_t layout;
      layout.width = 1920;
      layout.height = 1080;
      layout.fourcc = fourcc;
      layout.modifier = DRM_FORMAT_MOD_LINEAR;
      layout.fds[0] = 10;
      layout.pitches[0] = 7680;
      require(requires_opaque_alpha(fourcc), "Alpha-bearing fourcc skipped the census");
      const auto format = validate_layout(layout, true);
      const bool packed = packed_10bit(format);
      // Alpha is the top channel of every accepted alpha-bearing fourcc.
      const uint32_t alpha_shift = packed ? 30 : 24, alpha_max = packed ? 3 : 255, color_mask = (1u << alpha_shift) - 1;
      const uint32_t opaque_black = alpha_max << alpha_shift, opaque_white = opaque_black | color_mask;

      // Every alpha level over black: only the exact endpoints are special.
      for (uint32_t alpha = 0; alpha <= alpha_max; ++alpha) {
        const auto counts = census(format, std::vector<uint32_t>(n, alpha << alpha_shift));
        const auto kind = classify_alpha(counts);
        if (alpha == alpha_max) {
          require(kind == alpha_class_t::opaque && !counts.transparent_black && !counts.other_nonopaque, "Opaque black must be accepted without counting");
        } else if (!alpha) {
          require(kind == alpha_class_t::transparent_black && counts.transparent_black == n && !counts.other_nonopaque, "Wholly transparent black was not classified exactly");
        } else {
          require(kind == alpha_class_t::nonopaque && !counts.transparent_black && counts.other_nonopaque == n, "Uniformly fractional-alpha black was mistaken for an unpainted or opaque frame");
        }
      }
      require(classify_alpha(census(format, std::vector<uint32_t>(n, opaque_white))) == alpha_class_t::opaque, "Opaque color rejected");

      // Alpha zero with the smallest step of any single color bit is not black.
      for (uint32_t bit = 0; bit < alpha_shift; ++bit) {
        const auto counts = census(format, std::vector<uint32_t>(n, 1u << bit));
        require(classify_alpha(counts) == alpha_class_t::nonopaque && !counts.transparent_black && counts.other_nonopaque == n, "Colored alpha-zero frame was mistaken for transparent black");
        auto one = std::vector<uint32_t>(n, 0);
        one[bit % n] = 1u << bit;
        const auto mixed = census(format, one);
        require(classify_alpha(mixed) == alpha_class_t::nonopaque && mixed.transparent_black == n - 1 && mixed.other_nonopaque == 1, "One colored alpha-zero texel hidden in a transparent frame");
      }

      // Any mixture is rejected, whichever texel differs.
      for (uint32_t at : {0u, n / 2, n - 1}) {
        auto frame = std::vector<uint32_t>(n, 0);
        frame[at] = opaque_black;
        auto counts = census(format, frame);
        require(classify_alpha(counts) == alpha_class_t::nonopaque && counts.transparent_black == n - 1 && !counts.other_nonopaque, "One painted texel in a transparent frame must not be retried");
        frame.assign(n, opaque_white);
        frame[at] = 0;
        counts = census(format, frame);
        require(classify_alpha(counts) == alpha_class_t::nonopaque && counts.transparent_black == 1 && !counts.other_nonopaque, "One transparent texel in an opaque frame accepted");
        frame[at] = (alpha_max - 1) << alpha_shift | color_mask;
        counts = census(format, frame);
        require(classify_alpha(counts) == alpha_class_t::nonopaque && !counts.transparent_black && counts.other_nonopaque == 1, "One fractional-alpha texel in an opaque frame accepted");
      }
    }
    // X-channel formats carry no alpha contract; their undefined byte is never read.
    for (auto fourcc : {DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010}) {
      require(!requires_opaque_alpha(fourcc), "X-channel fourcc gained an alpha census");
    }

    // Host-side reading of the two counters, including a census that cannot be true.
    constexpr uint64_t native = uint64_t(2560) * 1440, largest = uint64_t(8192) * 8192;
    require(classify_alpha({native, 0, 0}) == alpha_class_t::opaque, "Opaque census");
    require(classify_alpha({native, uint32_t(native), 0}) == alpha_class_t::transparent_black, "Transparent-black census");
    require(classify_alpha({largest, uint32_t(largest), 0}) == alpha_class_t::transparent_black, "Largest accepted extent overflowed the census");
    for (auto counts : {alpha_counts_t {native, uint32_t(native) - 1, 0}, alpha_counts_t {native, 1, 0}, alpha_counts_t {native, 0, 1}, alpha_counts_t {native, 0, uint32_t(native)}, alpha_counts_t {native, uint32_t(native) - 1, 1}, alpha_counts_t {native, uint32_t(native), 1}, alpha_counts_t {native, UINT32_MAX, UINT32_MAX}, alpha_counts_t {0, 0, 0}}) {
      require(classify_alpha(counts) == alpha_class_t::nonopaque, "Partial, fractional, colored, inconsistent or empty census must fail closed");
    }
    require(describe_alpha({native, uint32_t(native), 0}) == "alpha=transparent-black (3686400 transparent-black, 0 other nonopaque of 3686400 texels)", "Transparent-black diagnostic text");
    require(describe_alpha({native, 5, 7}) == "alpha=nonopaque (5 transparent-black, 7 other nonopaque of 3686400 texels)", "Nonopaque diagnostic text");
    require(describe_alpha({native, 0, 0}) == "alpha=opaque (0 transparent-black, 0 other nonopaque of 3686400 texels)", "Opaque diagnostic text");
    std::cout << "alpha census: shader texel predicate over AR24/AB24/AR30/AB30, exact transparent-black versus fractional/colored/partial, host classification passed\n";
  }
}  // namespace

namespace pyrowave_diag {
  void test_live_import_contracts();
}

int main() {
  try {
    test_live_import_contracts();
    test_alpha_classification();
    int pipe_fds[2];
    require(!pipe(pipe_fds), "Create lifetime test pipe");
    int duplicate = -1;
    {
      import_fd_t failed_import(pipe_fds[0]);
      duplicate = failed_import.fd;
      require(fcntl(duplicate, F_GETFD) & FD_CLOEXEC, "Import duplicate must be CLOEXEC");
    }
    require(fcntl(duplicate, F_GETFD) == -1, "Failed pre-allocation import must close duplicate");
    require(fcntl(pipe_fds[0], F_GETFD) >= 0, "Capture FD survives duplicate cleanup");
    {
      import_fd_t consumed_import(pipe_fds[0]);
      duplicate = consumed_import.fd;
      close(duplicate);  // Successful vkAllocateMemory consumes the duplicate.
      consumed_import.consumed();  // Native caller commits consumption immediately.
      int other = open("/dev/null", O_RDONLY | O_CLOEXEC);
      require(other >= 0, "Create reused FD slot");
      require(dup2(other, duplicate) == duplicate, "Reuse consumed FD slot");
      if (other != duplicate) {
        close(other);
      }
    }
    require(fcntl(duplicate, F_GETFD) >= 0, "Error cleanup must preserve reused unrelated FD");
    close(duplicate);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    rejects([] {
      import_fd_t invalid(-1);
    });
    layout_t valid;
    valid.width = 1920;
    valid.height = 1080;
    valid.fourcc = DRM_FORMAT_XRGB8888;
    valid.modifier = DRM_FORMAT_MOD_LINEAR;
    valid.fds[0] = 10;
    valid.pitches[0] = 8192;
    valid.offsets[0] = 4096;
    require(validate_layout(valid) == VK_FORMAT_B8G8R8A8_UNORM, "XRGB channel order");
    valid.fourcc = DRM_FORMAT_XBGR8888;
    require(validate_layout(valid) == VK_FORMAT_R8G8B8A8_UNORM, "XBGR channel order");
    for (auto fourcc : {DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_NV12}) {
      auto bad = valid;
      bad.fourcc = fourcc;
      rejects([&] {
        validate_layout(bad);
      });
    }
    auto packed_layout = valid;
    packed_layout.fourcc = DRM_FORMAT_ABGR2101010;
    require(validate_layout(packed_layout) == VK_FORMAT_A2B10G10R10_UNORM_PACK32, "AB30 mapping");
    packed_layout.fourcc = DRM_FORMAT_ARGB2101010;
    require(validate_layout(packed_layout) == VK_FORMAT_A2R10G10B10_UNORM_PACK32, "AR30 mapping");
    require(requires_opaque_alpha(DRM_FORMAT_ABGR2101010) && !requires_opaque_alpha(DRM_FORMAT_XBGR2101010), "Packed alpha policy");
    uint32_t red10 = (3u << 30) | 1023;
    std::vector<uint8_t> packed_red(4);
    std::memcpy(packed_red.data(), &red10, 4);
    validate_opaque_alpha(packed_red);
    auto red10_planes = reference(packed_red, 1, 1, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    require(red10_planes[0][0] == 54 && red10_planes[1][0] == 99 && red10_planes[2][0] == 255, "10-bit red reference precision/channel order");
    red10 &= ~(3u << 30);
    std::memcpy(packed_red.data(), &red10, 4);
    rejects([&] {
      validate_opaque_alpha(packed_red);
    });
    auto bad = valid;
    bad.modifier = DRM_FORMAT_MOD_INVALID;
    rejects([&] {
      validate_layout(bad);
    });
    bad = valid;
    bad.fds[1] = 11;
    rejects([&] {
      validate_layout(bad);
    });
    bad = valid;
    bad.fds[0] = -1;
    rejects([&] {
      validate_layout(bad);
    });
    bad = valid;
    bad.pitches[0] = 7679;
    rejects([&] {
      validate_layout(bad);
    });
    bad = valid;
    bad.width = UINT32_MAX;
    rejects([&] {
      validate_layout(bad);
    });
    rejects([] {
      bounded_number("0", 1, 3600);
    });
    rejects([] {
      bounded_number("12junk", 1, 3600);
    });
    rejects([] {
      compare({1}, {});
    });
    auto diff = compare({0, 255}, {2, 250});
    require(diff.maximum == 5 && diff.mae == 3.5 && diff.mse == 14.5, "Error arithmetic");
    auto black = bt709(0, 0, 0), white = bt709(255, 255, 255);
    require(quantize(black[0]) == 0 && quantize(white[0]) == 255, "Full range endpoints");
    require(quantize(black[1]) == 128 && quantize(white[2]) == 128, "Neutral chroma midpoint");
    // An independent primary reference checks channel order and ignores X bytes.
    auto red = reference({0, 0, 255, 0}, 1, 1, VK_FORMAT_B8G8R8A8_UNORM);
    auto red_x = reference({255, 0, 0, 99}, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    require(red == red_x, "X-channel must not change SDR color");
    require(red[0][0] == 54 && red[1][0] == 99 && red[2][0] == 255, "Red BT.709 mapping");
    rejects([] {
      reference({}, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    });
    std::cout << "format, alpha rejection, bounds, CLI, BT.709 full range and reference error contracts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
