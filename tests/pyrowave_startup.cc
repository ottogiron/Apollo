/** @brief Fault injection through the production session factory and GPU cleanup. */
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/linux/pyrowave_capture.h"
#include "src/platform/linux/pyrowave_diagnostic_import.h"
#include "src/platform/linux/pyrowave_encoder_layout.h"
#include "src/process.h"
#include "src/pyrowave_lifetime.h"
#include "src/pyrowave_session.h"

#include <atomic>
#include <boost/make_shared.hpp>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <sstream>
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
    std::vector<size_t> block_sizes {8};
    std::vector<uint32_t> raw;
    size_t count_target = 0, packetize_target = 0, packetize_calls = 0;
    bool truncated_metadata = false, invalid_offset = false;
    std::function<void()> on_sleep;

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
      return {0, 0, 2560, 1440};
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

  void packetization(pyrowave::Dimensions output, State &s, const pyrowave::Limits &limits, std::string_view failure = {}) {
    state = &s;
    pyrowave::CaptureGate gate;
    auto lease = gate.acquire(true);
    bool caught = false;
    try {
      auto session = pyrowave::make_session(output, limits);
      require(failure.empty(), "Invalid codec frame returned a ready session");
      require(s.packetize_calls == 1, "Startup skipped production packetization");
    } catch (const std::runtime_error &e) {
      caught = true;
      require(!failure.empty() && std::string_view(e.what()).find(failure) != std::string_view::npos, "Unexpected packetization failure");
    }
    require(caught == !failure.empty(), "Packetization failure did not reach startup caller");
    const size_t target = std::min(size_t(65536), limits.frame_bytes - pyrowave::header_size - 4);
    require(s.count_target == target, "Codec packing target ignores available frame bytes");
    if (s.packetize_calls) {
      require(s.packetize_target == target, "Count and packetize use different packing targets");
    }
    require(!s.fatal_calls, "Recoverable packetization invoked fatal watchdog");
    check_released(s);
    lease.reset();
    require(bool(gate.acquire(true)), "Packetization failure leaked exclusive capture ownership");
    require(bool(gate.acquire(false)), "Packetization failure blocked conventional capture");
    std::cout << "PASS " << output.width << 'x' << output.height << " production packetization: " << (failure.empty() ? "complete block >1200 bytes accepted" : failure) << '\n';
  }

  void codec_record_bounds(pyrowave::Dimensions output) {
    const auto low = pyrowave::make_limits({1024, 80, 2, 10000, true});
    require(low.frame_bytes < 65536, "Low bitrate no longer supports smaller frame budgets");
    State large;
    large.block_sizes = {2048};  // One complete pinned-codec block, not an RTP fragment.
    packetization(output, large, low);

    State over_budget;
    over_budget.block_sizes = {16380};  // Maximum pinned payload_words (4095), still below the record cap.
    packetization(output, over_budget, low, "Actual codec bytes exceed frame budget");
    require(!over_budget.packetize_calls, "Oversized raw frame reached the unchecked codec copy");

    State over_record;
    // Inject a misbehaving packetizer that coalesces five complete blocks into
    // one >64 KiB record. Total bytes fit the high transport budget.
    over_record.block_sizes = {13108, 13108, 13108, 13108, 13108};
    packetization(output, over_record, pyrowave::make_limits({1392, 1, 0, 200000, false}), "codec record exceeds the 64 KiB limit");
    require(over_record.packetize_calls == 1, "Record-cap regression did not reach production output validation");

    State metadata;
    metadata.truncated_metadata = true;
    packetization(output, metadata, low, "Unexpected codec metadata layout");
    require(!metadata.packetize_calls, "Inexact metadata extent reached codec copy");

    State offset;
    offset.invalid_offset = true;
    packetization(output, offset, low, "Codec block outside mapped bitstream");
    require(!offset.packetize_calls, "Invalid raw range reached codec copy");
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

  void live_counters() {
    State s;
    state = &s;
    s.block_sizes = {2048};
    std::ostringstream messages;
    auto sink = boost::make_shared<boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>>();
    sink->locked_backend()->add_stream(boost::shared_ptr<std::ostream>(&messages, [](std::ostream *) {
    }));
    sink->set_formatter([](const boost::log::record_view &record, boost::log::formatting_ostream &out) {
      out << record.attribute_values()["Message"].extract<std::string>().get();
    });
    boost::log::core::get()->add_sink(sink);
    auto remove_sink = util::fail_guard([&]() {
      boost::log::core::get()->remove_sink(sink);
    });
    const auto limits = pyrowave::make_limits({1024, 80, 2, 10000, true});
    auto session = pyrowave::make_session({1920, 1080}, limits);
    mail::man = std::make_shared<safe::mail_raw_t>();
    auto local_mail = std::make_shared<safe::mail_raw_t>();
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto shutdown = local_mail->event<bool>(mail::shutdown);
    int sleeps = 0;
    s.on_sleep = [&]() {
      ++sleeps;
      if (sleeps == 3) {
        // Two queued tickets create one backpressure drop. Simulate completed
        // broadcaster sends, releasing both tickets before a third encode.
        while (packets->peek()) {
          auto packet = packets->pop(0ms);
          session->record_emitted(packet->data_size());
        }
        s.raw.resize(4095);
        s.metadata[0].words = 4095;  // Valid mapped extent, beyond frame budget.
      }
      if (sleeps == 4) {
        shutdown->raise(true);
      }
    };
    session->run(local_mail, &s);
    session->drain(&s);
    sink->flush();
    const auto text = messages.str();
    require(sleeps == 4 && text.find("host counters final") != std::string::npos, "Live counters did not finish after drain");
    require(text.find("frames emitted=2 capture/encode attempts=3 budget drops=1 backpressure drops=1") != std::string::npos, "Counters conflated queued/emitted frames, startup validation, or drops");
    require(text.find("payload bytes=4184 payload Mbps=") != std::string::npos && text.find("requested FPS=60, host emission FPS=") != std::string::npos && text.find("capture+encode ms mean=") != std::string::npos, "Live byte/rate/timing evidence missing");
    require(text.find("video wire budget 10000 Kbps") != std::string::npos && text.find("codec target ") != std::string::npos, "Startup hides wire budget or codec target");
    session.reset();
    require(s.destroyed_imports == 4 && s.destroyed_memory == 4 && s.destroyed_owned == 1 && s.destroyed_devices == 1 && s.captured.expired(), "Live counters changed cleanup ownership");
    mail::man.reset();
    std::cout << "PASS production run/drain counters: 2 emitted, 3 capture/encode attempts, 1 budget drop, 1 backpressure drop, 4184 payload bytes (simulated sends; no live FPS claim)\n";
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
    class Timer final: public high_precision_timer {
    public:
      void sleep_for(const std::chrono::nanoseconds &) override {
        state->on_sleep();
      }

      operator bool() override {
        return true;
      }
    };

    return state->on_sleep ? std::make_unique<Timer>() : nullptr;
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
  for (size_t i = 0; i < state->block_sizes.size(); ++i) {
    const size_t words = state->block_sizes[i] / 4;
    require(words >= 2 && words <= 4095 && words * 4 == state->block_sizes[i], "Invalid pinned codec block fixture");
    state->metadata[i] = {uint32_t(state->raw.size()), uint32_t(words)};
    const size_t offset = state->raw.size();
    state->raw.resize(offset + words);
    // Pinned BitstreamHeader: ballot, 12-bit payload_words, sequence=0,
    // extended=0, quant_code=0, and 24-bit block_index. Payload stays opaque.
    state->raw[offset] = uint32_t(words) << 16;
    state->raw[offset + 1] = uint32_t(i) << 8;
  }
  if (state->invalid_offset) {
    state->metadata[0].offset = state->raw.size();
  }
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

extern "C" pyrowave_result __wrap_pyrowave_encoder_compute_num_packets(pyrowave_encoder, size_t target, size_t *count) {
  state->count_target = target;
  // Fixtures request one complete record; the over-record case deliberately
  // simulates a packetizer violating the host's independent record cap.
  *count = 1;
  return PYROWAVE_SUCCESS;
}

extern "C" pyrowave_result __wrap_pyrowave_encoder_get_mapped_raw_bitstream(pyrowave_encoder, const void **raw, size_t *raw_size, const void **metadata, size_t *metadata_size) {
  *raw = state->raw.data();
  *raw_size = state->raw.size() * sizeof(uint32_t);
  *metadata = state->metadata.data();
  *metadata_size = state->metadata.size() * sizeof(RawBlock) - (state->truncated_metadata ? 1 : 0);
  return PYROWAVE_SUCCESS;
}

extern "C" pyrowave_result __wrap_pyrowave_encoder_packetize(pyrowave_encoder, pyrowave_packet *packets, size_t target, size_t *count, void *bytes, size_t size) {
  ++state->packetize_calls;
  state->packetize_target = target;
  require(*count == 1 && size == 8 + state->raw.size() * sizeof(uint32_t), "Production codec copy buffer differs from mapped extent");
  // Pinned BitstreamSequenceHeader followed by all whole codec blocks.
  const uint32_t sequence[] {uint32_t(state->output.width - 1) | (uint32_t(state->output.height - 1) << 14) | (1u << 31), uint32_t(state->block_sizes.size())};
  std::memcpy(bytes, sequence, sizeof(sequence));
  std::memcpy(static_cast<uint8_t *>(bytes) + sizeof(sequence), state->raw.data(), state->raw.size() * sizeof(uint32_t));
  packets[0] = {0, size};
  return PYROWAVE_SUCCESS;
}

int main() {
  try {
    for (auto output : {pyrowave::Dimensions {1920, 1080}, pyrowave::Dimensions {2560, 1440}, pyrowave::Dimensions {3840, 2160}}) {
      healthy_and_recoverable(output, VK_SUCCESS);
      healthy_and_recoverable(output, VK_TIMEOUT);
      codec_record_bounds(output);
    }
    stalled_cleanup();
    healthy_and_recoverable({3840, 2160}, VK_SUCCESS);
    live_counters();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
