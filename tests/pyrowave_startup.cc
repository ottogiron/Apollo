/** @brief Fault injection through the production session factory and GPU cleanup. */
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/linux/pyrowave_capture.h"
#include "src/platform/linux/pyrowave_diagnostic_import.h"
#include "src/platform/linux/pyrowave_encoder_layout.h"
#include "src/process.h"
#include "src/pyrowave_lifetime.h"
#include "src/pyrowave_session.h"
#include "src/rtsp_budget.h"

#include <atomic>
#include <boost/make_shared.hpp>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

// Access only to seed fake Vulkan handles. Both make_session/KmsSession and
// gpu_t::wait_encode/destructor are compiled from the production sources.
#define private public
#include "src/platform/linux/pyrowave_diagnostic_vulkan.h"
#undef private

// The module embedded in the host, generated next to its spirv-val input.
#include "pyrowave_alpha_spv.h"

using namespace std::chrono_literals;

namespace {
  struct RawBlock {
    uint32_t offset, words;
  };

  struct State {
    VkResult encode_result = VK_SUCCESS;
    bool block_idle = false, idle_entered = false, release_idle = false;
    bool retained_at_idle = false, retained_at_fatal = false, captured_at_idle = false;
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
    size_t rate_target = 0;
    bool separate_records = false;
    bool compositor = false, snapshot_done = false, encode_done = false;
    int snapshot_completions = 0, encode_completions = 0, source_destroyed = 0;
    int foreign_acquires = 0, foreign_releases = 0, image_copies = 0;
    uint32_t copy_signals = 0;
    VkResult copy_result = VK_SUCCESS;
    bool retained_at_copy_wait = false;
    bool truncated_metadata = false, invalid_offset = false;
    std::function<void()> on_sleep;
    std::vector<uint32_t> fourccs {DRM_FORMAT_XRGB8888};
    size_t captures = 0;
    int snapshots_created = 0, alpha_checks = 0;
    bool encoder_destroyed = false, codec_views_drained = false;
    std::vector<VkFormat> encoded_formats;
    // Scripted GPU census, one entry per alpha check; later frames are opaque.
    std::vector<pyrowave_diag::alpha_counts_t> alpha_script;
    bool alpha_done = false;
    size_t requests = 0, encodes_at_capture = 0;
    size_t failing_request = 0;  // 1-based request that the compositor never answers.
    bool ready_at_bound = false;  // A bounded request becomes ready only as its bound expires.
    std::vector<std::chrono::milliseconds> bounded_waits;
    // Mapped result block and simulated shader output for the real census path.
    uint32_t alpha_result[2] {0xdeadbeef, 0xdeadbeef}, shader_counts[2] {};
    int alpha_fills = 0, alpha_dispatches = 0;

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
    ~Source() override {
      if (state->compositor) {
        require(state->destroyed_devices == 1, "Wayland source released before GPU resource settlement");
        ++state->source_destroyed;
      }
    }

    bool compositor_owned() const override {
      return state->compositor;
    }

    std::string connector() const override {
      return state->compositor ? "CPU-OUTPUT" : "";
    }

    void snapshot_complete() override {
      if (state->compositor) {
        require(state->snapshot_done, "Destination reused before GPU copy completion");
        ++state->snapshot_completions;
      }
    }

    void encode_complete() override {
      if (state->compositor) {
        // A discarded frame is completed after its census, with nothing encoded.
        const bool discarded = state->alpha_done && state->encoded_formats.size() == state->encodes_at_capture;
        require(state->encode_done || discarded, "Snapshot reused before GPU encode completion or alpha census");
        ++state->encode_completions;
      }
    }

    std::shared_ptr<egl::img_descriptor_t> next_within(std::chrono::milliseconds timeout) override {
      state->bounded_waits.push_back(timeout);
      if (state->ready_at_bound) {
        std::this_thread::sleep_for(timeout);
      }
      return next();
    }

    std::shared_ptr<egl::img_descriptor_t> next() override {
      require(state->captured.expired(), "Previous capture lifetime retained into the next request");
      if (state->compositor) {
        require(state->captures == size_t(state->encode_completions), "Destination rewritten while snapshot/encode was in flight");
        state->snapshot_done = state->encode_done = false;
      }
      state->alpha_done = false;
      state->encodes_at_capture = state->encoded_formats.size();
      if (++state->requests == state->failing_request) {
        throw std::runtime_error("Pyrowave screencopy timeout; check compositor capture permission for the resolved versioned Apollo executable");
      }
      auto image = std::make_shared<egl::img_descriptor_t>();
      image->sd = {};
      std::fill_n(image->sd.fds, 4, -1);
      image->sd.width = 2560;
      image->sd.height = 1440;
      image->sd.fourcc = state->fourccs[std::min(state->captures++, state->fourccs.size() - 1)];
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
      return state->compositor ? platf::touch_port_t {0, 0, 2048, 1152} : platf::touch_port_t {0, 0, 2560, 1440};
    }

    std::pair<int, int> desktop_size() const override {
      return state->compositor ? std::pair<int, int> {2048, 1152} : std::pair<int, int> {2560, 1440};
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
    require(s.rate_target == limits.codec_target_bytes(), "Production encoder received a different codec target");
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
    packetization(output, over_record, pyrowave::make_limits({1392, 1, 0, 500000, false}), "codec record exceeds the 64 KiB limit");
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

  void higher_bitrate_startup() {
    for (int requested : {150000, 200000, 250000, 300000, 400000, 500000}) {
      for (bool encrypted : {false, true}) {
        const int budget = rtsp_stream::video_bitrate(requested, 30, 2, true, true);
        const auto limits = pyrowave::make_limits({1392, 30, 0, budget, encrypted});
        State large;
        large.separate_records = true;
        // Valid complete pinned blocks, with total payload close to the target.
        large.block_sizes.assign((limits.codec_target_bytes() - 8) / 16380, 16380);
        packetization({2560, 1440}, large, limits);
        require(large.raw.size() * 4 > 65536, "Higher-bitrate fixture did not exercise a multi-record frame");

        State oversized;
        oversized.separate_records = true;
        oversized.block_sizes.assign(limits.frame_bytes / 16380 + 1, 16380);
        packetization({2560, 1440}, oversized, limits, "Actual codec bytes exceed frame budget");
        require(!oversized.packetize_calls, "Oversized high-rate output reached the unchecked codec copy");
        std::cout << "PASS selected " << requested << " Kbps encrypted=" << encrypted << " production target=" << large.rate_target << " multi-record bounds\n";
      }
    }
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

  void production_owned_gpu_copy();

  void wayland_startup_and_mapping() {
    config::video.pyrowave_capture_source = "wayland";
    config::video.capture = "kms";
    config::video.experimental_pyrowave = true;
    require(pyrowave::enabled(), "Private Wayland source changed conventional global KMS setting");
    for (auto result : {VK_SUCCESS, VK_TIMEOUT}) {
      State s;
      state = &s;
      s.compositor = true;
      s.encode_result = result;
      s.fourccs = {DRM_FORMAT_ARGB8888};
      pyrowave::CaptureGate gate;
      auto lease = gate.acquire(true);
      const auto limits = pyrowave::make_limits({1200, 20, 0, 100000, true});
      if (result == VK_TIMEOUT) {
        try {
          pyrowave::make_session({2560, 1440}, limits);
          require(false, "Wayland timeout returned a session");
        } catch (const std::runtime_error &e) {
          require(std::string_view(e.what()).find("wait encode timeline") != std::string_view::npos, "Unexpected Wayland failure");
        }
        require(s.retained_at_idle && s.snapshot_completions == 1 && s.encode_completions == 0, "Failed encode lost destination before GPU settlement");
      } else {
        auto session = pyrowave::make_session({2560, 1440}, limits);
        mail::man = std::make_shared<safe::mail_raw_t>();
        auto local = std::make_shared<safe::mail_raw_t>();
        auto shutdown = local->event<bool>(mail::shutdown);
        auto mapping_event = local->event<input::touch_port_t>(mail::touch_port);
        s.on_sleep = [&]() {
          shutdown->raise(true);
        };
        session->run(local, &s);
        session->drain(&s);
        auto mapping = mapping_event->pop();
        require(mapping && mapping->width == 2560 && mapping->height == 1440 && mapping->env_width == 2048 && mapping->env_height == 1152 && mapping->scalar_inv == 0.8f && mapping->logical_desktop, "Production session confused native pixels with fractional logical input mapping");
        require(s.snapshot_completions == 2 && s.encode_completions == 2 && s.alpha_checks == 2, "Production session skipped completion ordering/opaque alpha");
        session.reset();
        mail::man.reset();
      }
      require(s.source_destroyed == 1 && s.captured.expired(), "Wayland source/FD cleanup leaked");
      lease.reset();
      require(bool(gate.acquire(false)), "Conventional capture cannot reconnect after Wayland teardown");
    }
    config::video.pyrowave_capture_source = "kms-diagnostic";
    std::cout << "PASS production Wayland session: global kms retained, logical scale1.25 mapping, snapshot/encode callbacks, alpha8, failed startup settlement and conventional reconnect\n";
  }

  void live_format_changes() {
    State s;
    state = &s;
    s.fourccs = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR2101010, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_XRGB2101010, DRM_FORMAT_XRGB8888};
    const auto limits = pyrowave::make_limits({1200, 20, 0, 100000, true});
    auto session = pyrowave::make_session({2560, 1440}, limits);
    mail::man = std::make_shared<safe::mail_raw_t>();
    auto local_mail = std::make_shared<safe::mail_raw_t>();
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto shutdown = local_mail->event<bool>(mail::shutdown);
    std::vector<int64_t> frames;
    auto consume = [&]() {
      while (packets->peek()) {
        auto packet = packets->pop(0ms);
        frames.push_back(packet->frame_index());
        session->record_emitted(packet->data_size());
      }
    };
    s.on_sleep = [&]() {
      consume();
      if (s.captures >= s.fourccs.size()) {
        shutdown->raise(true);
      }
    };
    session->run(local_mail, &s);
    consume();
    session->drain(&s);
    require(s.captures == 7 && s.encoded_formats == std::vector<VkFormat> {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_B8G8R8A8_UNORM}, "Production capture/import/encode failed to adapt all accepted channel orders");
    require(s.output.width == 2560 && s.output.height == 1440 && frames == std::vector<int64_t> {1, 2, 3, 4, 5, 6}, "Framebuffer changes reset negotiated dimensions or emitted frame sequence");
    require(s.snapshots_created == 4 && s.alpha_checks == 2 && !s.destroyed_owned, "Production transitions are bounded, preserve alpha-bearing checks and retain cached snapshots");
    session.reset();
    require(s.destroyed_owned == 4 && s.destroyed_imports == 7 && s.destroyed_memory == 7 && s.destroyed_devices == 1 && s.captured.expired(), "Production transitions leaked snapshots/imports/capture lifetime");
    mail::man.reset();
    std::cout << "PASS production format changes: six fourccs, four retained snapshots, per-frame alpha policy, stable output/frame numbering and settled capture lifetimes\n";
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
  std::unique_ptr<kms_diagnostic_source_t> make_pyrowave_wayland_source(const std::string &, const std::string &, std::function<void(const pyrowave_diag::layout_t &)>) {
    require(state->compositor, "Unexpected Wayland selection in KMS diagnostic fixture");
    return std::make_unique<Source>();
  }

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
  pyrowave_diag::snapshot_api_t api {};
  api.create_image = [](VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *image) {
    *image = reinterpret_cast<VkImage>(uintptr_t(10 + state->snapshots_created++));
    return VK_SUCCESS;
  };
  api.destroy_image = [](VkDevice, VkImage, const VkAllocationCallbacks *) {
    require(state->encoder_destroyed && state->codec_views_drained, "Snapshot released before codec teardown drained borrowed views");
    ++state->destroyed_owned;
  };
  api.image_requirements = [](VkDevice, VkImage, VkMemoryRequirements *requirements) {
    *requirements = {4096, 4096, 1};
  };
  api.memory_properties = [](VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *properties) {
    properties->memoryTypeCount = 1;
    properties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  };
  api.allocate_memory = [](VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *memory) {
    *memory = reinterpret_cast<VkDeviceMemory>(uintptr_t(5));
    return VK_SUCCESS;
  };
  api.free_memory = [](VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {
  };
  api.bind_image_memory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
  };
  gpu->snapshots = std::make_unique<pyrowave_diag::snapshots_t>(VK_NULL_HANDLE, gpu->device, api);
  gpu->active_snapshot = &gpu->snapshots->select(2560, 1440, VK_FORMAT_B8G8R8A8_UNORM, true);
  gpu->owned_image = gpu->active_snapshot->image;
  gpu->width = 2560;
  gpu->height = 1440;
  gpu->format = VK_FORMAT_B8G8R8A8_UNORM;
  gpu->diagnostic = false;
  gpu->encoder = reinterpret_cast<pyrowave_encoder>(uintptr_t(6));
  gpu->pyro = reinterpret_cast<pyrowave_device>(uintptr_t(7));
}

extern "C" void mock_import(pyrowave_diag::gpu_t *, const pyrowave_diag::layout_t &) asm("__wrap__ZN13pyrowave_diag5gpu_t6importERKNS_8layout_tE");

extern "C" void mock_import(pyrowave_diag::gpu_t *gpu, const pyrowave_diag::layout_t &layout) {
  gpu->prepare(layout.width, layout.height, pyrowave_diag::validate_layout(layout, gpu->compositor_handoff));
  pyrowave_diag::import_api_t api {};
  api.destroy_image = import_destroy;
  api.free_memory = import_free;
  gpu->imported = std::make_unique<pyrowave_diag::dma_buf_image_t>(gpu->device, api);
  gpu->imported->image = reinterpret_cast<VkImage>(uintptr_t(3));
  gpu->imported->memory = reinterpret_cast<VkDeviceMemory>(uintptr_t(4));
}

extern "C" void mock_snapshot(pyrowave_diag::gpu_t *, const std::vector<uint8_t> *) asm("__wrap__ZN13pyrowave_diag5gpu_t8snapshotEPKSt6vectorIhSaIhEE");

extern "C" void mock_snapshot(pyrowave_diag::gpu_t *, const std::vector<uint8_t> *) {
  state->snapshot_done = true;
}

extern "C" pyrowave_diag::alpha_counts_t mock_alpha(pyrowave_diag::gpu_t *) asm("__wrap__ZN13pyrowave_diag5gpu_t11count_alphaEv");

extern "C" pyrowave_diag::alpha_counts_t mock_alpha(pyrowave_diag::gpu_t *) {
  require(state->snapshot_done, "Alpha census ran before the snapshot copy completed");
  const size_t index = state->alpha_checks++;
  state->alpha_done = true;
  return index < state->alpha_script.size() ? state->alpha_script[index] : pyrowave_diag::alpha_counts_t {uint64_t(2560) * 1440, 0, 0};
}

extern "C" pyrowave_diag::alpha_counts_t real_count_alpha(pyrowave_diag::gpu_t *) asm("__real__ZN13pyrowave_diag5gpu_t11count_alphaEv");

// The simulated device executes the recorded commands in order: the result
// block is cleared, then the shader accumulates its two counters.
extern "C" VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
  require(!offset && size == sizeof(state->alpha_result) && !data, "Alpha census did not clear both counters");
  std::memset(state->alpha_result, 0, sizeof(state->alpha_result));
  ++state->alpha_fills;
}

extern "C" VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint point, VkPipeline pipeline) {
  require(point == VK_PIPELINE_BIND_POINT_COMPUTE && pipeline, "Alpha census lost its compute pipeline");
}

extern "C" VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint point, VkPipelineLayout, uint32_t first, uint32_t count, const VkDescriptorSet *, uint32_t dynamic, const uint32_t *) {
  require(point == VK_PIPELINE_BIND_POINT_COMPUTE && !first && count == 1 && !dynamic, "Alpha census descriptor binding changed");
}

extern "C" VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdDispatch(VkCommandBuffer, uint32_t x, uint32_t y, uint32_t z) {
  require(state->alpha_fills == state->alpha_dispatches + 1, "Alpha census dispatched without first clearing its counters");
  require(x == 160 && y == 90 && z == 1, "Alpha census does not cover every native 2560x1440 texel in 16x16 groups");
  state->alpha_result[0] += state->shader_counts[0];
  state->alpha_result[1] += state->shader_counts[1];
  ++state->alpha_dispatches;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo *, uint64_t timeout) {
  state->wait_timeout = timeout;
  state->encode_done = state->encode_result == VK_SUCCESS;
  return state->encode_result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkDeviceWaitIdle(VkDevice) {
  std::unique_lock lock(state->mutex);
  state->idle_entered = true;
  state->retained_at_idle = state->retained();
  state->captured_at_idle = !state->captured.expired();
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

extern "C" void __wrap_pyrowave_encoder_destroy(pyrowave_encoder) {
  require(!state->destroyed_owned && !state->destroyed_devices, "Encoder teardown lost its source snapshots/device");
  state->encoder_destroyed = true;
}

extern "C" void __wrap_pyrowave_device_destroy(pyrowave_device) {
  require(state->encoder_destroyed && !state->destroyed_owned && !state->destroyed_devices, "Deferred codec views were drained after releasing snapshots/device");
  state->codec_views_drained = true;
}

extern "C" pyrowave_result __wrap_pyrowave_encoder_encode_gpu_scaled_synchronous(pyrowave_encoder, const pyrowave_gpu_sync_operation *, const pyrowave_gpu_sync_operation *, const pyrowave_scaled_encode_info *scale, const pyrowave_rate_control *rate) {
  require(scale->intermediate_plane_format == VK_FORMAT_R16_UNORM, "Live scaler must retain a constant 10-bit-capable intermediate");
  state->encoded_formats.push_back(scale->view.view_format);
  state->rate_target = rate->maximum_bitstream_size;
  return PYROWAVE_SUCCESS;
}

extern "C" pyrowave_result __wrap_pyrowave_encoder_compute_num_packets(pyrowave_encoder, size_t target, size_t *count) {
  state->count_target = target;
  // Fixtures request one complete record; the over-record case deliberately
  // simulates a packetizer violating the host's independent record cap.
  *count = state->separate_records ? state->block_sizes.size() : 1;
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
  require(*count == (state->separate_records ? state->block_sizes.size() : 1) && size == 8 + state->raw.size() * sizeof(uint32_t), "Production codec copy buffer differs from mapped extent");
  // Pinned BitstreamSequenceHeader followed by all whole codec blocks.
  const uint32_t sequence[] {uint32_t(state->output.width - 1) | (uint32_t(state->output.height - 1) << 14) | (1u << 31), uint32_t(state->block_sizes.size())};
  std::memcpy(bytes, sequence, sizeof(sequence));
  std::memcpy(static_cast<uint8_t *>(bytes) + sizeof(sequence), state->raw.data(), state->raw.size() * sizeof(uint32_t));
  if (state->separate_records) {
    size_t offset = 0;
    for (size_t i = 0; i < *count; ++i) {
      const size_t record_size = state->block_sizes[i] + (i ? 0 : sizeof(sequence));
      packets[i] = {offset, record_size};
      offset += record_size;
    }
    require(offset == size, "Mock packetizer did not preserve complete blocks");
  } else {
    packets[0] = {0, size};
  }
  return PYROWAVE_SUCCESS;
}

extern "C" void real_snapshot(pyrowave_diag::gpu_t *, const std::vector<uint8_t> *) asm("__real__ZN13pyrowave_diag5gpu_t8snapshotEPKSt6vectorIhSaIhEE");

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags) {
  return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkResetFences(VkDevice, uint32_t, const VkFence *) {
  return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *) {
  return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEndCommandBuffer(VkCommandBuffer) {
  return VK_SUCCESS;
}

extern "C" VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t count, const VkImageMemoryBarrier *barriers) {
  for (uint32_t i = 0; i < count; ++i) {
    if (barriers[i].srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT) {
      ++state->foreign_acquires;
    }
    if (barriers[i].dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT) {
      ++state->foreign_releases;
    }
  }
}

extern "C" VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t count, const VkImageCopy *copy) {
  require(count == 1 && copy->extent.width == 2560 && copy->extent.height == 1440, "GPU snapshot changed native capture extent");
  ++state->image_copies;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueSubmit(VkQueue, uint32_t count, const VkSubmitInfo *submit, VkFence) {
  require(count == 1, "Unexpected GPU submission count");
  state->copy_signals = submit->signalSemaphoreCount;
  return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t timeout) {
  require(timeout == 1'000'000'000ULL, "Owned GPU snapshot lost bounded wait");
  state->retained_at_copy_wait = state->retained();
  return state->copy_result;
}

namespace {
  void production_owned_gpu_copy() {
    for (auto result : {VK_SUCCESS, VK_TIMEOUT}) {
      State s;
      state = &s;
      s.compositor = true;
      s.copy_result = result;
      {
        Source source;
        pyrowave_diag::gpu_t gpu;
        gpu.compositor_handoff = true;
        mock_init(&gpu, nullptr, false, 2560, 1440);
        auto image = source.next();
        gpu.capture_lifetime = image;
        pyrowave_diag::layout_t layout;
        layout.width = 2560;
        layout.height = 1440;
        layout.fourcc = DRM_FORMAT_XRGB8888;
        layout.modifier = DRM_FORMAT_MOD_LINEAR;
        layout.pitches[0] = 10240;
        layout.fds[0] = image->sd.fds[0];
        mock_import(&gpu, layout);
        try {
          real_snapshot(&gpu, nullptr);
          require(result == VK_SUCCESS, "Timed-out GPU copy was accepted");
        } catch (const std::runtime_error &e) {
          require(result == VK_TIMEOUT && std::string_view(e.what()).find("wait snapshot copy") != std::string_view::npos, "Unexpected snapshot failure");
        }
        require(s.retained_at_copy_wait && s.foreign_acquires == 1 && s.foreign_releases == 1 && s.image_copies == 1 && s.copy_signals == 0, "Production owned copy lost FD lifetime/FOREIGN transitions or used KMS reservation publication");
      }
      check_released(s);
      require(s.source_destroyed == 1, "Owned source destroyed before GPU cleanup");
    }
    std::cout << "PASS real GPU snapshot with mocked Vulkan: ready-owned destination, FOREIGN acquire/copy/release, no reservation bridge, bounded copy wait/timeout and retained FD cleanup\n";
  }

  using pyrowave_diag::alpha_class_t;
  using pyrowave_diag::alpha_counts_t;
  constexpr uint64_t native_texels = uint64_t(2560) * 1440;
  constexpr uint32_t every_texel = uint32_t(native_texels);
  constexpr alpha_counts_t unpainted {native_texels, every_texel, 0};

  // Collects Apollo's log records while it is alive.
  class LogCapture {
  public:
    LogCapture():
        sink(boost::make_shared<sink_t>()) {
      sink->locked_backend()->add_stream(boost::shared_ptr<std::ostream>(&messages, [](std::ostream *) {
      }));
      sink->set_formatter([](const boost::log::record_view &record, boost::log::formatting_ostream &out) {
        out << record.attribute_values()["Message"].extract<std::string>().get();
      });
      boost::log::core::get()->add_sink(sink);
    }

    ~LogCapture() {
      boost::log::core::get()->remove_sink(sink);
    }

    std::string text() {
      sink->flush();
      return messages.str();
    }

  private:
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    std::ostringstream messages;
    boost::shared_ptr<sink_t> sink;
  };

  bool has(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
  }

  size_t occurrences(std::string_view text, std::string_view part) {
    size_t count = 0;
    for (auto at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size())) {
      ++count;
    }
    return count;
  }

  void wayland_alpha(State &s, std::vector<alpha_counts_t> script) {
    s.compositor = true;
    s.fourccs = {DRM_FORMAT_ARGB8888};
    s.alpha_script = std::move(script);
  }

  struct Outcome {
    std::string error, log;  // error is empty when startup returned a session.
  };

  // Production factory, constructor and teardown with a scripted census. Every
  // outcome must settle GPU, capture and gate ownership exactly once.
  Outcome scripted_startup(State &s) {
    state = &s;
    config::video.pyrowave_capture_source = s.compositor ? "wayland" : "kms-diagnostic";
    Outcome outcome;
    {
      LogCapture log;
      pyrowave::CaptureGate gate;
      auto lease = gate.acquire(true);
      try {
        auto session = pyrowave::make_session({2560, 1440}, pyrowave::make_limits({1200, 20, 0, 100000, true}));
        session.reset();
      } catch (const std::runtime_error &e) {
        outcome.error = e.what();
        require(!outcome.error.empty(), "Startup failure carried no diagnostic");
      }
      outcome.log = log.text();
      lease.reset();
      require(bool(gate.acquire(false)), "Conventional capture cannot reconnect after the alpha policy settled");
    }
    config::video.pyrowave_capture_source = "kms-diagnostic";
    require(!s.fatal_calls, "Alpha policy reached the fatal watchdog");
    require(s.destroyed_devices == 1 && s.captured.expired(), "Alpha policy leaked the GPU device or a captured FD");
    require(s.destroyed_imports == int(s.captures) && s.destroyed_memory == int(s.captures), "Each captured import must be released exactly once");
    require(!s.compositor || s.source_destroyed == 1, "Wayland source was not destroyed after GPU settlement");
    return outcome;
  }

  void startup_rerequests_unpainted_frames() {
    for (size_t discarded : {size_t(1), size_t(4)}) {
      State s;
      wayland_alpha(s, std::vector<alpha_counts_t>(discarded, unpainted));
      const auto outcome = scripted_startup(s);
      require(outcome.error.empty(), outcome.error.c_str());
      const size_t total = discarded + 1;
      require(s.requests == total && s.captures == total && size_t(s.alpha_checks) == total, "Startup did not request one frame per discard plus the accepted frame");
      require(size_t(s.snapshot_completions) == total && size_t(s.encode_completions) == total, "A discarded or accepted frame was not completed exactly once");
      require(s.encoded_formats.size() == 1 && s.packetize_calls == 1, "A discarded transparent frame reached the encoder or packetizer");
      require(s.bounded_waits.size() == discarded && s.bounded_waits[0] == 1000ms, "Re-requests were not bounded by the startup budget");
      for (size_t i = 1; i < discarded; ++i) {
        require(s.bounded_waits[i] > 0ms && s.bounded_waits[i] <= s.bounded_waits[i - 1], "A later re-request restarted or exceeded the shared retry budget");
      }
      require(!s.captured_at_idle, "A discarded capture outlived the session it was dropped from");
      require(occurrences(outcome.log, "is wholly transparent black; discarding it and requesting again within ") == discarded, "Each discarded startup frame must be logged exactly once");
      require(has(outcome.log, "Pyrowave Wayland startup capture 1/5 is wholly transparent black") && has(outcome.log, "alpha=transparent-black (3686400 transparent-black, 0 other nonopaque of 3686400 texels); 2560x1440 fourcc=AR24"), "Discard log lacks attempt, census or capture layout");
      require(has(outcome.log, ", capture layout 2560x1440 fourcc=AR24") && has(outcome.log, ", startup capture attempts=" + std::to_string(total)), "Ready log hides the capture layout or startup attempts");
      require(!has(outcome.log, "primary-plane"), "Wayland startup used the KMS primary-plane wording");
    }
    State opaque;
    wayland_alpha(opaque, {});
    const auto outcome = scripted_startup(opaque);
    require(outcome.error.empty() && opaque.requests == 1 && opaque.bounded_waits.empty() && has(outcome.log, ", startup capture attempts=1") && !has(outcome.log, "requesting again"), "An opaque first frame, whatever its color, must start without a re-request");
    std::cout << "PASS Wayland startup re-request: one and four wholly transparent-black frames discarded, completed once, never encoded; next opaque frame accepted\n";
  }

  void startup_exhausts_attempts() {
    State s;
    wayland_alpha(s, std::vector<alpha_counts_t>(8, unpainted));
    const auto outcome = scripted_startup(s);
    require(has(outcome.error, "Wayland screencopy destination stayed wholly transparent black at startup: 5 of 5 capture attempts, ") && has(outcome.error, " ms of the 1000 ms retry budget") && has(outcome.error, "alpha=transparent-black") && has(outcome.error, "fourcc=AR24") && !has(outcome.error, "primary-plane"), outcome.error.c_str());
    require(s.requests == 5 && s.captures == 5 && s.alpha_checks == 5 && s.snapshot_completions == 5, "Attempt bound is not exactly five captures");
    require(s.encode_completions == 4 && s.captured_at_idle, "Discards must complete once each; the rejected last frame stays held until GPU settlement");
    require(s.encoded_formats.empty() && !s.packetize_calls, "A wholly transparent frame was encoded after exhaustion");
    require(s.bounded_waits.size() == 4 && occurrences(outcome.log, "requesting again within ") == 4, "Exhaustion did not follow four bounded re-requests");
    std::cout << "PASS Wayland startup exhaustion: fifth wholly transparent-black capture fails closed, nothing encoded, cleanup settled\n";
  }

  void startup_budget_expires() {
    State s;
    wayland_alpha(s, std::vector<alpha_counts_t>(8, unpainted));
    s.ready_at_bound = true;
    const auto began = std::chrono::steady_clock::now();
    const auto outcome = scripted_startup(s);
    const auto elapsed = std::chrono::steady_clock::now() - began;
    require(has(outcome.error, "stayed wholly transparent black at startup: 2 of 5 capture attempts, ") && has(outcome.error, " ms of the 1000 ms retry budget"), outcome.error.c_str());
    require(s.requests == 2 && s.bounded_waits == std::vector<std::chrono::milliseconds> {1000ms}, "An exhausted retry budget still issued another request");
    require(elapsed >= 1000ms && elapsed < 5000ms, "Retry budget is not about one second of capture waiting");
    require(s.encode_completions == 1 && s.encoded_formats.empty() && s.captured_at_idle, "Budget exhaustion completed, encoded or released the rejected frame");
    std::cout << "PASS Wayland startup budget: a re-request ready only as the one-second budget expires fails closed after two captures\n";
  }

  void startup_rerequest_failure() {
    State s;
    wayland_alpha(s, {unpainted});
    s.failing_request = 2;
    const auto outcome = scripted_startup(s);
    require(has(outcome.error, "Wayland screencopy startup re-request failed (capture attempt 2 of 5, bounded to 1000 ms) after a wholly transparent-black frame: Pyrowave screencopy timeout"), outcome.error.c_str());
    require(s.requests == 2 && s.captures == 1 && s.alpha_checks == 1, "Failed re-request was repeated");
    require(s.snapshot_completions == 1 && s.encode_completions == 1, "Discarded frame was not completed exactly once before the failed re-request");
    require(s.encoded_formats.empty() && !s.captured_at_idle, "Discarded import/FD was encoded or still held when the re-request failed");
    std::cout << "PASS Wayland startup re-request failure: discarded frame settled once, compositor error reported with startup context, cleanup settled\n";
  }

  void startup_rejects_other_nonopaque() {
    // One painted texel, one transparent texel, fractional or colored texels
    // everywhere or once, a mixture, and two censuses that cannot be true.
    const std::vector<alpha_counts_t> cases {{native_texels, every_texel - 1, 0}, {native_texels, 1, 0}, {native_texels, 0, every_texel}, {native_texels, 0, 1}, {native_texels, every_texel - 1, 1}, {native_texels, every_texel, 1}, {0, 0, 0}};
    for (const auto &counts : cases) {
      State s;
      wayland_alpha(s, {counts});
      const auto outcome = scripted_startup(s);
      require(has(outcome.error, "Wayland screencopy destination is not opaque; frame rejected (alpha=nonopaque (") && has(outcome.error, "fourcc=AR24") && !has(outcome.error, "primary-plane"), outcome.error.c_str());
      require(s.requests == 1 && s.captures == 1 && s.bounded_waits.empty() && !has(outcome.log, "requesting again"), "A partially, fractionally or colored nonopaque first frame was re-requested");
      require(s.snapshot_completions == 1 && s.encode_completions == 0 && s.encoded_formats.empty() && s.captured_at_idle, "Rejected frame was completed, encoded or released before GPU settlement");
    }
    std::cout << "PASS Wayland startup rejection: partial, fractional, colored and inconsistent censuses fail closed immediately\n";
  }

  void kms_never_rerequests() {
    const std::vector<alpha_counts_t> cases {unpainted, {native_texels, 0, 1}, {native_texels, every_texel - 1, 0}};
    for (const auto &counts : cases) {
      State s;
      s.fourccs = {DRM_FORMAT_ABGR2101010};
      s.alpha_script = {counts};
      const auto outcome = scripted_startup(s);
      require(outcome.error.rfind("Nonopaque primary-plane alpha: composition is unimplemented (alpha=", 0) == 0 && has(outcome.error, "fourcc=AB30") && !has(outcome.error, "Wayland"), outcome.error.c_str());
      require(s.requests == 1 && s.captures == 1 && s.bounded_waits.empty() && s.encoded_formats.empty() && s.captured_at_idle, "KMS diagnostic re-requested, encoded or released a nonopaque frame");
    }
    std::cout << "PASS KMS diagnostic alpha: wholly transparent, fractional and partial primary planes fail closed without a re-request\n";
  }

  void steady_state_rejects() {
    const std::vector<std::pair<alpha_counts_t, std::string_view>> cases {{unpainted, "is wholly transparent black after startup; frame rejected (alpha=transparent-black ("}, {{native_texels, 0, 1}, "is not opaque; frame rejected (alpha=nonopaque ("}};
    for (const auto &[counts, reason] : cases) {
      State s;
      state = &s;
      wayland_alpha(s, {{native_texels, 0, 0}, counts});
      config::video.pyrowave_capture_source = "wayland";
      std::string text;
      {
        LogCapture log;
        auto session = pyrowave::make_session({2560, 1440}, pyrowave::make_limits({1200, 20, 0, 100000, true}));
        mail::man = std::make_shared<safe::mail_raw_t>();
        auto local = std::make_shared<safe::mail_raw_t>();
        auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
        auto shutdown = local->event<bool>(mail::shutdown);
        int sleeps = 0;
        s.on_sleep = [&]() {
          if (++sleeps > 3) {
            shutdown->raise(true);  // Only reached if the rejected frame was accepted.
          }
        };
        session->run(local, &s);
        require(shutdown->peek() && !packets->peek(), "Rejected live frame was queued or left the session running");
        session->drain(&s);
        session.reset();
        text = log.text();
        mail::man.reset();
      }
      config::video.pyrowave_capture_source = "kms-diagnostic";
      require(has(text, "Pyrowave live capture stopped: Wayland screencopy destination " + std::string(reason)) && has(text, "fourcc=AR24"), "Steady-state rejection lost its source-specific diagnostic");
      require(s.requests == 2 && s.captures == 2 && s.bounded_waits.empty() && !has(text, "requesting again"), "A nonopaque frame after startup was re-requested");
      require(s.alpha_checks == 2 && s.snapshot_completions == 2 && s.encode_completions == 1 && s.encoded_formats.size() == 1, "Rejected live frame was completed or encoded");
      require(s.captured_at_idle && s.source_destroyed == 1 && s.captured.expired() && s.destroyed_devices == 1 && !s.fatal_calls, "Steady-state rejection leaked capture/GPU ownership");
    }
    std::cout << "PASS Wayland steady state: wholly transparent-black and other nonopaque frames after startup end capture without a re-request\n";
  }

  // The real census submission and readback, with only device entry points mocked.
  void production_alpha_census() {
    struct case_t {
      uint32_t transparent_black, other_nonopaque;
      alpha_class_t expected;
    };

    for (const auto &c : {case_t {0, 0, alpha_class_t::opaque}, case_t {every_texel, 0, alpha_class_t::transparent_black}, case_t {every_texel - 1, 0, alpha_class_t::nonopaque}, case_t {0, every_texel, alpha_class_t::nonopaque}, case_t {7, 9, alpha_class_t::nonopaque}}) {
      State s;
      state = &s;
      pyrowave_diag::gpu_t gpu;
      // Fake handles skip pipeline creation and must never reach real teardown.
      auto unseed = util::fail_guard([&]() {
        gpu.alpha_pipeline = VK_NULL_HANDLE;
        gpu.alpha_mapped = nullptr;
      });
      mock_init(&gpu, nullptr, false, 2560, 1440);
      gpu.alpha_pipeline = reinterpret_cast<VkPipeline>(uintptr_t(8));
      gpu.alpha_bound_image = gpu.owned_image;
      gpu.alpha_mapped = s.alpha_result;
      s.shader_counts[0] = c.transparent_black;
      s.shader_counts[1] = c.other_nonopaque;
      for (int frame = 1; frame <= 2; ++frame) {
        const auto counts = real_count_alpha(&gpu);
        require(counts.pixels == native_texels && counts.transparent_black == c.transparent_black && counts.other_nonopaque == c.other_nonopaque, "Census readback lost a counter or kept stale data from the previous frame");
        require(pyrowave_diag::classify_alpha(counts) == c.expected, "Census readback was classified incorrectly");
        require(s.alpha_fills == frame && s.alpha_dispatches == frame && !s.copy_signals, "Census was not one cleared, bounded, unsignalled submission per frame");
      }
    }
    State s;
    state = &s;
    s.copy_result = VK_TIMEOUT;
    pyrowave_diag::gpu_t gpu;
    auto unseed = util::fail_guard([&]() {
      gpu.alpha_pipeline = VK_NULL_HANDLE;
      gpu.alpha_mapped = nullptr;
    });
    mock_init(&gpu, nullptr, false, 2560, 1440);
    gpu.alpha_pipeline = reinterpret_cast<VkPipeline>(uintptr_t(8));
    gpu.alpha_bound_image = gpu.owned_image;
    gpu.alpha_mapped = s.alpha_result;
    try {
      real_count_alpha(&gpu);
      require(false, "Timed-out alpha census returned counters");
    } catch (const std::runtime_error &e) {
      require(has(e.what(), "wait alpha check"), "Unexpected alpha census failure");
    }
    std::cout << "PASS real alpha census with mocked Vulkan: both counters cleared, full native dispatch, one-second bounded wait, exact readback\n";
  }

  void embedded_alpha_shader() {
    std::ifstream file(APOLLO_PYROWAVE_ALPHA_SPV, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(pyrowave_alpha_spv[0] == 0x07230203 && bytes.size() == sizeof(pyrowave_alpha_spv) && !std::memcmp(bytes.data(), pyrowave_alpha_spv, bytes.size()), "Embedded alpha census SPIR-V differs from the binary module that spirv-val checks");
    std::cout << "PASS embedded alpha census SPIR-V is byte-identical to the statically validated module\n";
  }
}  // namespace

int main() {
  config::video.pyrowave_capture_source = "kms-diagnostic";
  try {
    for (auto output : {pyrowave::Dimensions {1920, 1080}, pyrowave::Dimensions {2560, 1440}, pyrowave::Dimensions {3840, 2160}}) {
      healthy_and_recoverable(output, VK_SUCCESS);
      healthy_and_recoverable(output, VK_TIMEOUT);
      codec_record_bounds(output);
    }
    higher_bitrate_startup();
    stalled_cleanup();
    healthy_and_recoverable({3840, 2160}, VK_SUCCESS);
    live_counters();
    live_format_changes();
    wayland_startup_and_mapping();
    production_owned_gpu_copy();
    startup_rerequests_unpainted_frames();
    startup_exhausts_attempts();
    startup_budget_expires();
    startup_rerequest_failure();
    startup_rejects_other_nonopaque();
    kms_never_rerequests();
    steady_state_rejects();
    production_alpha_census();
    embedded_alpha_shader();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
