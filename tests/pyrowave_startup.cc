/** @brief Fault injection through the production session factory and GPU cleanup. */
#include "src/platform/linux/pyrowave_capture.h"
#include "src/platform/linux/pyrowave_diagnostic_import.h"
#include "src/platform/linux/pyrowave_encoder_layout.h"
#include "src/process.h"
#include "src/pyrowave_lifetime.h"
#include "src/pyrowave_session.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>

// Access only to seed fake Vulkan handles. Both make_session/KmsSession and
// gpu_t::wait_encode/destructor are compiled from the production sources.
#define private public
#include "src/platform/linux/pyrowave_diagnostic_vulkan.h"
#undef private

using namespace std::chrono_literals;

namespace {
  struct RawBlock {
    uint32_t offset, words;
  };

  struct State {
    VkResult encode_result = VK_SUCCESS;
    bool block_idle = false, idle_entered = false, release_idle = false;
    bool retained_at_idle = false, retained_at_fatal = false;
    std::atomic<int> fatal_calls {0}, destroyed_imports {0}, destroyed_memory {0}, destroyed_owned {0}, destroyed_devices {0};
    std::mutex mutex;
    std::condition_variable cv;
    std::weak_ptr<egl::img_descriptor_t> captured;
    int captured_fd = -1;
    uint64_t wait_timeout = 0;
    pyrowave::Dimensions output;
    std::vector<RawBlock> metadata;

    bool retained() const {
      return !captured.expired() && fcntl(captured_fd, F_GETFD) >= 0 &&
             !destroyed_imports && !destroyed_memory && !destroyed_owned && !destroyed_devices;
    }
  };

  State *state = nullptr;  // Each scenario settles all threads before replacing it.

  void require(bool ok, const char *message) {
    if (!ok) {
      throw std::runtime_error(message);
    }
  }

  class Source final: public platf::kms_diagnostic_source_t {
  public:
    std::shared_ptr<egl::img_descriptor_t> next() override {
      auto image = std::make_shared<egl::img_descriptor_t>();
      image->sd = {};
      std::fill_n(image->sd.fds, 4, -1);
      image->sd.width = 2560;
      image->sd.height = 1440;
      image->sd.fourcc = DRM_FORMAT_XRGB8888;
      image->sd.pitches[0] = 2560 * 4;
      image->sd.fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
      require(image->sd.fds[0] >= 0, "Cannot create harmless captured FD");
      state->captured = image;
      state->captured_fd = image->sd.fds[0];
      return image;
    }

    platf::kms_diagnostic_info_t info() const override {
      return {};
    }

    platf::touch_port_t viewport() const override {
      return {};
    }

    std::pair<int, int> desktop_size() const override {
      return {2560, 1440};
    }
  };

  void import_destroy(VkDevice, VkImage, const VkAllocationCallbacks *) {
    ++state->destroyed_imports;
  }

  void import_free(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {
    ++state->destroyed_memory;
  }

  void check_released(const State &s) {
    require(s.destroyed_imports == 1 && s.destroyed_memory == 1 && s.destroyed_owned == 1 && s.destroyed_devices == 1, "GPU cleanup did not release exactly once");
    require(s.captured.expired() && fcntl(s.captured_fd, F_GETFD) < 0, "Captured FD/lifetime leaked after cleanup");
  }

  void healthy_and_recoverable(pyrowave::Dimensions output, VkResult result) {
    State s;
    state = &s;
    s.encode_result = result;
    pyrowave::CaptureGate gate;
    auto lease = gate.acquire(true);  // Same caller-owned lease as session::start.
    const auto limits = pyrowave::make_limits({1200, 20, 0, 100000, true});
    bool caught = false;
    try {
      auto session = pyrowave::make_session(output, limits);
      require(result == VK_SUCCESS, "Timed-out encode returned a session");
      require(s.output.width == output.width && s.output.height == output.height, "Startup changed selected dimensions");
      session.reset();  // Actual healthy gpu_t destructor.
    } catch (const std::runtime_error &e) {
      caught = true;
      require(result == VK_TIMEOUT && std::string(e.what()).find("wait encode timeline: 2") != std::string::npos, "Unexpected startup failure");
    }
    require(caught == (result == VK_TIMEOUT), "Recoverable timeout did not reach the caller failure handler");
    require(s.wait_timeout == 1'000'000'000ULL && !s.fatal_calls, "Encode deadline or watchdog cancellation changed");
    if (result == VK_TIMEOUT) {
      require(s.retained_at_idle, "Timed-out GPU resources released before idle wait");
    }
    check_released(s);
    lease.reset();
    require(bool(gate.acquire(false)), "Conventional capture cannot reconnect after recoverable failure");
    std::cout << "PASS " << output.width << 'x' << output.height << (caught ? " recoverable timeout" : " healthy startup/teardown") << '\n';
  }

  void stalled_cleanup() {
    State s;
    state = &s;
    s.encode_result = VK_TIMEOUT;
    s.block_idle = true;
    pyrowave::CaptureGate gate;
    auto lease = gate.acquire(true);
    std::atomic<bool> failure_handler {false};
    const auto began = std::chrono::steady_clock::now();
    std::thread startup([&]() {
      try {
        pyrowave::make_session({3840, 2160}, pyrowave::make_limits({1200, 20, 0, 100000, true}));
      } catch (const std::runtime_error &) {
        failure_handler = true;
      }
    });
    bool idle_seen, fatal_seen, handler_blocked, retained, capture_excluded;
    int64_t elapsed_ms;
    {
      std::unique_lock lock(s.mutex);
      idle_seen = s.cv.wait_for(lock, 2s, [&]() {
        return s.idle_entered;
      });
      fatal_seen = s.cv.wait_until(lock, began + 12s, [&]() {
        return s.fatal_calls.load() > 0;
      });
      elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
      handler_blocked = !failure_handler;
      retained = s.retained_at_idle && s.retained_at_fatal;
      capture_excluded = !gate.acquire(false) && !gate.acquire(true);
      s.release_idle = true;  // The mock fatal hook returns only in this test.
    }
    s.cv.notify_all();
    startup.join();  // Always settle the simulated RTSP thread before assertions.
    require(idle_seen && fatal_seen && s.fatal_calls == 1, "Production startup watchdog did not fire through blocked GPU cleanup");
    require(elapsed_ms >= 9000 && elapsed_ms <= 12000, "Production startup watchdog deadline changed");
    require(handler_blocked && retained && capture_excluded, "Cleanup freed resources/FD or capture ownership while GPU was in flight");
    require(failure_handler, "Cleanup completion did not reach the caller failure handler");
    check_released(s);
    lease.reset();
    require(bool(gate.acquire(true)), "Capture ownership leaked after simulated recovery");
    std::cout << "PASS stalled constructor-unwind cleanup: fatal watchdog after " << elapsed_ms << " ms, GPU/FD/lease retained; simulated cleanup then joined\n";
  }
}  // namespace

// Only external device operations and the process-fatal endpoint are mocked.
// No daemon is started, no GPU is used and no signal/kill is delivered.
namespace config {
  video_t video {};
}

namespace proc {
  proc_t proc;
  proc_t::~proc_t() = default;
}  // namespace proc

namespace mail {
  safe::mail_t man;
}

namespace display_device {
  std::string map_output_name(const std::string &) {
    return {};
  }
}  // namespace display_device

namespace platf {
  std::unique_ptr<kms_diagnostic_source_t> make_kms_diagnostic_source(const std::string &, bool live) {
    require(live, "Factory selected diagnostic capture");
    return std::make_unique<Source>();
  }

  void adjust_thread_priority(thread_priority_e) {}

  std::unique_ptr<high_precision_timer> create_high_precision_timer() {
    return {};
  }
}  // namespace platf

boost::log::sources::severity_logger<int> info, warning, error, fatal;

namespace logging {
  void log_flush() {}
}  // namespace logging

namespace lifetime {
  void debug_trap() {
    std::lock_guard lock(state->mutex);
    state->retained_at_fatal = state->retained();
    ++state->fatal_calls;
    state->cv.notify_all();
  }
}  // namespace lifetime

extern "C" void mock_init(pyrowave_diag::gpu_t *, const platf::kms_diagnostic_info_t *, bool, int, int) asm("__wrap__ZN13pyrowave_diag5gpu_t4initEPKN5platf21kms_diagnostic_info_tEbii");

extern "C" void mock_init(pyrowave_diag::gpu_t *gpu, const platf::kms_diagnostic_info_t *, bool diagnostic, int width, int height) {
  require(!diagnostic, "Startup allocated a diagnostic decoder");
  state->output = {width, height};
  state->metadata.resize(pyrowave::raw_block_count(state->output));
  state->metadata[0] = {0, 2};
  gpu->device = reinterpret_cast<VkDevice>(uintptr_t(1));
  gpu->owned_image = reinterpret_cast<VkImage>(uintptr_t(2));
  gpu->width = 2560;
  gpu->height = 1440;
  gpu->format = VK_FORMAT_B8G8R8A8_UNORM;
  gpu->diagnostic = false;
}

extern "C" void mock_import(pyrowave_diag::gpu_t *, const pyrowave_diag::layout_t &) asm("__wrap__ZN13pyrowave_diag5gpu_t6importERKNS_8layout_tE");

extern "C" void mock_import(pyrowave_diag::gpu_t *gpu, const pyrowave_diag::layout_t &) {
  pyrowave_diag::import_api_t api {};
  api.destroy_image = import_destroy;
  api.free_memory = import_free;
  gpu->imported = std::make_unique<pyrowave_diag::dma_buf_image_t>(gpu->device, api);
  gpu->imported->image = reinterpret_cast<VkImage>(uintptr_t(3));
  gpu->imported->memory = reinterpret_cast<VkDeviceMemory>(uintptr_t(4));
}

extern "C" void mock_snapshot(pyrowave_diag::gpu_t *, const std::vector<uint8_t> *) asm("__wrap__ZN13pyrowave_diag5gpu_t8snapshotEPKSt6vectorIhSaIhEE");

extern "C" void mock_snapshot(pyrowave_diag::gpu_t *, const std::vector<uint8_t> *) {}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo *, uint64_t timeout) {
  state->wait_timeout = timeout;
  return state->encode_result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkDeviceWaitIdle(VkDevice) {
  std::unique_lock lock(state->mutex);
  state->idle_entered = true;
  state->retained_at_idle = state->retained();
  state->cv.notify_all();
  state->cv.wait(lock, []() {
    return !state->block_idle || state->release_idle;
  });
  return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyDevice(VkDevice, const VkAllocationCallbacks *) {
  ++state->destroyed_devices;
}

extern "C" VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *) {
  ++state->destroyed_owned;
}

extern "C" pyrowave_result __wrap_pyrowave_encoder_encode_gpu_scaled_synchronous(pyrowave_encoder, const pyrowave_gpu_sync_operation *, const pyrowave_gpu_sync_operation *, const pyrowave_scaled_encode_info *, const pyrowave_rate_control *) {
  return PYROWAVE_SUCCESS;
}

extern "C" pyrowave_result __wrap_pyrowave_encoder_compute_num_packets(pyrowave_encoder, size_t, size_t *count) {
  *count = 1;
  return PYROWAVE_SUCCESS;
}

extern "C" pyrowave_result __wrap_pyrowave_encoder_get_mapped_raw_bitstream(pyrowave_encoder, const void **raw, size_t *raw_size, const void **metadata, size_t *metadata_size) {
  static const uint32_t payload[2] {};
  *raw = payload;
  *raw_size = sizeof(payload);
  *metadata = state->metadata.data();
  *metadata_size = state->metadata.size() * sizeof(RawBlock);
  return PYROWAVE_SUCCESS;
}

extern "C" pyrowave_result __wrap_pyrowave_encoder_packetize(pyrowave_encoder, pyrowave_packet *packets, size_t, size_t *count, void *bytes, size_t size) {
  require(*count == 1 && size == 16, "Mock codec bounds changed");
  std::memset(bytes, 0, size);
  packets[0] = {0, size};
  return PYROWAVE_SUCCESS;
}

int main() {
  try {
    for (auto output : {pyrowave::Dimensions {1920, 1080}, pyrowave::Dimensions {3840, 2160}}) {
      healthy_and_recoverable(output, VK_SUCCESS);
      healthy_and_recoverable(output, VK_TIMEOUT);
    }
    stalled_cleanup();
    healthy_and_recoverable({3840, 2160}, VK_SUCCESS);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
