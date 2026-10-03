/** @brief CPU-only fake compositor exercises the actual private Wayland adapter/listeners/GBM export/import. */
#include "src/platform/linux/pyrowave_wayland.h"

#include "linux-dmabuf-unstable-v1-server.h"
#include "src/platform/linux/pyrowave_diagnostic_import.h"
#include "wlr-screencopy-unstable-v1-server.h"
#include "xdg-output-unstable-v1-server.h"

#include <atomic>
#include <fcntl.h>
#include <iostream>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <wayland-server.h>
#include <xf86drm.h>

using namespace pyrowave_wl;

namespace {
  void require(bool ok, const char *message) {
    if (!ok) {
      throw std::runtime_error(message);
    }
  }

  template<class F>
  void rejects(F fn, std::string_view reason) {
    try {
      fn();
    } catch (const std::exception &e) {
      require(std::string_view(e.what()).find(reason) != std::string_view::npos, e.what());
      return;
    }
    throw std::runtime_error("Expected failure was accepted");
  }

  enum class fault_t {
    none,
    missing_sync,
    old_dmabuf,
    old_screencopy,
    table_size,
    index,
    other_device,
    params_timeout,
    params_failed,
    ready_timeout,
    failed,
    invert,
    no_flags,
    bad_time,
    bad_shape,
    bad_format,
    no_dmabuf,
    disconnect,
    output_removed
  };

  struct stats_t {
    std::atomic<int> allocations {0}, bo_destroyed {0}, plane_exports {0}, imports {0}, copies {0}, cursors {0}, requests {0}, frame_destroyed {0}, params_destroyed {0}, buffer_destroyed {0};
    int planes = 1;
    bool reject_memory = false, gbm_fail = false;
    std::vector<int> fds;
  };

  stats_t *stats = nullptr;

  struct fake_bo_t {
    uint32_t width, height, format;
    uint64_t modifier;
  };

  // Real importer, fake Vulkan entry points: no Vulkan initialization or GPU calls.
  pyrowave_diag::import_api_t import_api() {
    pyrowave_diag::import_api_t api {};
    api.format_properties = [](VkPhysicalDevice, VkFormat, VkFormatProperties2 *p) {
      auto *list = static_cast<VkDrmFormatModifierPropertiesListEXT *>(p->pNext);
      if (list->pDrmFormatModifierProperties) {
        list->pDrmFormatModifierProperties[0] = {DRM_FORMAT_MOD_LINEAR, 1, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT};
      }
      list->drmFormatModifierCount = 1;
    };
    api.image_format_properties = [](VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *p) {
      p->imageFormatProperties = {{8192, 8192, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, 1ULL << 32};
      auto *external = static_cast<VkExternalImageFormatProperties *>(p->pNext);
      external->externalMemoryProperties = {VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
      return VK_SUCCESS;
    };
    api.memory_properties = [](VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *p) {
      p->memoryTypeCount = 1;
      p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    };
    api.fd_properties = [](VkDevice, VkExternalMemoryHandleTypeFlagBits, int fd, VkMemoryFdPropertiesKHR *p) {
      require(fcntl(fd, F_GETFD) >= 0, "Importer did not query an actual exported FD");
      p->memoryTypeBits = stats->reject_memory ? 2 : 1;
      return VK_SUCCESS;
    };
    api.create_image = [](VkDevice, const VkImageCreateInfo *info, const VkAllocationCallbacks *, VkImage *image) {
      auto *external = static_cast<const VkExternalMemoryImageCreateInfo *>(info->pNext);
      auto *modifier = static_cast<const VkImageDrmFormatModifierExplicitCreateInfoEXT *>(external->pNext);
      require(modifier->drmFormatModifierPlaneCount == 1 && modifier->pPlaneLayouts[0].offset == 128 && modifier->pPlaneLayouts[0].rowPitch == 10240, "Importer lost actual GBM pitch/offset");
      *image = reinterpret_cast<VkImage>(uintptr_t(1));
      return VK_SUCCESS;
    };
    api.image_requirements = [](VkDevice, const VkImageMemoryRequirementsInfo2 *, VkMemoryRequirements2 *p) {
      p->memoryRequirements = {4096, 4096, 1};
    };
    api.allocate_memory = [](VkDevice, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *, VkDeviceMemory *memory) {
      auto *import = static_cast<const VkImportMemoryFdInfoKHR *>(info->pNext);
      auto *dedicated = static_cast<const VkMemoryDedicatedAllocateInfo *>(import->pNext);
      require(dedicated->image && info->memoryTypeIndex == 0, "Import lost dedicated image/memory intersection");
      close(import->fd);
      *memory = reinterpret_cast<VkDeviceMemory>(uintptr_t(2));
      ++stats->imports;
      return VK_SUCCESS;
    };
    api.bind_image_memory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
      return VK_SUCCESS;
    };
    api.destroy_image = [](VkDevice, VkImage, const VkAllocationCallbacks *) {
    };
    api.free_memory = [](VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {
    };
    return api;
  }

  class compositor_t {
  public:
    compositor_t(fault_t fault, bool two_outputs = false):
        fault(fault) {
      char pattern[] = "/tmp/apollo-wl-cpu-XXXXXX";
      auto *dir = mkdtemp(pattern);
      require(dir, "Cannot create isolated fake-compositor directory");
      path = dir;
      setenv("XDG_RUNTIME_DIR", path.c_str(), 1);
      display = wl_display_create();
      require(display, "Cannot create fake Wayland display");
      const char *socket = wl_display_add_socket_auto(display);
      require(socket, "Cannot create isolated CPU protocol socket");
      setenv("WAYLAND_DISPLAY", socket, 1);
      first_output_global = wl_global_create(display, &wl_output_interface, 4, this, output_bind);
      if (two_outputs) {
        wl_global_create(display, &wl_output_interface, 4, this, second_output_bind);
      }
      wl_global_create(display, &zxdg_output_manager_v1_interface, 3, this, xdg_bind);
      wl_global_create(display, &zwlr_screencopy_manager_v1_interface, fault == fault_t::old_screencopy ? 2 : 3, this, capture_bind);
      wl_global_create(display, &zwp_linux_dmabuf_v1_interface, fault == fault_t::old_dmabuf ? 3 : 4, this, dmabuf_bind);
      static const wl_interface sync_interface {"wp_linux_drm_syncobj_manager_v1", 1, 0, nullptr, 0, nullptr};
      if (fault != fault_t::missing_sync) {
        wl_global_create(display, &sync_interface, 1, this, [](wl_client *, void *, uint32_t, uint32_t) {
        });
      }
      wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      require(wake >= 0, "Cannot create fake compositor shutdown FD");
      event = wl_event_loop_add_fd(wl_display_get_event_loop(display), wake, WL_EVENT_READABLE, [](int fd, uint32_t, void *data) {
        uint64_t value;
        (void) read(fd, &value, sizeof(value));
        wl_display_terminate(static_cast<compositor_t *>(data)->display);
        return 0;
      },
                                   this);
      thread = std::thread([&]() {
        wl_display_run(display);
      });
    }

    ~compositor_t() {
      uint64_t value = 1;
      (void) write(wake, &value, sizeof(value));
      thread.join();
      wl_event_source_remove(event);
      close(wake);
      wl_display_destroy_clients(display);
      wl_display_destroy(display);
      rmdir(path.c_str());
    }

    fault_t fault;

  private:
    wl_display *display;
    wl_global *first_output_global;
    wl_event_source *event;
    int wake;
    std::thread thread;
    std::string path;

    static void destroy(wl_client *, wl_resource *r) {
      wl_resource_destroy(r);
    }

    static void output_bind_common(wl_client *client, void *data, uint32_t version, uint32_t id, bool second) {
      auto *r = wl_resource_create(client, &wl_output_interface, version, id);
      static const struct wl_output_interface implementation {destroy};
      // Low bit marks output 2; compositor objects are aligned.
      wl_resource_set_implementation(r, &implementation, reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(data) | second), nullptr);
      wl_output_send_geometry(r, second ? 2048 : 0, 0, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN, "CPU", "Protocol test", WL_OUTPUT_TRANSFORM_NORMAL);
      wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT, 2560, 1440, 60000);
      wl_output_send_scale(r, 2);
      if (version >= 4) {
        wl_output_send_name(r, second ? "TEST-2" : "TEST-1");
        wl_output_send_description(r, "CPU fake output");
      }
      wl_output_send_done(r);
    }

    static void output_bind(wl_client *c, void *d, uint32_t v, uint32_t id) {
      output_bind_common(c, d, v, id, false);
    }

    static void second_output_bind(wl_client *c, void *d, uint32_t v, uint32_t id) {
      output_bind_common(c, d, v, id, true);
    }

    static void xdg_bind(wl_client *client, void *data, uint32_t version, uint32_t id) {
      auto *r = wl_resource_create(client, &zxdg_output_manager_v1_interface, version, id);
      static const struct zxdg_output_manager_v1_interface implementation {destroy, [](wl_client *client, wl_resource *manager, uint32_t id, wl_resource *output) {
                                                                             auto *r = wl_resource_create(client, &zxdg_output_v1_interface, 3, id);
                                                                             static const struct zxdg_output_v1_interface implementation {destroy};
                                                                             wl_resource_set_implementation(r, &implementation, wl_resource_get_user_data(manager), nullptr);
                                                                             bool second = reinterpret_cast<uintptr_t>(wl_resource_get_user_data(output)) & 1;
                                                                             zxdg_output_v1_send_logical_position(r, second ? 2048 : 0, 0);
                                                                             zxdg_output_v1_send_logical_size(r, 2048, 1152);  // fractional scale 1.25, independent of integer wl_output.scale.
                                                                             zxdg_output_v1_send_name(r, second ? "TEST-2" : "TEST-1");
                                                                             wl_output_send_done(output);
                                                                           }};
      wl_resource_set_implementation(r, &implementation, data, nullptr);
    }

    static void capture_bind(wl_client *client, void *data, uint32_t version, uint32_t id) {
      auto *r = wl_resource_create(client, &zwlr_screencopy_manager_v1_interface, version, id);
      static const struct zwlr_screencopy_manager_v1_interface implementation {
        [](wl_client *client, wl_resource *manager, uint32_t id, int32_t cursor, wl_resource *) {
          auto *a = static_cast<compositor_t *>(wl_resource_get_user_data(manager));
          ++stats->requests;
          if (cursor == 1) {
            ++stats->cursors;
          }
          auto *r = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface, 3, id);
          static const struct zwlr_screencopy_frame_v1_interface implementation {frame_copy, destroy, [](wl_client *, wl_resource *, wl_resource *) {
                                                                                   std::terminate();
                                                                                 }};
          wl_resource_set_implementation(r, &implementation, a, [](wl_resource *) {
            ++stats->frame_destroyed;
          });
          if (a->fault == fault_t::disconnect) {
            // End the transport, letting the event loop destroy the client
            // after this active request callback has returned.
            shutdown(wl_client_get_fd(client), SHUT_RDWR);
            return;
          }
          if (a->fault != fault_t::no_dmabuf) {
            zwlr_screencopy_frame_v1_send_linux_dmabuf(r, a->fault == fault_t::bad_format ? DRM_FORMAT_NV12 : DRM_FORMAT_ARGB8888, a->fault == fault_t::bad_shape ? 2048 : 2560, a->fault == fault_t::bad_shape ? 1152 : 1440);
          }
          zwlr_screencopy_frame_v1_send_buffer(r, WL_SHM_FORMAT_ARGB8888, 2560, 1440, 10240);
          zwlr_screencopy_frame_v1_send_buffer_done(r);
        },
        [](wl_client *, wl_resource *, uint32_t, int32_t, wl_resource *, int32_t, int32_t, int32_t, int32_t) {
          std::terminate();
        },
        destroy
      };
      wl_resource_set_implementation(r, &implementation, data, nullptr);
    }

    static void frame_copy(wl_client *client, wl_resource *frame, wl_resource *buffer) {
      ++stats->copies;
      auto *a = static_cast<compositor_t *>(wl_resource_get_user_data(frame));
      if (a->fault == fault_t::ready_timeout) {
        return;
      }
      if (a->fault == fault_t::output_removed) {
        wl_global_remove(a->first_output_global);
        return;
      }
      if (a->fault == fault_t::failed) {
        zwlr_screencopy_frame_v1_send_failed(frame);
        return;
      }
      if (a->fault != fault_t::no_flags) {
        zwlr_screencopy_frame_v1_send_flags(frame, a->fault == fault_t::invert ? 1 : 0);
      }
      zwlr_screencopy_frame_v1_send_ready(frame, 0, 1, a->fault == fault_t::bad_time ? 1000000000 : 0);
      wl_buffer_send_release(buffer);
      (void) client;
    }

    static void dmabuf_bind(wl_client *client, void *data, uint32_t version, uint32_t id) {
      auto *r = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
      static const struct zwp_linux_dmabuf_v1_interface implementation {destroy, params_create, feedback_create, [](wl_client *, wl_resource *, uint32_t, wl_resource *) {
                                                                          std::terminate();
                                                                        }};
      wl_resource_set_implementation(r, &implementation, data, nullptr);
    }

    static void params_create(wl_client *client, wl_resource *dmabuf, uint32_t id) {
      auto *r = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, 4, id);
      static const struct zwp_linux_buffer_params_v1_interface implementation {
        destroy,
        [](wl_client *, wl_resource *, int32_t fd, uint32_t plane, uint32_t offset, uint32_t stride, uint32_t hi, uint32_t lo) {
          require(plane == 0 && offset == 128 && stride == 10240 && hi == 0 && lo == 0, "Wire DMA-BUF params lost the exported plane layout");
          close(fd);
        },
        [](wl_client *client, wl_resource *params, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
          require(width == 2560 && height == 1440 && format == DRM_FORMAT_ARGB8888 && !flags, "Native GPU buffer shape/format/flags lost");
          auto *a = static_cast<compositor_t *>(wl_resource_get_user_data(params));
          if (a->fault == fault_t::params_timeout) {
            return;
          }
          if (a->fault == fault_t::params_failed) {
            zwp_linux_buffer_params_v1_send_failed(params);
            return;
          }
          auto *r = wl_resource_create(client, &wl_buffer_interface, 1, 0);
          static const struct wl_buffer_interface implementation {destroy};
          wl_resource_set_implementation(r, &implementation, a, [](wl_resource *) {
            ++stats->buffer_destroyed;
          });
          zwp_linux_buffer_params_v1_send_created(params, r);
        },
        [](wl_client *, wl_resource *, uint32_t, int32_t, int32_t, uint32_t, uint32_t) {
          std::terminate();
        }
      };
      wl_resource_set_implementation(r, &implementation, wl_resource_get_user_data(dmabuf), [](wl_resource *) {
        ++stats->params_destroyed;
      });
    }

    static void feedback_create(wl_client *client, wl_resource *dmabuf, uint32_t id) {
      auto *a = static_cast<compositor_t *>(wl_resource_get_user_data(dmabuf));
      auto *r = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, 4, id);
      static const struct zwp_linux_dmabuf_feedback_v1_interface implementation {destroy};
      wl_resource_set_implementation(r, &implementation, a, nullptr);

      struct entry_t {
        uint32_t format, padding;
        uint64_t modifier;
      };

      entry_t table {DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_LINEAR};
      const int fd = memfd_create("apollo-wl-cpu-table", MFD_CLOEXEC);
      require(fd >= 0 && write(fd, &table, sizeof(table)) == sizeof(table), "Cannot create CPU format table");
      zwp_linux_dmabuf_feedback_v1_send_format_table(r, fd, a->fault == fault_t::table_size ? 15 : sizeof(table));
      close(fd);
      dev_t device = makedev(1, 3);
      wl_array array {sizeof(device), sizeof(device), &device};
      zwp_linux_dmabuf_feedback_v1_send_main_device(r, &array);
      if (a->fault == fault_t::other_device) {
        device = makedev(1, 5);
      }
      zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(r, &array);
      zwp_linux_dmabuf_feedback_v1_send_tranche_flags(r, 0);
      uint16_t index = a->fault == fault_t::index ? 5 : 0;
      array = {sizeof(index), sizeof(index), &index};
      zwp_linux_dmabuf_feedback_v1_send_tranche_formats(r, &array);
      zwp_linux_dmabuf_feedback_v1_send_tranche_done(r);
      zwp_linux_dmabuf_feedback_v1_send_done(r);
    }
  };

  std::unique_ptr<platf::kms_diagnostic_source_t> source(const std::string &name = {}, const std::string &app = {}) {
    return platf::make_pyrowave_wayland_source(name, app, [](const pyrowave_diag::layout_t &layout) {
      auto image = pyrowave_diag::import_dma_buf(VK_NULL_HANDLE, VK_NULL_HANDLE, layout, import_api(), true);
    });
  }

  void check_fds(const stats_t &s) {
    for (int fd : s.fds) {
      require(fcntl(fd, F_GETFD) < 0, "Exported GBM FD leaked after settled capture");
    }
  }

  void healthy() {
    stats_t s;
    stats = &s;
    {
      compositor_t compositor(fault_t::none);
      {
        auto capture = source();
        require(capture->compositor_owned() && capture->connector() == "TEST-1", "Selected wrong backend/connector");
        require(capture->info().render_device == makedev(1, 3), "Lost compositor device identity");
        auto viewport = capture->viewport();
        require(viewport.width == 2048 && viewport.height == 1152 && capture->desktop_size() == std::pair<int, int> {2048, 1152}, "Logical input mapping used physical/integer scale geometry");
        auto image = capture->next();
        require(image->sd.width == 2560 && image->sd.height == 1440 && image->sd.offsets[0] == 128, "Capture used logical instead of native pixels");
        rejects([&]() {
          capture->next();
        },
                "before snapshot");
        rejects([&]() {
          capture->encode_complete();
        },
                "before snapshot");
        capture->snapshot_complete();
        rejects([&]() {
          capture->next();
        },
                "before snapshot");
        capture->encode_complete();
        image.reset();
        image = capture->next();
        capture->snapshot_complete();
        capture->encode_complete();
        image.reset();
        require(s.allocations == 1 && s.copies == 2 && s.cursors == 2 && s.imports == 1, "Destination recreated/rewritten or cursor was omitted");
      }
    }
    require(s.bo_destroyed == 1 && s.buffer_destroyed == 1 && s.frame_destroyed == 2 && s.params_destroyed == 1, "Actual proxy/GBM cleanup leaked");
    check_fds(s);
    std::cout << "PASS production adapter: cursor, native2560x1440/logical2048x1152, actual offset/FD/dedicated import, one destination, reuse ordering, cleanup\n";
  }

  void failure(fault_t fault, std::string_view reason, int planes = 1, bool reject_memory = false, bool gbm_fail = false) {
    std::cout << "CASE failure=" << int(fault) << " planes=" << planes << " memory=" << reject_memory << " gbm=" << gbm_fail << '\n';
    stats_t s;
    stats = &s;
    s.planes = planes;
    s.reject_memory = reject_memory;
    s.gbm_fail = gbm_fail;
    {
      compositor_t compositor(fault);
      rejects([&]() {
        auto capture = source();
        rejects([&]() {
          capture->next();
        },
                reason);
        rejects([&]() {
          capture->next();
        },
                "after capture failure");
        throw std::runtime_error(std::string(reason));
      },
              reason);
    }
    require(s.allocations == s.bo_destroyed, "Failed capture leaked GBM ownership");
    check_fds(s);
    if (planes == 2) {
      require(s.plane_exports == 2, "Did not enumerate auxiliary planes before rejection");
    }
    // A new independent source can reconnect after settlement.
    healthy();
  }

  void selection() {
    stats_t s;
    stats = &s;
    {
      compositor_t compositor(fault_t::none, true);
      rejects([&]() {
        source();
      },
              "ambiguous");
      rejects([&]() {
        source("0");
      },
              "connector name");
      rejects([&]() {
        source("TEST-2", "0");
      },
              "per-app");
      auto capture = source("TEST-2", "TEST-2");
      require(capture->connector() == "TEST-2" && capture->viewport().offset_x == 2048 && capture->viewport().width == 2048 && capture->desktop_size().first == 4096, "Explicit connector selection lost logical desktop/input offset");
    }
    output_t o {1, "TEST", 2560, 1440, 0, 0, 2048, 1152, 0, true, false};
    o.transform = 1;
    rejects([&]() {
      select_output({o}, {}, {});
    },
            "untransformed");
    o.transform = 0;
    o.native_width = 3840;
    o.native_height = 2160;
    rejects([&]() {
      select_output({o}, {}, {});
    },
            "native4K");
    std::cout << "PASS output selection: sole active/name/ambiguous/per-app refusal, fractional scale, desktop offsets and transform/native4K rejection\n";
  }

  // Adversarial events after cancellation are passed to the same production
  // callback state machine. A dead token can neither resurrect nor rewrite it.
  void late_callbacks() {
    struct fake_transport_t: transport_t {
      capture_t *capture = nullptr;
      bool cancelled = false, disconnected = false;

      void request(uint64_t) override {}

      std::shared_ptr<destination_t> allocate(uint32_t, uint32_t, uint32_t) override {
        throw std::runtime_error("unexpected");
      }

      void create_buffer(uint64_t, const destination_t &) override {}

      void copy(uint64_t) override {
        throw std::runtime_error("late callback requested copy");
      }

      void cancel() noexcept override {
        cancelled = true;
      }

      void disconnect() noexcept override {
        disconnected = true;
      }

      bool dispatch(std::chrono::steady_clock::time_point) override {
        return false;
      }
    } transport;

    {
      capture_t capture(transport, {1, "TEST", 2560, 1440, 0, 0, 2048, 1152, 0, true, false});
      rejects([&]() {
        capture.next();
      },
              "timeout");
      require(transport.cancelled, "Timeout did not cancel pending request");
      capture.dmabuf(capture.token(), DRM_FORMAT_ARGB8888, 2560, 1440);
      capture.buffer_done(capture.token());
      capture.created(capture.token());
      capture.flags(capture.token(), 0);
      capture.ready(capture.token(), 0);
      require(capture.state() == capture_t::phase_t::failed, "Late callback resurrected cancelled capture");
    }
    require(transport.disconnected, "Teardown did not disconnect listener ownership");
    std::cout << "PASS production callback state: timeout/cancel/late events remain terminal\n";
  }
}  // namespace

// Only device boundaries are mocked; no real GBM/DRM/GPU operation is invoked.
extern "C" int __wrap_drmGetDeviceFromDevId(dev_t device, uint32_t, drmDevicePtr *result) {
  auto *d = static_cast<drmDevicePtr>(calloc(1, sizeof(drmDevice)));
  d->nodes = static_cast<char **>(calloc(DRM_NODE_MAX, sizeof(char *)));
  d->nodes[DRM_NODE_RENDER] = strdup(device == makedev(1, 5) ? "/dev/zero" : "/dev/null");
  d->nodes[DRM_NODE_PRIMARY] = strdup(d->nodes[DRM_NODE_RENDER]);
  d->available_nodes = (1 << DRM_NODE_RENDER) | (1 << DRM_NODE_PRIMARY);
  *result = d;
  return 0;
}

extern "C" void __wrap_drmFreeDevice(drmDevicePtr *d) {
  for (int i = 0; i < DRM_NODE_MAX; ++i) {
    free((*d)->nodes[i]);
  }
  free((*d)->nodes);
  free(*d);
  *d = nullptr;
}

extern "C" gbm_device *__wrap_gbm_create_device(int fd) {
  require(fcntl(fd, F_GETFD) >= 0, "Invalid selected render FD");
  return reinterpret_cast<gbm_device *>(uintptr_t(1));
}

extern "C" void __wrap_gbm_device_destroy(gbm_device *) {}

extern "C" gbm_bo *__wrap_gbm_bo_create_with_modifiers2(gbm_device *, uint32_t w, uint32_t h, uint32_t f, const uint64_t *m, unsigned n, uint32_t flags) {
  require(n == 1 && flags == GBM_BO_USE_RENDERING, "Implicit modifier/unsupported allocation flags");
  if (stats->gbm_fail) {
    return nullptr;
  }
  ++stats->allocations;
  return reinterpret_cast<gbm_bo *>(new fake_bo_t {w, h, f, *m});
}

extern "C" void __wrap_gbm_bo_destroy(gbm_bo *bo) {
  ++stats->bo_destroyed;
  delete reinterpret_cast<fake_bo_t *>(bo);
}

extern "C" uint32_t __wrap_gbm_bo_get_width(gbm_bo *bo) {
  return reinterpret_cast<fake_bo_t *>(bo)->width;
}

extern "C" uint32_t __wrap_gbm_bo_get_height(gbm_bo *bo) {
  return reinterpret_cast<fake_bo_t *>(bo)->height;
}

extern "C" uint32_t __wrap_gbm_bo_get_format(gbm_bo *bo) {
  return reinterpret_cast<fake_bo_t *>(bo)->format;
}

extern "C" uint64_t __wrap_gbm_bo_get_modifier(gbm_bo *bo) {
  return reinterpret_cast<fake_bo_t *>(bo)->modifier;
}

extern "C" int __wrap_gbm_bo_get_plane_count(gbm_bo *) {
  return stats->planes;
}

extern "C" int __wrap_gbm_bo_get_fd_for_plane(gbm_bo *, int) {
  int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
  ++stats->plane_exports;
  stats->fds.push_back(fd);
  return fd;
}

extern "C" uint32_t __wrap_gbm_bo_get_stride_for_plane(gbm_bo *, int) {
  return 10240;
}

extern "C" uint32_t __wrap_gbm_bo_get_offset(gbm_bo *, int) {
  return 128;
}

int main() {
  std::cout.setf(std::ios::unitbuf);
  try {
    healthy();
    selection();
    late_callbacks();
    for (auto f : {fault_t::missing_sync, fault_t::old_dmabuf, fault_t::old_screencopy}) {
      failure(f, "requires wlr-screencopy");
    }
    failure(fault_t::table_size, "format table");
    failure(fault_t::index, "out of bounds");
    failure(fault_t::other_device, "Incomplete");
    failure(fault_t::params_timeout, "timeout");
    failure(fault_t::params_failed, "Compositor screencopy failed");
    failure(fault_t::ready_timeout, "timeout");
    failure(fault_t::output_removed, "output removed");
    failure(fault_t::failed, "Compositor screencopy failed");
    failure(fault_t::invert, "flags/y-invert");
    failure(fault_t::no_flags, "ready event");
    failure(fault_t::bad_time, "ready event");
    failure(fault_t::bad_shape, "physical extent");
    failure(fault_t::bad_format, "format/physical");
    failure(fault_t::no_dmabuf, "SHM fallback");
    failure(fault_t::disconnect, "Wayland");
    failure(fault_t::none, "Multiple framebuffer", 2);
    failure(fault_t::none, "memory type", 1, true);
    failure(fault_t::none, "GBM allocation", 1, false, true);
    std::cout << "PASS all CPU fake-compositor failures settle and reconnect; no hardware probes\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
