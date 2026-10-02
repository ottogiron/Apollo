/** @brief Bounded, local-only capture/encode/decode experiment; never a host service. */
#include "pyrowave_capture.h"
#include "pyrowave_diagnostic_vulkan.h"

#include <chrono>
#include <csignal>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <optional>
#include <sstream>
#include <thread>

namespace {
  using namespace pyrowave_diag;
  using diagnostic_clock = std::chrono::steady_clock;
  using json = nlohmann::json;
  volatile std::sig_atomic_t interrupted = 0;

  void interrupt(int) {
    interrupted = 1;
  }

  struct options_t {
    bool synthetic = false, synthetic_10bit = false;
    std::string display = "0", report;
    int warmup = 30, frames = 120, mbps = 250, reference_every = 60, seconds = 120;
  };

  void help() {
    std::cout << "Opt-in Linux Apollo KMS/Pyrowave local diagnostic\n"
                 "  --synthetic              synthetic RGB upload; never real-capture evidence\n"
                 "  --synthetic-10bit         synthetic opaque ABGR2101010 SDR upload\n"
                 "  --display N              Apollo KMS display index (default 0)\n"
                 "  --warmup N               0..600 frames (default 30)\n"
                 "  --frames N               1..3600 measured frames (default 120)\n"
                 "  --mbps N                 10..1000 payload budget at 60 fps (default 250)\n"
                 "  --reference-every N      1..3600; compare first and every Nth measured frame (default 60)\n"
                 "  --seconds N              1..600 wall-clock bound between API calls (default 120)\n"
                 "  --report PATH            JSON report; keep private evidence outside tracked source\n"
                 "  --help                   no GPU or KMS initialization\n"
                 "Output is 1920x1080 at nominal 60 fps, SDR BT.709 full-range YUV420.\n"
                 "Cursor and compositor-exclusive ownership are unimplemented; Phase 3 remains open.\n";
  }

  options_t parse(int argc, char **argv) {
    options_t options;
    for (int i = 1; i < argc; ++i) {
      std::string flag = argv[i];
      if (flag == "--synthetic" || flag == "--synthetic-10bit") {
        options.synthetic_10bit = flag == "--synthetic-10bit";
        options.synthetic = true;
        continue;
      }
      if (i + 1 == argc) {
        throw std::runtime_error("Missing value for " + flag);
      }
      std::string value = argv[++i];
      if (flag == "--frames") {
        options.frames = bounded_number(value, 1, 3600);
      } else if (flag == "--warmup") {
        options.warmup = bounded_number(value, 0, 600);
      } else if (flag == "--mbps") {
        options.mbps = bounded_number(value, 10, 1000);
      } else if (flag == "--reference-every") {
        options.reference_every = bounded_number(value, 1, 3600);
      } else if (flag == "--seconds") {
        options.seconds = bounded_number(value, 1, 600);
      } else if (flag == "--display") {
        options.display = std::to_string(bounded_number(value, 0, 128));
      } else if (flag == "--report") {
        options.report = value;
      } else {
        throw std::runtime_error("Unknown option: " + flag);
      }
    }
    return options;
  }

  double ms(diagnostic_clock::time_point start, diagnostic_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
  }

  pyrowave_cpu_buffer output_buffer(planes_t &planes) {
    pyrowave_cpu_buffer buffer {};
    buffer.width = output_width;
    buffer.height = output_height;
    buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    for (int i = 0; i < 3; ++i) {
      buffer.data[i] = planes[i].data();
      buffer.row_stride_in_bytes[i] = i ? output_width / 2 : output_width;
      buffer.plane_size_in_bytes[i] = planes[i].size();
    }
    return buffer;
  }

  std::vector<uint8_t> synthetic_pixels(int frame, bool ten_bit) {
    // 1440p -> 1080p exercises actual GPU scaling. RGB, including flat primary
    // blocks, ramps and a moving grid; X is deliberately not always 255.
    constexpr int w = 2560, h = 1440;
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        auto p = &pixels[(size_t(y) * w + x) * 4];
        if (y < h / 2) {
          int block = x * 4 / w;
          p[0] = block == 0 || block == 3 ? 255 : 0;
          p[1] = block == 1 || block == 3 ? 255 : 0;
          p[2] = block == 2 || block == 3 ? 255 : 0;
        } else {
          p[0] = uint8_t((x + frame) % 256);
          p[1] = uint8_t(y % 256);
          p[2] = uint8_t(((x + frame) / 32 + y / 32) % 2 ? 200 : 32);
        }
        p[3] = uint8_t((x + y + frame) % 256);
        if (ten_bit) {
          uint32_t packed = (3u << 30) | (uint32_t(p[0]) * 1023 / 255) |
                            ((uint32_t(p[1]) * 1023 / 255) << 10) | ((uint32_t(p[2]) * 1023 / 255) << 20);
          std::memcpy(p, &packed, sizeof(packed));
        }
      }
    }
    return pixels;
  }

  layout_t layout_of(const egl::surface_descriptor_t &surface) {
    layout_t layout;
    layout.width = surface.width;
    layout.height = surface.height;
    layout.fourcc = surface.fourcc;
    layout.modifier = surface.modifier;
    std::copy_n(surface.fds, 4, layout.fds.begin());
    std::copy_n(surface.pitches, 4, layout.pitches.begin());
    std::copy_n(surface.offsets, 4, layout.offsets.begin());
    return layout;
  }

  size_t open_fds() {
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      (void) entry;
      ++count;
    }
    return count;
  }

  void write_report(const json &report, const std::string &path) {
    auto text = report.dump(2) + '\n';
    if (!path.empty()) {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      if (!output || !(output << text)) {
        throw std::runtime_error("Cannot write JSON report");
      }
    }
    std::cout << text;
  }

  std::string runtime_library_hash() {
    Dl_info library {};
    if (!dladdr(reinterpret_cast<void *>(pyrowave_get_api_version), &library) || !library.dli_fname) {
      throw std::runtime_error("Cannot identify loaded Pyrowave shared library");
    }
    std::ifstream input(library.dli_fname, std::ios::binary);
    if (!input) {
      throw std::runtime_error("Cannot read loaded Pyrowave shared library");
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
      throw std::runtime_error("Cannot initialize library digest");
    }
    std::array<char, 65536> block {};
    while (input.read(block.data(), block.size()) || input.gcount()) {
      if (EVP_DigestUpdate(context.get(), block.data(), size_t(input.gcount())) != 1) {
        throw std::runtime_error("Cannot hash loaded library");
      }
    }
    if (!input.eof()) {
      throw std::runtime_error("Loaded library read failed");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1) {
      throw std::runtime_error("Cannot finish library digest");
    }
    std::ostringstream hex;
    for (unsigned int i = 0; i < size; ++i) {
      hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
    }
    return hex.str();
  }

  void summarize(json &report) {
    const auto &samples = report["samples"];
    report["summary"] = json::object();
    if (samples.empty()) {
      return;
    }
    for (const auto &key : {"pipeline_ms", "diagnostic_frame_ms", "capture_or_synthetic_generation_ms", "import_and_producer_wait_ms", "producer_wait_ms", "snapshot_copy_ms", "reference_gpu_readback_ms", "alpha_check_cpu_ms", "scale_convert_encode_and_bitstream_wait_ms", "packetize_and_push_ms", "decode_and_cpu_readback_ms", "reference_cpu_compare_ms"}) {
      std::vector<double> values;
      for (const auto &sample : samples) {
        if (sample.contains(key)) {
          values.push_back(sample[key].get<double>());
        }
      }
      if (values.empty()) {
        continue;
      }
      std::sort(values.begin(), values.end());
      auto percentile = [&](double p) {
        return values[std::max<size_t>(1, size_t(std::ceil(values.size() * p))) - 1];
      };
      report["summary"][key] = {{"count", values.size()}, {"p50", percentile(0.50)}, {"p95", percentile(0.95)}, {"p99", percentile(0.99)}, {"maximum", values.back()}};
    }
    report["summary"]["percentile_method"] = "nearest rank; warmup excluded; short runs limit tail evidence";
    for (const auto &key : {"pipeline_ms", "diagnostic_frame_ms"}) {
      size_t overruns = 0;
      for (const auto &sample : samples) {
        if (sample[key].get<double>() > 1000.0 / fps) {
          ++overruns;
        }
      }
      report["summary"][std::string(key) + "_overruns_16_67ms"] = overruns;
    }
  }
}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--help") {
      help();
      return 0;
    }
  }
  options_t options;
  json report = {
    {"schema", 1},
    {"status", "failed"},
    {"phase3_gate", "not_claimed"},
    {"pyrowave_pin", APOLLO_PYROWAVE_PIN},
    {"configured_library_sha256", APOLLO_PYROWAVE_LIBRARY_SHA256},
    {"output", {{"width", output_width}, {"height", output_height}, {"nominal_fps", fps}, {"color", "SDR sRGB RGB -> BT.709 full-range, center-sited YUV420, chroma midpoint 128/255"}}},
    {"cursor", "unimplemented; separate KMS cursor plane omitted"},
    {"producer_ownership", "unproven: reservation writer wait, FOREIGN acquire/release and published READ completion do not prevent compositor reuse during the non-atomic snapshot bridge"},
    {"capture_count_semantics", "polls; not proof of unique compositor presentations"},
    {"zero_copy", false},
    {"quality_pass", nullptr},
    {"fps_pass", nullptr},
    {"validation", "lossy local decode + approximate independent CPU reference from the exact owned RGB snapshot; no pixel-exact or subjective-quality claim"},
    {"copies", "live: DMA-BUF -> owned RGB image; sampled frames: RGB -> CPU reference; scaler: RGB -> GPU YUV420 planes; codec bitstream -> CPU; every decode: GPU YUV -> CPU"},
    {"timing_scope", "CPU wall times with GPU waits; encode includes scaling/conversion and internal bitstream readback; decode includes output readback; no kernel/end-to-end latency claim"},
    {"samples", json::array()},
    {"warmup_completed", 0},
    {"measured_completed", 0},
    {"captured", 0},
    {"imported", 0},
    {"encoded", 0},
    {"decoded", 0},
    {"reference_comparisons", 0}
  };
  int exit_code = 1;
  auto run_start = diagnostic_clock::now();
  std::optional<size_t> baseline_fds;
  try {
    options = parse(argc, argv);
    report["source"] = options.synthetic ? "synthetic_rgb_upload" : "real_kms_dmabuf";
    report["real_capture_tested"] = false;
    report["requested_warmup"] = options.warmup;
    report["requested_frames"] = options.frames;
    report["reference_every"] = options.reference_every;
    report["seconds_bound"] = options.seconds;
    size_t budget = size_t(options.mbps) * 1'000'000 / 8 / fps;
    report["payload_budget_bytes"] = budget & ~size_t(3);
    auto runtime_hash = runtime_library_hash();
    report["runtime_library_sha256"] = runtime_hash;
    if (runtime_hash != APOLLO_PYROWAVE_LIBRARY_SHA256) {
      throw std::runtime_error("Loaded Pyrowave binary differs from configured pin's library hash; reconfigure with the pinned build");
    }
    uint32_t major = 0, minor = 0, patch = 0;
    pyrowave_get_api_version(&major, &minor, &patch);
    if (major != 0 || minor != 6 || patch != 0) {
      throw std::runtime_error("Runtime Pyrowave API version differs from pinned 0.6.0");
    }
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    auto deadline = run_start + std::chrono::seconds(options.seconds);
    baseline_fds = open_fds();
    {
      std::unique_ptr<platf::kms_diagnostic_source_t> capture;
      platf::kms_diagnostic_info_t identity {};
      // Capture source outlives GPU resources on all error paths.
      if (!options.synthetic) {
        capture = platf::make_kms_diagnostic_source(options.display);
        identity = capture->info();
      }
      gpu_t gpu;
      gpu.init(options.synthetic ? nullptr : &identity);
      report["same_gpu_checked"] = gpu.same_gpu_checked;
      report["producer_sync"] = options.synthetic ? "owned synthetic upload" : "DMA_BUF_IOCTL_EXPORT_SYNC_FILE READ + bounded CPU poll; FOREIGN queue transfer; copy-completion READ fence publication";
      auto decoded = make_planes();
      auto buffer = output_buffer(decoded);
      pyrowave_rate_control rate {budget};
      std::vector<uint8_t> bitstream(budget + 65536);
      std::vector<pyrowave_packet> packets;
      std::vector<layout_t> seen_layouts;
      auto scheduled = diagnostic_clock::now();
      auto measured_start = scheduled;
      auto measured_end = scheduled;
      for (int frame = 0; frame < options.warmup + options.frames; ++frame) {
        if (interrupted) {
          throw std::runtime_error("Interrupted; resources are being released");
        }
        if (diagnostic_clock::now() >= deadline) {
          throw std::runtime_error("Diagnostic wall-clock bound reached");
        }
        std::this_thread::sleep_until(scheduled);
        bool measured = frame >= options.warmup;
        int index = frame - options.warmup;
        bool compare_reference = measured && (index % options.reference_every == 0);
        if (frame == options.warmup) {
          measured_start = diagnostic_clock::now();
        }
        auto start = diagnostic_clock::now();
        // Declared before the frame's operations and retained through encode
        // completion, packetization, decode and comparison, including exceptions.
        std::shared_ptr<egl::img_descriptor_t> captured;
        std::vector<uint8_t> synthetic;
        layout_t layout;
        VkFormat format;
        if (options.synthetic) {
          layout.width = 2560;
          layout.height = 1440;
          layout.fourcc = options.synthetic_10bit ? DRM_FORMAT_ABGR2101010 : DRM_FORMAT_XBGR8888;
          format = options.synthetic_10bit ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM;
          synthetic = synthetic_pixels(frame, options.synthetic_10bit);
          gpu.prepare(layout.width, layout.height, format);
        } else {
          captured = capture->next();
          report["captured"] = report["captured"].get<int>() + 1;
          layout = layout_of(captured->sd);
          format = validate_layout(layout);
          if (std::none_of(seen_layouts.begin(), seen_layouts.end(), [&](const auto &previous) {
                return previous.fourcc == layout.fourcc && previous.modifier == layout.modifier &&
                       previous.pitches == layout.pitches && previous.offsets == layout.offsets;
              })) {
            seen_layouts.push_back(layout);
            report["framebuffer_layouts"].push_back({{"width", layout.width}, {"height", layout.height}, {"fourcc", layout.fourcc}, {"modifier", layout.modifier}, {"pitch", layout.pitches[0]}, {"offset", layout.offsets[0]}});
          }
        }
        auto acquired = diagnostic_clock::now();
        if (!options.synthetic) {
          gpu.capture_lifetime = captured;
          gpu.import(layout);
          report["imported"] = report["imported"].get<int>() + 1;
        }
        auto imported = diagnostic_clock::now();
        report["reference_memory_properties"] = gpu.reference_memory_properties;
        report["reference_host_cached"] = bool(gpu.reference_memory_properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        gpu.snapshot(options.synthetic ? &synthetic : nullptr);
        auto copied = diagnostic_clock::now();
        bool alpha_check = requires_opaque_alpha(layout.fourcc);
        if (compare_reference || alpha_check) {
          gpu.readback_snapshot();
        }
        auto referenced = diagnostic_clock::now();
        std::vector<uint8_t> reference_input;
        if (compare_reference || alpha_check) {
          reference_input = gpu.reference_pixels();
        }
        if (alpha_check) {
          validate_opaque_alpha(reference_input);
        }
        auto alpha_checked = diagnostic_clock::now();
        pyrowave_scaled_encode_info scale {};
        scale.view = gpu.snapshot_view();
        scale.input_color_space = scale.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        scale.intermediate_plane_format = packed_10bit(format) ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
        scale.ycbcr_chroma_midpoint = 128.0f / 255.0f;
        scale.force_linear_filtering = true;
        scale.skip_dither = true;
        // Snapshot is already owned by our queue; no external acquire is needed.
        // Pyrowave's release synchronization signals completion after bitstream copy.
        pyrowave_gpu_sync_operation release {};
        release.sync = {gpu.completion_semaphore(), uint64_t(frame + 1)};
        checked(pyrowave_encoder_encode_gpu_scaled_synchronous(gpu.encoder, nullptr, &release, &scale, &rate), "scaled GPU encode");
        gpu.wait_encode(uint64_t(frame + 1));
        size_t count = 0;
        checked(pyrowave_encoder_compute_num_packets(gpu.encoder, 1200, &count), "compute packets (encode fence complete)");
        auto encoded = diagnostic_clock::now();
        report["encoded"] = report["encoded"].get<int>() + 1;
        // Imported image and captured FDs remain valid until after encode waits.
        gpu.release_import();
        if (!count || count > bitstream.size()) {
          throw std::runtime_error("Invalid packet count");
        }
        packets.resize(count);
        size_t out_count = count;
        checked(pyrowave_encoder_packetize(gpu.encoder, packets.data(), 1200, &out_count, bitstream.data(), bitstream.size()), "packetize locally");
        if (out_count != count) {
          throw std::runtime_error("Packet count changed");
        }
        pyrowave_decoder_clear(gpu.decoder);
        size_t payload = 0;
        for (const auto &packet : packets) {
          if (!packet.size || packet.offset != payload || packet.offset > bitstream.size() || packet.size > bitstream.size() - packet.offset) {
            throw std::runtime_error("Invalid packet bounds/order");
          }
          checked(pyrowave_decoder_push_packet(gpu.decoder, bitstream.data() + packet.offset, packet.size), "push local packet");
          payload += packet.size;
        }
        if (payload > (budget & ~size_t(3))) {
          throw std::runtime_error("Encoded payload exceeds frame budget");
        }
        if (!pyrowave_decoder_decode_is_ready(gpu.decoder, false)) {
          throw std::runtime_error("Local decoder frame incomplete");
        }
        auto packetized = diagnostic_clock::now();
        checked(pyrowave_decoder_decode_cpu_buffer_synchronous(gpu.decoder, &buffer), "local decode and readback");
        auto readback = diagnostic_clock::now();
        report["decoded"] = report["decoded"].get<int>() + 1;
        json sample = {{"frame", index}, {"payload_bytes", payload}, {"input_precision_bits", packed_10bit(format) ? 10 : 8}, {"scaler_plane_bits", packed_10bit(format) ? 16 : 8}, {"packets", count}, {"capture_or_synthetic_generation_ms", ms(start, acquired)}, {"import_and_producer_wait_ms", ms(acquired, imported)}, {"producer_wait_ms", options.synthetic ? 0 : gpu.producer_wait_ms}, {"snapshot_copy_ms", ms(imported, copied)}, {"reference_gpu_readback_ms", ms(copied, referenced)}, {"reference_readback_included", compare_reference}, {"opaque_alpha_checked", alpha_check}, {"alpha_check_cpu_ms", ms(referenced, alpha_checked)}, {"scale_convert_encode_and_bitstream_wait_ms", ms(alpha_checked, encoded)}, {"packetize_and_push_ms", ms(encoded, packetized)}, {"decode_and_cpu_readback_ms", ms(packetized, readback)}, {"pipeline_ms", ms(start, readback)}};
        if (!options.synthetic) {
          sample["import_memory_type"] = {{"image_requirement_bits", gpu.import_image_type_bits}, {"fd_compatible_bits", gpu.import_fd_type_bits}, {"index", gpu.import_type_index}, {"dedicated", true}};
        }
        if (compare_reference) {
          auto reference_planes = reference(reference_input, layout.width, layout.height, format);
          json errors = json::array();
          for (int i = 0; i < 3; ++i) {
            auto e = compare(reference_planes[i], decoded[i]);
            errors.push_back({{"plane", i}, {"mae", e.mae}, {"mse", e.mse}, {"max_error", e.maximum}, {"psnr_db", e.mse == 0 ? json(nullptr) : json(10 * std::log10(255.0 * 255.0 / e.mse))}});
          }
          sample["reference_errors"] = errors;
          sample["reference_cpu_compare_ms"] = ms(readback, diagnostic_clock::now());
          report["reference_comparisons"] = report["reference_comparisons"].get<int>() + 1;
        }
        sample["diagnostic_frame_ms"] = ms(start, diagnostic_clock::now());
        if (measured) {
          report["samples"].push_back(sample);
          report["measured_completed"] = report["measured_completed"].get<int>() + 1;
          measured_end = diagnostic_clock::now();
        } else {
          report["warmup_completed"] = report["warmup_completed"].get<int>() + 1;
        }
        captured.reset();
        scheduled += std::chrono::nanoseconds(1'000'000'000 / fps);
        if (scheduled < diagnostic_clock::now()) {
          scheduled = diagnostic_clock::now();
        }
        if (diagnostic_clock::now() >= deadline) {
          throw std::runtime_error("Diagnostic wall-clock bound reached");
        }
      }
      // Include the last nominal frame interval even for a one-frame run.
      std::this_thread::sleep_until(scheduled);
      if (interrupted) {
        throw std::runtime_error("Interrupted; resources are being released");
      }
      measured_end = diagnostic_clock::now();
      auto elapsed = ms(measured_start, measured_end) / 1000;
      if (!(elapsed > 0) || !std::isfinite(elapsed)) {
        throw std::runtime_error("Invalid measured elapsed time");
      }
      report["measured_elapsed_seconds"] = elapsed;
      report["diagnostic_completed_fps"] = options.frames / elapsed;
      report["real_capture_tested"] = !options.synthetic;
      report["status"] = "completed_with_open_gates";
    }
    exit_code = 0;
  }

  catch (const std::exception &error) {
    report["error"] = error.what();
    report["interrupted"] = interrupted != 0;
    std::cerr << error.what() << '\n';
    exit_code = interrupted ? 130 : 1;
  }

  try {
    if (baseline_fds) {
      auto after_cleanup = open_fds();
      report["open_fds_before_init"] = *baseline_fds;
      report["open_fds_after_cleanup"] = after_cleanup;
      report["fd_growth_after_cleanup"] = int64_t(after_cleanup) - int64_t(*baseline_fds);
      report["cleanup"] = "RAII completed; FD count is observational, not proof of absence of GPU/driver leaks";
    }
    report["total_elapsed_seconds"] = ms(run_start, diagnostic_clock::now()) / 1000;
    summarize(report);
    write_report(report, options.report);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return exit_code;
}
