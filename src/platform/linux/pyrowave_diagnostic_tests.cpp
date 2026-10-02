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
}  // namespace

namespace pyrowave_diag {
  void test_live_import_contracts();
}

int main() {
  try {
    test_live_import_contracts();
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
