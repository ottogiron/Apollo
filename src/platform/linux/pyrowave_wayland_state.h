/** @brief Private screencopy state machine; used unchanged by the Wayland adapter and CPU fault injection. */
#pragma once

#include "pyrowave_live_layout.h"

#include <chrono>
#include <functional>
#include <memory>

namespace pyrowave_wl {
  struct output_t {
    uint32_t id = 0;
    std::string name;
    int native_width = 0, native_height = 0;
    int x = 0, y = 0, width = 0, height = 0, transform = 0;
    bool complete = false, removed = false;

    bool active() const {
      return complete && !removed && native_width > 0 && native_height > 0 && width > 0 && height > 0;
    }
  };

  inline output_t select_output(const std::vector<output_t> &outputs, const std::string &name, const std::string &app_name) {
    std::vector<output_t> matches;
    for (const auto &o : outputs) {
      if (o.active() && (name.empty() || o.name == name)) {
        matches.push_back(o);
      }
    }
    if (matches.size() != 1 || matches[0].name.empty()) {
      throw std::runtime_error("Pyrowave Wayland output is absent or ambiguous; set pyrowave_output_name to an active connector name (never a KMS index)");
    }
    const auto &o = matches[0];
    if (!app_name.empty() && app_name != o.name) {
      throw std::runtime_error("Conventional per-app display override cannot select Pyrowave Wayland output; use the matching connector name or remove the override");
    }
    if (o.transform != 0 || !pyrowave::supported_capture({o.native_width, o.native_height}) || o.native_width > 2560 || o.native_height > 1440 || o.width > 32768 || o.height > 32768 || std::abs(int64_t(o.x)) > 32768 || std::abs(int64_t(o.y)) > 32768 || std::abs(double(o.native_width) / o.width - double(o.native_height) / o.height) > 0.01) {
      throw std::runtime_error("Pyrowave Wayland requires untransformed native 1080p/1440p SDR with a uniform logical input scale; native4K/HDR are deferred");
    }
    return o;
  }

  struct destination_t {
    virtual ~destination_t() = default;
    pyrowave_diag::layout_t layout;
  };

  // No requests overlap. Callbacks use a generation so cancelled/late events
  // cannot modify the next request. The adapter destroys proxies before any
  // listener data, on its single dedicated dispatch thread.
  class transport_t {
  public:
    virtual ~transport_t() = default;
    virtual void request(uint64_t generation) = 0;
    virtual std::shared_ptr<destination_t> allocate(uint32_t format, uint32_t width, uint32_t height) = 0;
    virtual void create_buffer(uint64_t generation, const destination_t &destination) = 0;
    virtual void copy(uint64_t generation) = 0;
    virtual void cancel() noexcept = 0;  // frame and asynchronous buffer params/listeners
    virtual void disconnect() noexcept = 0;  // destroys buffer, then the connection
    virtual bool dispatch(std::chrono::steady_clock::time_point deadline) = 0;
  };

  class capture_t {
  public:
    enum class phase_t {
      idle,
      offered,
      params,
      copying,
      ready,
      snapshot,
      failed
    };

    capture_t(transport_t &transport, output_t output):
        transport(transport),
        output(std::move(output)) {}

    ~capture_t() {
      // Keep GBM/FDs alive through proxy cancellation and connection teardown.
      transport.cancel();
      transport.disconnect();
    }

    capture_t(const capture_t &) = delete;
    capture_t &operator=(const capture_t &) = delete;

    // Longest wait for one screencopy `ready`. A caller may ask for less.
    static constexpr std::chrono::milliseconds ready_timeout {1000};

    std::shared_ptr<destination_t> next(std::chrono::milliseconds timeout = ready_timeout) {
      if (phase != phase_t::idle) {
        throw std::runtime_error("Pyrowave destination cannot be rewritten before snapshot and encode completion, or after capture failure");
      }
      ++generation;
      phase = phase_t::offered;
      seen_dmabuf = seen_flags = false;
      const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, ready_timeout);
      try {
        transport.request(generation);
        while (phase != phase_t::ready && phase != phase_t::failed) {
          if (std::chrono::steady_clock::now() >= deadline || !transport.dispatch(deadline)) {
            fail("Pyrowave screencopy timeout; check compositor capture permission for the resolved versioned Apollo executable");
          }
        }
        if (phase == phase_t::failed) {
          throw std::runtime_error(error);
        }
        transport.cancel();
        return destination;
      } catch (...) {
        fail("Pyrowave Wayland request/allocation/connection failed");
        transport.cancel();
        throw;
      }
    }

    // Called at the actual C listener boundary. Exceptions are converted by
    // the adapter to fail(), never unwound through libwayland.
    void dmabuf(uint64_t token, uint32_t format, uint32_t width, uint32_t height) {
      if (!current(token)) {
        return;
      }
      if (phase != phase_t::offered || seen_dmabuf || width != uint32_t(output.native_width) || height != uint32_t(output.native_height) || (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_XBGR8888 && format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_ABGR8888 && format != DRM_FORMAT_XBGR2101010 && format != DRM_FORMAT_XRGB2101010 && format != DRM_FORMAT_ARGB2101010 && format != DRM_FORMAT_ABGR2101010)) {
        fail("Unsupported screencopy DMA-BUF format/physical extent; only opaque packed RGB native SDR is supported");
        return;
      }
      seen_dmabuf = true;
      offered_format = format;
    }

    void buffer_done(uint64_t token) {
      if (!current(token)) {
        return;
      }
      if (phase != phase_t::offered || !seen_dmabuf) {
        fail("Compositor did not offer a valid screencopy GPU buffer; SHM fallback is deferred");
        return;
      }
      if (!destination) {
        destination = transport.allocate(offered_format, output.native_width, output.native_height);
        if (!destination) {
          throw std::runtime_error("No compositor/GBM/Vulkan compatible DMA-BUF destination");
        }
        pyrowave_diag::validate_layout(destination->layout, true);
        if (destination->layout.width != uint32_t(output.native_width) || destination->layout.height != uint32_t(output.native_height) || destination->layout.fourcc != offered_format) {
          throw std::runtime_error("GBM destination does not match the offered capture shape");
        }
        phase = phase_t::params;
        transport.create_buffer(generation, *destination);
      } else {
        if (destination->layout.fourcc != offered_format) {
          fail("Screencopy format changed; reconnect required");
          return;
        }
        phase = phase_t::copying;
        transport.copy(generation);
      }
    }

    void created(uint64_t token) {
      if (!current(token)) {
        return;
      }
      if (phase != phase_t::params) {
        fail("Unexpected asynchronous DMA-BUF buffer callback");
        return;
      }
      phase = phase_t::copying;
      transport.copy(generation);
    }

    void flags(uint64_t token, uint32_t flags) {
      if (!current(token)) {
        return;
      }
      if (phase != phase_t::copying || seen_flags || flags != 0) {
        fail("Unsupported screencopy flags/y-invert; reconnect with an untransformed output");
        return;
      }
      seen_flags = true;
    }

    void ready(uint64_t token, uint32_t nanoseconds) {
      if (!current(token)) {
        return;
      }
      if (phase != phase_t::copying || !seen_flags || nanoseconds >= 1'000'000'000) {
        fail("Malformed or premature screencopy ready event");
        return;
      }
      phase = phase_t::ready;
    }

    void failed(uint64_t token) {
      if (current(token)) {
        fail("Compositor screencopy failed; check capture permission, output and GPU support");
      }
    }

    void fail(const std::string &reason) {
      if (phase != phase_t::failed) {
        error = reason;
      }
      phase = phase_t::failed;
    }

    void snapshot_complete() {
      if (phase != phase_t::ready) {
        throw std::runtime_error("Snapshot completion without screencopy ready");
      }
      phase = phase_t::snapshot;
    }

    void encode_complete() {
      if (phase != phase_t::snapshot) {
        throw std::runtime_error("Encode completion before snapshot fence completion");
      }
      phase = phase_t::idle;
    }

    phase_t state() const {
      return phase;
    }

    uint64_t token() const {
      return generation;
    }

  private:
    bool current(uint64_t token) const {
      return generation == token && phase != phase_t::failed && phase != phase_t::idle;
    }

    transport_t &transport;
    output_t output;
    std::shared_ptr<destination_t> destination;
    phase_t phase = phase_t::idle;
    uint64_t generation = 0;
    uint32_t offered_format = 0;
    bool seen_dmabuf = false, seen_flags = false;
    std::string error;
  };
}  // namespace pyrowave_wl
