/** @brief Bounded private Wayland screencopy adapter for opted-in Pyrowave sessions. */
#include "pyrowave_wayland.h"

#include "linux-dmabuf-unstable-v1.h"
#include "wlr-screencopy-unstable-v1.h"
#include "xdg-output-unstable-v1.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>

namespace pyrowave_wl {
  namespace {
    struct device_t {
      int fd = -1;
      gbm_device *gbm = nullptr;

      ~device_t() {
        if (gbm) {
          gbm_device_destroy(gbm);
        }
        if (fd >= 0) {
          close(fd);
        }
      }
    };

    struct format_t {
      uint32_t format, padding;
      uint64_t modifier;
    };

    static_assert(sizeof(format_t) == 16);

    // DRM feedback may name primary or render nodes of the same GPU.
    dev_t render_node(dev_t device, std::string *path = nullptr, dev_t *primary = nullptr) {
      drmDevicePtr drm = nullptr;
      if (drmGetDeviceFromDevId(device, 0, &drm) || !drm) {
        throw std::runtime_error("Cannot resolve compositor DMA-BUF device metadata");
      }
      auto cleanup = util::fail_guard([&]() {
        drmFreeDevice(&drm);
      });
      if (!(drm->available_nodes & (1 << DRM_NODE_RENDER))) {
        throw std::runtime_error("Compositor GPU has no render node");
      }
      struct stat info {};
      if (stat(drm->nodes[DRM_NODE_RENDER], &info) || !S_ISCHR(info.st_mode)) {
        throw std::runtime_error("Invalid compositor render node");
      }
      const auto result = info.st_rdev;
      if (path) {
        *path = drm->nodes[DRM_NODE_RENDER];
      }
      if (primary && (drm->available_nodes & (1 << DRM_NODE_PRIMARY))) {
        if (stat(drm->nodes[DRM_NODE_PRIMARY], &info) || !S_ISCHR(info.st_mode)) {
          throw std::runtime_error("Invalid compositor primary node");
        }
        *primary = info.st_rdev;
      }
      return result;
    }

    class adapter_t final: public transport_t {
    public:
      explicit adapter_t(validator_t validate):
          validate(std::move(validate)) {}

      ~adapter_t() {
        disconnect();
      }

      struct output_proxy_t {
        adapter_t *owner;
        wl_output *proxy = nullptr;
        zxdg_output_v1 *xdg = nullptr;
        output_t metadata;
      };

      struct request_t {
        adapter_t *owner;
        uint64_t generation;
        zwlr_screencopy_frame_v1 *frame = nullptr;
        zwp_linux_buffer_params_v1 *params = nullptr;
      };

      template<class F>
      void callback(F &&fn) noexcept {
        try {
          fn();
        } catch (const std::exception &e) {
          fail(e.what());
        } catch (...) {
          fail("Unexpected Wayland callback failure");
        }
      }

      void fail(const std::string &reason) {
        if (error.empty()) {
          error = reason;
        }
        if (capture) {
          capture->fail(error);
        }
      }

      void initialize(const std::string &name, const std::string &app) {
        display = wl_display_connect(nullptr);
        if (!display) {
          throw std::runtime_error("Pyrowave Wayland connection unavailable; import WAYLAND_DISPLAY/XDG_RUNTIME_DIR into the Apollo user service");
        }
        registry = wl_display_get_registry(display);
        if (!registry || wl_registry_add_listener(registry, &registry_listener, this)) {
          throw std::runtime_error("Cannot create Wayland registry listener");
        }
        sync();
        if (!manager || !dmabuf || !xdg_manager || !sync_id) {
          throw std::runtime_error("Pyrowave requires wlr-screencopy v3, linux-dmabuf v4 feedback, xdg-output v3 and native explicit sync (wp_linux_drm_syncobj_manager_v1); no KMS/SHM fallback");
        }
        for (auto &o : outputs) {
          o->xdg = zxdg_output_manager_v1_get_xdg_output(xdg_manager, o->proxy);
          if (!o->xdg || zxdg_output_v1_add_listener(o->xdg, &xdg_listener, o.get())) {
            throw std::runtime_error("Cannot create logical output listener");
          }
        }
        feedback = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
        if (!feedback || zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &feedback_listener, this)) {
          throw std::runtime_error("Cannot create DMA-BUF feedback listener");
        }
        sync();
        wait([&]() {
          return feedback_done;
        });
        std::vector<output_t> metadata;
        for (const auto &o : outputs) {
          metadata.push_back(o->metadata);
        }
        selected = select_output(metadata, name, app);
        for (auto &o : outputs) {
          if (o->metadata.id == selected.id) {
            selected_proxy = o->proxy;
          }
        }
        int64_t min_x = selected.x, min_y = selected.y, max_x = int64_t(selected.x) + selected.width, max_y = int64_t(selected.y) + selected.height;
        for (const auto &o : metadata) {
          if (o.active()) {
            min_x = std::min(min_x, int64_t(o.x));
            min_y = std::min(min_y, int64_t(o.y));
            max_x = std::max(max_x, int64_t(o.x) + o.width);
            max_y = std::max(max_y, int64_t(o.y) + o.height);
          }
        }
        if (max_x - min_x > 65536 || max_y - min_y > 65536) {
          throw std::runtime_error("Unsupported logical desktop geometry");
        }
        viewport = {int(selected.x - min_x), int(selected.y - min_y), selected.width, selected.height};
        desktop = {int(max_x - min_x), int(max_y - min_y)};
        std::string path;
        identity.render_device = render_node(main_device, &path, &identity.primary_device);
        device = std::make_shared<device_t>();
        device->fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        struct stat opened {};
        if (device->fd < 0 || fstat(device->fd, &opened) || !S_ISCHR(opened.st_mode) || opened.st_rdev != identity.render_device) {
          throw std::runtime_error("Cannot open the compositor-selected GPU render node");
        }
        device->gbm = gbm_create_device(device->fd);
        if (!device->gbm) {
          throw std::runtime_error("Cannot create GBM on the compositor GPU");
        }
        frozen = true;
      }

      void sync() {
        bool done = false;
        auto *proxy = wl_display_sync(display);
        if (!proxy) {
          throw std::runtime_error("Cannot synchronize Wayland startup");
        }
        const wl_callback_listener listener {[](void *data, wl_callback *, uint32_t) {
          *static_cast<bool *>(data) = true;
        }};
        auto cleanup = util::fail_guard([&]() {
          wl_callback_destroy(proxy);
        });
        if (wl_callback_add_listener(proxy, &listener, &done)) {
          throw std::runtime_error("Cannot attach startup sync listener");
        }
        wait([&]() {
          return done;
        });
      }

      template<class F>
      void wait(F complete) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!complete()) {
          if (!error.empty()) {
            throw std::runtime_error(error);
          }
          if (!dispatch(deadline)) {
            throw std::runtime_error("Pyrowave Wayland startup timed out");
          }
        }
        if (!error.empty()) {
          throw std::runtime_error(error);
        }
      }

      bool dispatch(std::chrono::steady_clock::time_point deadline) override {
        if (!display || !error.empty()) {
          if (capture) {
            capture->fail(error.empty() ? "Wayland disconnected" : error);
          }
          return false;
        }
        // Every prepare_read is paired with read_events or cancel_read, even
        // on flush EAGAIN, EINTR, timeout, malformed event or disconnect.
        const int dispatched = wl_display_dispatch_pending(display);
        if (dispatched < 0) {
          throw std::runtime_error("Wayland dispatch failed/disconnected");
        }
        if (dispatched > 0) {
          return true;
        }
        if (wl_display_prepare_read(display) != 0) {
          return true;
        }
        bool prepared = true;
        auto cancel_read = util::fail_guard([&]() {
          if (prepared) {
            wl_display_cancel_read(display);
          }
        });
        for (;;) {
          if (!error.empty()) {
            return false;
          }
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
          if (remaining <= 0) {
            return false;
          }
          const int flushed = wl_display_flush(display);
          if (flushed < 0 && errno != EAGAIN) {
            throw std::runtime_error("Wayland flush failed/disconnected");
          }
          pollfd fd {wl_display_get_fd(display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
          const int result = poll(&fd, 1, int(std::min<int64_t>(remaining, 1000)));
          if (result < 0 && errno == EINTR) {
            continue;
          }
          if (result < 0 || fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw std::runtime_error("Wayland connection lost during capture");
          }
          if (!result) {
            return false;
          }
          if (fd.revents & POLLIN) {
            prepared = false;
            if (wl_display_read_events(display) < 0 || wl_display_dispatch_pending(display) < 0) {
              throw std::runtime_error("Wayland read/dispatch failed during capture");
            }
            return true;
          }
        }
      }

      void request(uint64_t generation) override {
        if (!error.empty()) {
          throw std::runtime_error(error);
        }
        if (pending) {
          throw std::runtime_error("Overlapping screencopy requests");
        }
        pending = std::make_unique<request_t>(request_t {this, generation});
        pending->frame = zwlr_screencopy_manager_v1_capture_output(manager, 1, selected_proxy);
        if (!pending->frame || zwlr_screencopy_frame_v1_add_listener(pending->frame, &frame_listener, pending.get())) {
          throw std::runtime_error("Cannot request screencopy with cursor inclusion");
        }
      }

      std::shared_ptr<destination_t> allocate(uint32_t format, uint32_t width, uint32_t height) override {
        std::vector<uint64_t> modifiers;
        for (const auto &f : formats) {
          if (f.format == format && std::find(modifiers.begin(), modifiers.end(), f.modifier) == modifiers.end()) {
            modifiers.push_back(f.modifier);
          }
        }
        const gbm_api_t api {gbm_bo_create_with_modifiers2, gbm_bo_destroy, gbm_bo_get_width, gbm_bo_get_height, gbm_bo_get_format, gbm_bo_get_modifier, gbm_bo_get_plane_count, gbm_bo_get_fd_for_plane, gbm_bo_get_stride_for_plane, gbm_bo_get_offset};
        return allocate_destination(device->gbm, device, api, modifiers, format, width, height, validate);
      }

      void create_buffer(uint64_t generation, const destination_t &destination) override {
        if (!pending || generation != pending->generation || buffer) {
          throw std::runtime_error("Invalid DMA-BUF params lifetime");
        }
        pending->params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        if (!pending->params || zwp_linux_buffer_params_v1_add_listener(pending->params, &params_listener, pending.get())) {
          throw std::runtime_error("Cannot create asynchronous DMA-BUF params");
        }
        const auto &l = destination.layout;
        for (size_t plane = 0; plane < l.fds.size(); ++plane) {
          if (l.fds[plane] >= 0) {
            zwp_linux_buffer_params_v1_add(pending->params, l.fds[plane], plane, l.offsets[plane], l.pitches[plane], uint32_t(l.modifier >> 32), uint32_t(l.modifier));
          }
        }
        zwp_linux_buffer_params_v1_create(pending->params, l.width, l.height, l.fourcc, 0);
      }

      void copy(uint64_t generation) override {
        if (!pending || generation != pending->generation || !buffer) {
          throw std::runtime_error("Copy without a settled DMA-BUF destination");
        }
        zwlr_screencopy_frame_v1_copy(pending->frame, buffer);
      }

      void cancel() noexcept override {
        if (pending) {
          if (pending->params) {
            zwp_linux_buffer_params_v1_destroy(pending->params);
          }
          if (pending->frame) {
            zwlr_screencopy_frame_v1_destroy(pending->frame);
          }
          pending.reset();  // libwayland discards queued events for destroyed proxies.
        }
      }

      void disconnect() noexcept override {
        cancel();
        capture = nullptr;
        if (buffer) {
          wl_buffer_destroy(buffer);
          buffer = nullptr;
        }
        if (feedback) {
          zwp_linux_dmabuf_feedback_v1_destroy(feedback);
          feedback = nullptr;
        }
        for (auto &o : outputs) {
          if (o->xdg) {
            zxdg_output_v1_destroy(o->xdg);
          }
          if (o->proxy) {
            wl_output_release(o->proxy);
          }
        }
        outputs.clear();
        if (xdg_manager) {
          zxdg_output_manager_v1_destroy(xdg_manager);
          xdg_manager = nullptr;
        }
        if (dmabuf) {
          zwp_linux_dmabuf_v1_destroy(dmabuf);
          dmabuf = nullptr;
        }
        if (manager) {
          zwlr_screencopy_manager_v1_destroy(manager);
          manager = nullptr;
        }
        if (registry) {
          wl_registry_destroy(registry);
          registry = nullptr;
        }
        if (display) {
          wl_display_disconnect(display);
          display = nullptr;
        }
      }

      capture_t *capture = nullptr;
      output_t selected;
      platf::touch_port_t viewport {};
      std::pair<int, int> desktop;
      platf::kms_diagnostic_info_t identity;

    private:
      validator_t validate;
      wl_display *display = nullptr;
      wl_registry *registry = nullptr;
      zwlr_screencopy_manager_v1 *manager = nullptr;
      zwp_linux_dmabuf_v1 *dmabuf = nullptr;
      zxdg_output_manager_v1 *xdg_manager = nullptr;
      zwp_linux_dmabuf_feedback_v1 *feedback = nullptr;
      wl_buffer *buffer = nullptr;
      wl_output *selected_proxy = nullptr;
      uint32_t manager_id = 0, dmabuf_id = 0, xdg_id = 0, sync_id = 0;
      std::vector<std::unique_ptr<output_proxy_t>> outputs;
      std::unique_ptr<request_t> pending;
      std::shared_ptr<device_t> device;
      std::vector<format_t> table, formats, tranche_formats;
      dev_t main_device = 0, tranche_device = 0;
      bool feedback_done = false, frozen = false, tranche_started = false, tranche_flags_seen = false;
      std::string error;

      static void output_changed(output_proxy_t *o) {
        if (o->owner->frozen) {
          o->owner->fail("Wayland output topology changed; reconnect required for input mapping");
        }
      }

      static const wl_registry_listener registry_listener;
      static const wl_output_listener output_listener;
      static const zxdg_output_v1_listener xdg_listener;
      static const zwp_linux_dmabuf_feedback_v1_listener feedback_listener;
      static const zwlr_screencopy_frame_v1_listener frame_listener;
      static const zwp_linux_buffer_params_v1_listener params_listener;
    };

    const wl_registry_listener adapter_t::registry_listener {
      [](void *data, wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          if (!strcmp(interface, "zwlr_screencopy_manager_v1") && version >= 3 && !a->manager) {
            a->manager = static_cast<zwlr_screencopy_manager_v1 *>(wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, 3));
            a->manager_id = id;
          } else if (!strcmp(interface, "zwp_linux_dmabuf_v1") && version >= 4 && !a->dmabuf) {
            a->dmabuf = static_cast<zwp_linux_dmabuf_v1 *>(wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 4));
            a->dmabuf_id = id;
          } else if (!strcmp(interface, "zxdg_output_manager_v1") && version >= 3 && !a->xdg_manager) {
            a->xdg_manager = static_cast<zxdg_output_manager_v1 *>(wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, 3));
            a->xdg_id = id;
          } else if (!strcmp(interface, "wp_linux_drm_syncobj_manager_v1") && version >= 1) {
            a->sync_id = id;  // Capability advertisement, not a screencopy surface/fence protocol.
          } else if (!strcmp(interface, "wl_output") && version >= 3) {
            if (a->frozen) {
              a->fail("Wayland output added; reconnect required for desktop input mapping");
              return;
            }
            auto o = std::make_unique<output_proxy_t>();
            o->owner = a;
            o->metadata.id = id;
            o->proxy = static_cast<wl_output *>(wl_registry_bind(registry, id, &wl_output_interface, std::min(version, 4u)));
            if (!o->proxy || wl_output_add_listener(o->proxy, &output_listener, o.get())) {
              if (o->proxy) {
                wl_output_release(o->proxy);
              }
              throw std::runtime_error("Cannot bind output listener");
            }
            if (a->outputs.size() >= 256) {
              wl_output_release(o->proxy);
              throw std::runtime_error("Too many Wayland outputs");
            }
            a->outputs.push_back(std::move(o));
          }
        });
      },
      [](void *data, wl_registry *, uint32_t id) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          for (auto &o : a->outputs) {
            if (o->metadata.id == id) {
              o->metadata.removed = true;
              if (a->frozen) {
                a->fail("Wayland output removed; reconnect required");
              }
            }
          }
          if (id == a->manager_id || id == a->dmabuf_id || id == a->xdg_id || id == a->sync_id) {
            a->fail("Required compositor capture/sync protocol removed");
          }
        });
      }
    };
    const wl_output_listener adapter_t::output_listener {
      [](void *data, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t transform) {
        auto *o = static_cast<output_proxy_t *>(data);
        output_changed(o);
        o->metadata.transform = transform;
      },
      [](void *data, wl_output *, uint32_t flags, int32_t width, int32_t height, int32_t) {
        auto *o = static_cast<output_proxy_t *>(data);
        if (flags & WL_OUTPUT_MODE_CURRENT) {
          output_changed(o);
          o->metadata.native_width = width;
          o->metadata.native_height = height;
        }
      },
      [](void *data, wl_output *) {
        static_cast<output_proxy_t *>(data)->metadata.complete = true;
      },
      [](void *data, wl_output *, int32_t) {
        output_changed(static_cast<output_proxy_t *>(data));
      },
      [](void *data, wl_output *, const char *name) {
        auto *o = static_cast<output_proxy_t *>(data);
        o->owner->callback([&]() {
          if (!o->metadata.name.empty() && o->metadata.name != name) {
            throw std::runtime_error("Conflicting output connector names");
          }
          o->metadata.name = name;
        });
      },
      [](void *, wl_output *, const char *) {
      }
    };
    const zxdg_output_v1_listener adapter_t::xdg_listener {
      [](void *data, zxdg_output_v1 *, int32_t x, int32_t y) {
        auto *o = static_cast<output_proxy_t *>(data);
        output_changed(o);
        o->metadata.x = x;
        o->metadata.y = y;
      },
      [](void *data, zxdg_output_v1 *, int32_t width, int32_t height) {
        auto *o = static_cast<output_proxy_t *>(data);
        output_changed(o);
        o->metadata.width = width;
        o->metadata.height = height;
      },
      [](void *, zxdg_output_v1 *) {
      },  // v3 batches through wl_output.done.
      [](void *data, zxdg_output_v1 *, const char *name) {
        auto *o = static_cast<output_proxy_t *>(data);
        o->owner->callback([&]() {
          if (!o->metadata.name.empty() && o->metadata.name != name) {
            throw std::runtime_error("Conflicting output connector names");
          }
          o->metadata.name = name;
        });
      },
      [](void *, zxdg_output_v1 *, const char *) {
      }
    };
    const zwp_linux_dmabuf_feedback_v1_listener adapter_t::feedback_listener {
      [](void *data, zwp_linux_dmabuf_feedback_v1 *) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          if (a->feedback_done || !a->main_device || a->table.empty() || a->formats.empty() || a->tranche_started) {
            throw std::runtime_error("Incomplete/changed DMA-BUF device/format feedback; reconnect required");
          }
          a->feedback_done = true;
        });
      },
      [](void *data, zwp_linux_dmabuf_feedback_v1 *, int32_t fd, uint32_t size) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          auto close_fd = util::fail_guard([&]() {
            close(fd);
          });
          struct stat info {};
          if (a->feedback_done || !a->table.empty() || !size || size % sizeof(format_t) || size > 65536 * sizeof(format_t) || fstat(fd, &info) || info.st_size < size) {
            throw std::runtime_error("Malformed/changed DMA-BUF format table");
          }
          void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
          if (map == MAP_FAILED) {
            throw std::runtime_error("Cannot map DMA-BUF format table");
          }
          auto unmap = util::fail_guard([&]() {
            munmap(map, size);
          });
          a->table.resize(size / sizeof(format_t));
          memcpy(a->table.data(), map, size);
        });
      },
      [](void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *device) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          if (a->feedback_done || a->main_device || device->size != sizeof(dev_t)) {
            throw std::runtime_error("Malformed/changed DMA-BUF main device");
          }
          memcpy(&a->main_device, device->data, sizeof(dev_t));
        });
      },
      [](void *data, zwp_linux_dmabuf_feedback_v1 *) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          if (!a->tranche_started || !a->tranche_flags_seen || a->tranche_formats.empty()) {
            throw std::runtime_error("Malformed DMA-BUF tranche");
          }
          if (render_node(a->main_device) == render_node(a->tranche_device)) {
            if (a->formats.size() + a->tranche_formats.size() > 65536) {
              throw std::runtime_error("Too many DMA-BUF feedback formats");
            }
            a->formats.insert(a->formats.end(), a->tranche_formats.begin(), a->tranche_formats.end());
          }
          a->tranche_started = a->tranche_flags_seen = false;
          a->tranche_formats.clear();
        });
      },
      [](void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *device) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          if (a->feedback_done || a->tranche_started || device->size != sizeof(dev_t)) {
            throw std::runtime_error("Malformed DMA-BUF tranche device");
          }
          memcpy(&a->tranche_device, device->data, sizeof(dev_t));
          a->tranche_started = true;
        });
      },
      [](void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *indices) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          if (!a->tranche_started || indices->size % sizeof(uint16_t) || indices->size > 65536 * sizeof(uint16_t)) {
            throw std::runtime_error("Malformed DMA-BUF tranche format indices");
          }
          for (size_t i = 0; i < indices->size; i += sizeof(uint16_t)) {
            uint16_t index;
            memcpy(&index, static_cast<char *>(indices->data) + i, sizeof(index));
            if (index >= a->table.size() || a->tranche_formats.size() >= 65536) {
              throw std::runtime_error("DMA-BUF feedback index/count out of bounds");
            }
            a->tranche_formats.push_back(a->table[index]);
          }
        });
      },
      [](void *data, zwp_linux_dmabuf_feedback_v1 *, uint32_t flags) {
        auto *a = static_cast<adapter_t *>(data);
        a->callback([&]() {
          if (!a->tranche_started || a->tranche_flags_seen || (flags & ~1u)) {
            throw std::runtime_error("Malformed DMA-BUF tranche flags");
          }
          a->tranche_flags_seen = true;
        });
      }
    };
    const zwlr_screencopy_frame_v1_listener adapter_t::frame_listener {
      [](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t, uint32_t) {
      },  // SHM is deferred.
      [](void *data, zwlr_screencopy_frame_v1 *, uint32_t flags) {
        auto *r = static_cast<request_t *>(data);
        r->owner->callback([&]() {
          r->owner->capture->flags(r->generation, flags);
        });
      },
      [](void *data, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t ns) {
        auto *r = static_cast<request_t *>(data);
        r->owner->callback([&]() {
          r->owner->capture->ready(r->generation, ns);
        });
      },
      [](void *data, zwlr_screencopy_frame_v1 *) {
        auto *r = static_cast<request_t *>(data);
        r->owner->callback([&]() {
          r->owner->capture->failed(r->generation);
        });
      },
      [](void *data, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t, uint32_t) {
        auto *r = static_cast<request_t *>(data);
        r->owner->fail("Unexpected damage event for full screencopy request");
      },
      [](void *data, zwlr_screencopy_frame_v1 *, uint32_t format, uint32_t width, uint32_t height) {
        auto *r = static_cast<request_t *>(data);
        r->owner->callback([&]() {
          r->owner->capture->dmabuf(r->generation, format, width, height);
        });
      },
      [](void *data, zwlr_screencopy_frame_v1 *) {
        auto *r = static_cast<request_t *>(data);
        r->owner->callback([&]() {
          r->owner->capture->buffer_done(r->generation);
        });
      }
    };
    const zwp_linux_buffer_params_v1_listener adapter_t::params_listener {
      [](void *data, zwp_linux_buffer_params_v1 *params, wl_buffer *buffer) {
        auto *r = static_cast<request_t *>(data);
        auto *a = r->owner;
        a->callback([&]() {
          if (a->buffer || !a->capture || a->capture->state() != capture_t::phase_t::params || a->capture->token() != r->generation) {
            wl_buffer_destroy(buffer);
            a->fail("Late/duplicate DMA-BUF buffer creation");
            return;
          }
          a->buffer = buffer;
          static const wl_buffer_listener listener {[](void *, wl_buffer *) {
          }};
          if (wl_buffer_add_listener(buffer, &listener, nullptr)) {
            throw std::runtime_error("Cannot attach DMA-BUF release listener");
          }
          zwp_linux_buffer_params_v1_destroy(params);
          r->params = nullptr;
          a->capture->created(r->generation);
        });
      },
      [](void *data, zwp_linux_buffer_params_v1 *) {
        auto *r = static_cast<request_t *>(data);
        r->owner->callback([&]() {
          r->owner->capture->failed(r->generation);
        });
      }
    };

    class source_t final: public platf::kms_diagnostic_source_t {
    public:
      source_t(const std::string &name, const std::string &app, validator_t validate):
          adapter(std::move(validate)) {
        adapter.initialize(name, app);
        capture = std::make_unique<capture_t>(adapter, adapter.selected);
        adapter.capture = capture.get();
      }

      std::shared_ptr<egl::img_descriptor_t> next() override {
        auto destination = capture->next();

        struct image_t final: egl::img_descriptor_t {
          std::shared_ptr<destination_t> destination;
        };

        auto image = std::make_shared<image_t>();
        image->destination = destination;
        image->sd = {};
        std::fill_n(image->sd.fds, 4, -1);
        const auto &l = destination->layout;
        image->sd.width = l.width;
        image->sd.height = l.height;
        image->sd.fourcc = l.fourcc;
        image->sd.modifier = l.modifier;
        for (size_t plane = 0; plane < l.fds.size(); ++plane) {
          if (l.fds[plane] >= 0) {
            image->sd.fds[plane] = fcntl(l.fds[plane], F_DUPFD_CLOEXEC, 0);
            if (image->sd.fds[plane] < 0) {
              throw std::runtime_error("Cannot retain capture plane FD");
            }
          }
          image->sd.pitches[plane] = l.pitches[plane];
          image->sd.offsets[plane] = l.offsets[plane];
        }
        // Protocol timestamp has arbitrary clock offset. Use local handoff time
        // for Apollo's transport pacing diagnostics, without claiming latency.
        image->frame_timestamp = std::chrono::steady_clock::now();
        return image;
      }

      platf::kms_diagnostic_info_t info() const override {
        return adapter.identity;
      }

      platf::touch_port_t viewport() const override {
        return adapter.viewport;
      }

      std::pair<int, int> desktop_size() const override {
        return adapter.desktop;
      }

      bool compositor_owned() const override {
        return true;
      }

      void snapshot_complete() override {
        capture->snapshot_complete();
      }

      void encode_complete() override {
        capture->encode_complete();
      }

      std::string connector() const override {
        return adapter.selected.name;
      }

    private:
      adapter_t adapter;
      std::unique_ptr<capture_t> capture;  // Disconnect/cancel while listener data and GBM are still alive.
    };
  }  // namespace
}  // namespace pyrowave_wl

namespace platf {
  std::unique_ptr<kms_diagnostic_source_t> make_pyrowave_wayland_source(const std::string &name, const std::string &app, pyrowave_wl::validator_t validate) {
    return std::make_unique<pyrowave_wl::source_t>(name, app, std::move(validate));
  }
}  // namespace platf
