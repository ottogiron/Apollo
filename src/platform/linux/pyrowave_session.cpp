/** @brief Private Wayland/KMS Pyrowave video producer for the existing Apollo session transport. */
#include "src/pyrowave_session.h"

#include "pyrowave_capture.h"
#include "pyrowave_diagnostic_vulkan.h"
#include "pyrowave_encoder_layout.h"
#include "pyrowave_live_layout.h"
#include "pyrowave_wayland.h"
#include "src/display_device.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/process.h"
#include "src/pyrowave_lifetime.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace pyrowave {
  using namespace std::chrono_literals;
  using pyrowave_diag::checked;

  namespace {
    // Arm before constructing CaptureSession and keep armed through constructor
    // unwinding (including vkDeviceWaitIdle and codec destruction). A separate
    // joined monitor cannot be starved by work on Apollo's shared task pool.
    // Use the same process-fatal recovery policy as stream::session::join.
    class StartupWatchdog {
    public:
      StartupWatchdog():
          monitor([this, deadline = std::chrono::steady_clock::now() + 10s]() {
            std::unique_lock lock(mutex);
            if (cv.wait_until(lock, deadline, [this]() {
                  return finished;
                })) {
              return;
            }
            lock.unlock();
            BOOST_LOG(fatal) << "Hang detected! Pyrowave startup or cleanup failed to finish in 10 seconds; terminating Apollo.";
            logging::log_flush();
            lifetime::debug_trap();
          }) {}

      ~StartupWatchdog() {
        {
          std::lock_guard lock(mutex);
          finished = true;
        }
        cv.notify_one();
        monitor.join();
      }

      StartupWatchdog(const StartupWatchdog &) = delete;
      StartupWatchdog &operator=(const StartupWatchdog &) = delete;

    private:
      std::mutex mutex;
      std::condition_variable cv;
      bool finished = false;
      std::thread monitor;
    };

    // Private mapped metadata ABI at the exact codec pin, not a wire type.
    struct RawBlock {
      uint32_t offset_u32, num_words;
    };

    static_assert(sizeof(RawBlock) == 8);

    // Budget failures can drop an independent frame. Capture/import/fence errors
    // terminate the session instead of falling back to another capture backend.
    class FrameTooLarge: public std::runtime_error {
    public:
      using std::runtime_error::runtime_error;
    };

    class CaptureSession final: public Session {
    public:
      CaptureSession(Dimensions output, const Limits &limits):
          output(output),
          limits(limits),
          window(std::make_shared<FrameWindow>()) {
        if (!output.supported()) {
          throw std::runtime_error("Unsupported Pyrowave output dimensions");
        }
        if (config::video.pyrowave_capture_source == "wayland") {
          gpu.compositor_handoff = true;
          source = platf::make_pyrowave_wayland_source(config::video.pyrowave_output_name, proc::proc.display_name, [&](const pyrowave_diag::layout_t &layout) {
            // Validate actual exported FDs before handing the buffer to Wayland.
            // No commands have been submitted; a rejected candidate unwinds transactionally.
            gpu.import(layout);
            gpu.release_import();
          });
        } else if (config::video.pyrowave_capture_source == "kms-diagnostic") {
          const auto name = !proc::proc.display_name.empty() ? proc::proc.display_name : display_device::map_output_name(config::video.output_name);
          source = platf::make_kms_diagnostic_source(name, true);
          BOOST_LOG(warning) << "Pyrowave KMS diagnostic selected: producer reuse is not atomic; separate hardware cursor omitted";
        } else {
          throw std::runtime_error("Unknown pyrowave_capture_source; choose wayland or explicit kms-diagnostic (no fallback)");
        }
        const auto identity = source->info();
        gpu.init(&identity, false, output.width, output.height);  // No decoder or CPU reference/readback allocation.
        encode(1);  // Validate first capture; acquire fresh content after the video ping.
        BOOST_LOG(info) << "Experimental Pyrowave v" << version << " codec=" << APOLLO_PYROWAVE_PIN
                        << " backend=" << (source->compositor_owned() ? "wayland-screencopy-dmabuf" : "kms-diagnostic")
                        << " connector=" << source->connector() << " ready: " << output.width << 'x' << output.height
                        << "/60 SDR from " << capture_size.width << 'x' << capture_size.height
                        << (output.width != capture_size.width || output.height != capture_size.height ? " (scaled), maximum frame " : " (native size), maximum frame ")
                        << limits.frame_bytes << " bytes, codec record cap " << max_codec_record_size
                        << " bytes, video wire budget " << limits.transport.bitrate_kbps << " Kbps (RTP/FEC/encryption included)"
                        << ", codec target " << limits.codec_target_bytes() << " bytes/frame (" << limits.codec_target_bytes() * 8 * 60 / 1000.0 << " Kbps at requested 60 FPS)"
                        << "; cursor=" << (source->compositor_owned() ? "included by compositor" : "separate hardware cursor omitted")
                        << ", logical input=" << source->viewport().width << 'x' << source->viewport().height;
      }

      void run(safe::mail_t mail, void *channel_data) override {
        counters_started = counters_reported = std::chrono::steady_clock::now();
        auto shutdown = mail->event<bool>(mail::shutdown);
        auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
        auto idr = mail->event<bool>(mail::idr);
        auto invalidate = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
        auto stopped = util::fail_guard([&]() {
          window->close();
          shutdown->raise(true);
        });
        const auto viewport = source->viewport();
        const auto [env_width, env_height] = source->desktop_size();
        const float scalar = std::min(float(output.width) / viewport.width, float(output.height) / viewport.height);
        mail->event<input::touch_port_t>(mail::touch_port)->raise(input::touch_port_t {{viewport.offset_x, viewport.offset_y, output.width, output.height}, env_width, env_height, (output.width - viewport.width * scalar) / 2, (output.height - viewport.height * scalar) / 2, 1 / scalar, source->compositor_owned()});
        mail->event<video::hdr_info_t>(mail::hdr)->raise(std::make_unique<video::hdr_info_raw_t>(false));
        platf::adjust_thread_priority(platf::thread_priority_e::high);
        auto timer = platf::create_high_precision_timer();
        if (!timer || !*timer) {
          BOOST_LOG(error) << "Pyrowave capture timer is unavailable";
          return;
        }
        auto due = std::chrono::steady_clock::now();
        uint32_t frame = 1;
        try {
          while (!shutdown->peek() && packets->running()) {
            const auto now = std::chrono::steady_clock::now();
            if (now < due) {
              timer->sleep_for(due - now);
            }
            if (shutdown->peek()) {
              break;
            }
            // No burst catch-up after a slow capture/encode or broadcaster stall.
            due = std::max(due + std::chrono::nanoseconds(1'000'000'000 / 60), std::chrono::steady_clock::now());
            report_counters(false);
            if (idr->peek()) {
              idr->pop();
            }
            if (invalidate->peek()) {
              invalidate->pop();
            }
            auto ticket = window->acquire();
            if (!ticket) {
              ++counters.backpressure_drops;
              continue;
            }
            std::vector<uint8_t> data;
            try {
              ++counters.attempts;
              const auto began = std::chrono::steady_clock::now();
              auto measured = util::fail_guard([&]() {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
                counters.encode_ms += ms;
                counters.max_encode_ms = std::max(counters.max_encode_ms, ms);
                interval_max_encode_ms = std::max(interval_max_encode_ms, ms);
              });
              data = encode(frame);
            } catch (const FrameTooLarge &e) {
              if (++counters.budget_drops == 1 || counters.budget_drops % 60 == 0) {
                BOOST_LOG(warning) << "Pyrowave dropped independent frame beyond transport budget (total " << counters.budget_drops << "): " << e.what();
              }
              continue;
            }
            auto packet = std::make_unique<video::packet_raw_generic>(std::move(data), frame, true);
            packet->channel_data = channel_data;
            packet->frame_timestamp = capture_timestamp;
            packet->broadcast_ticket = std::move(ticket);
            if (!shutdown->peek()) {
              packets->raise(std::move(packet));
            }
            if (frame == std::numeric_limits<uint32_t>::max()) {
              break;  // Reconnect rather than wrap a frame number.
            }
            ++frame;
          }
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Pyrowave live capture stopped: " << e.what();
        }
      }

      void drain(void *channel_data) override {
        window->close();
        auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
        packets->discard_if([&](const video::packet_t &packet) {
          return packet->channel_data == channel_data;
        });
        // A broadcaster-owned packet still uses the raw session pointer. Join
        // cannot release that pointer until the packet's acknowledgement dies.
        while (!window->wait_drained(100ms)) {}
        if (counters_started != std::chrono::steady_clock::time_point {}) {
          report_counters(true);
        }
      }

      void record_emitted(size_t bytes) override {
        payload_bytes.fetch_add(bytes, std::memory_order_relaxed);
        emitted_frames.fetch_add(1, std::memory_order_relaxed);
      }

    private:
      struct Counters {
        uint64_t attempts = 0, budget_drops = 0, backpressure_drops = 0;
        double encode_ms = 0, max_encode_ms = 0;
        uint64_t emitted = 0, bytes = 0;
      };

      void report_counters(bool final) {
        const auto now = std::chrono::steady_clock::now();
        if (!final && now - counters_reported < 5s) {
          return;
        }
        counters.emitted = emitted_frames.load(std::memory_order_relaxed);
        counters.bytes = payload_bytes.load(std::memory_order_relaxed);
        const auto previous = final ? Counters {} : last_report;
        const double seconds = std::chrono::duration<double>(now - (final ? counters_started : counters_reported)).count();
        const auto attempts = counters.attempts - previous.attempts;
        BOOST_LOG(info) << "Pyrowave host counters " << (final ? "final" : "interval") << " session=" << this
                        << " seconds=" << seconds << " requested FPS=60, host emission FPS=" << (seconds > 0 ? (counters.emitted - previous.emitted) / seconds : 0)
                        << " frames emitted=" << counters.emitted - previous.emitted << " capture/encode attempts=" << attempts
                        << " budget drops=" << counters.budget_drops - previous.budget_drops << " backpressure drops=" << counters.backpressure_drops - previous.backpressure_drops
                        << " capture+encode ms mean=" << (attempts ? (counters.encode_ms - previous.encode_ms) / attempts : 0)
                        << " max=" << (final ? counters.max_encode_ms : interval_max_encode_ms)
                        << " payload bytes=" << counters.bytes - previous.bytes << " payload Mbps=" << (seconds > 0 ? (counters.bytes - previous.bytes) * 8 / seconds / 1'000'000 : 0);
        last_report = counters;
        counters_reported = now;
        interval_max_encode_ms = 0;
      }

      std::vector<uint8_t> encode(uint32_t frame) {
        auto image = source->next();
        if (!image) {
          throw std::runtime_error("Pyrowave source returned no captured image");
        }
        const auto &sd = image->sd;
        pyrowave_diag::layout_t layout;
        layout.width = sd.width;
        layout.height = sd.height;
        layout.fourcc = sd.fourcc;
        layout.modifier = sd.modifier;
        std::copy_n(sd.fds, 4, layout.fds.begin());
        std::copy_n(sd.pitches, 4, layout.pitches.begin());
        std::copy_n(sd.offsets, 4, layout.offsets.begin());
        if (input_layout && !pyrowave_diag::same_layout(*input_layout, layout)) {
          BOOST_LOG(info) << "Pyrowave captured framebuffer layout change: " << pyrowave_diag::describe_layout(*input_layout)
                          << " -> " << pyrowave_diag::describe_layout(layout);
        }
        pyrowave_diag::validate_live_layout(layout, input_layout ? &*input_layout : nullptr, source->compositor_owned());
        capture_size = {int(layout.width), int(layout.height)};
        capture_timestamp = image->frame_timestamp;
        gpu.capture_lifetime = image;
        try {
          gpu.import(layout);  // Actual-FD memory-type intersection and error-fence checks preserved.
        } catch (const std::exception &e) {
          throw std::runtime_error("Framebuffer import rejected (" + pyrowave_diag::describe_layout(layout) + "): " + e.what());
        }
        gpu.snapshot();
        source->snapshot_complete();
        if (pyrowave_diag::requires_opaque_alpha(layout.fourcc)) {
          gpu.validate_alpha();
        }
        pyrowave_scaled_encode_info scale {};
        scale.view = gpu.snapshot_view();
        scale.input_color_space = scale.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        scale.intermediate_plane_format = pyrowave_diag::live_intermediate_format;
        scale.ycbcr_chroma_midpoint = 128.0f / 255.0f;
        scale.force_linear_filtering = true;
        scale.skip_dither = true;
        // Reserve all possible record headers; actual output is checked again.
        pyrowave_rate_control rate {limits.codec_target_bytes()};
        pyrowave_gpu_sync_operation release {};
        release.sync = {gpu.completion_semaphore(), ++encode_sequence};
        checked(pyrowave_encoder_encode_gpu_scaled_synchronous(gpu.encoder, nullptr, &release, &scale, &rate), "live scale/encode");
        gpu.wait_encode(encode_sequence);
        gpu.release_import();
        source->encode_complete();
        input_layout = layout;
        // The pinned codec keeps blocks intact even when larger than its packing
        // target. This target is independent of RTP's MTU and the record cap;
        // low-bandwidth sessions need not have room for a maximum-size record.
        const size_t packing_target = std::min(max_codec_record_size, limits.frame_bytes - header_size - 4);
        size_t count = 0;
        checked(pyrowave_encoder_compute_num_packets(gpu.encoder, packing_target, &count), "live packet count");
        if (!count || count > max_packets) {
          throw FrameTooLarge("Codec packet count exceeds envelope limit");
        }
        // The pinned packetizer assumes the supplied buffer fits. Query actual
        // metadata lengths first; do not trust rate-control or assertions for bounds.
        // This internal layout is pinned and declared by pyrowave_common.hpp.
        const void *raw = nullptr, *metadata = nullptr;
        size_t raw_size = 0, metadata_size = 0;
        checked(pyrowave_encoder_get_mapped_raw_bitstream(gpu.encoder, &raw, &raw_size, &metadata, &metadata_size), "live bitstream bounds");
        if (!raw || !metadata || metadata_size != raw_block_count(output) * sizeof(RawBlock)) {
          throw std::runtime_error("Unexpected codec metadata layout");
        }
        uint64_t bytes = 8 /* pinned BitstreamSequenceHeader */;
        auto blocks = static_cast<const RawBlock *>(metadata);
        for (size_t i = 0; i < metadata_size / sizeof(*blocks); ++i) {
          if (uint64_t(blocks[i].offset_u32) * 4 + uint64_t(blocks[i].num_words) * 4 > raw_size) {
            throw std::runtime_error("Codec block outside mapped bitstream");
          }
          bytes += uint64_t(blocks[i].num_words) * 4;
        }
        if (bytes + header_size + count * 4 > limits.frame_bytes) {
          throw FrameTooLarge("Actual codec bytes exceed frame budget");
        }
        std::vector<uint8_t> bitstream(bytes);
        std::vector<pyrowave_packet> layout_packets(count);
        size_t out_count = count;
        checked(pyrowave_encoder_packetize(gpu.encoder, layout_packets.data(), packing_target, &out_count, bitstream.data(), bitstream.size()), "live packetize");
        if (out_count != count) {
          throw std::runtime_error("Codec packet count changed after encode completion");
        }
        std::vector<PacketView> views;
        size_t end = 0;
        for (const auto &p : layout_packets) {
          if (p.offset != end || p.size > bitstream.size() - end) {
            throw std::runtime_error("Invalid codec packet layout");
          }
          if (!p.size || p.size > max_codec_record_size) {
            throw FrameTooLarge("A codec record exceeds the 64 KiB limit or is empty");
          }
          views.push_back({bitstream.data() + p.offset, p.size});
          end += p.size;
        }
        if (end != bitstream.size()) {
          throw std::runtime_error("Incomplete codec packet layout");
        }
        if (!limits.cost(header_size + views.size() * 4 + bitstream.size()).fits) {
          throw FrameTooLarge("Actual envelope exceeds transport/FEC budget");
        }
        return envelope(frame, output, views, limits);
      }

      Dimensions output, capture_size;
      Limits limits;
      std::shared_ptr<FrameWindow> window;
      std::unique_ptr<platf::kms_diagnostic_source_t> source;
      pyrowave_diag::gpu_t gpu;  // Destroy before capture source; retains failing import's captured FDs.
      std::optional<std::chrono::steady_clock::time_point> capture_timestamp;
      uint64_t encode_sequence = 0;
      std::optional<pyrowave_diag::layout_t> input_layout;
      Counters counters, last_report;
      std::chrono::steady_clock::time_point counters_started {}, counters_reported {};
      double interval_max_encode_ms = 0;
      std::atomic<uint64_t> emitted_frames {0}, payload_bytes {0};
    };
  }  // namespace

  std::unique_ptr<Session> make_session(Dimensions output, const Limits &limits) {
    StartupWatchdog watchdog;
    return std::make_unique<CaptureSession>(output, limits);
  }
}  // namespace pyrowave
