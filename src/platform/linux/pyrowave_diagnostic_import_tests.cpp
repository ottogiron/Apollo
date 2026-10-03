/** @brief CPU mocks exercise the actual live wait/import helpers; no real capture. */
#include "pyrowave_diagnostic_import.h"
#include "pyrowave_diagnostic_sync.h"
#include "pyrowave_live_layout.h"
#include "pyrowave_snapshot.h"

#include <functional>
#include <iostream>
#include <type_traits>

namespace {
  using namespace pyrowave_diag;

  void require(bool value, const char *message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }

  void expect_error(const std::function<void()> &run, const char *message) {
    try {
      run();
    } catch (const std::runtime_error &error) {
      require(std::string(error.what()).find(message) != std::string::npos, "Expected rejection at the selected failure stage");
      return;
    }
    throw std::runtime_error("Expected live import/wait rejection");
  }

  struct sync_mock_t {
    int source = -1, exported = -1, status = 1, export_result = 0, query_result = 0;
    int poll_result = 1, interruptions = 0, poll_calls = 0, query_calls = 0, close_calls = 0;
    short revents = POLLIN;
    bool export_fd = true, query_while_open = false;
  };

  sync_mock_t *sync_mock;

  int sync_ioctl(int fd, unsigned long request, void *arg) {
    auto &mock = *sync_mock;
    if (request == DMA_BUF_IOCTL_EXPORT_SYNC_FILE) {
      auto info = static_cast<dma_buf_export_sync_file *>(arg);
      require(fd == mock.source && info->flags == DMA_BUF_SYNC_READ && info->fd == -1, "Export current writer fences for READ");
      if (mock.export_fd) {
        info->fd = mock.exported = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        require(mock.exported >= 0, "Mock export FD creation");
      }
      return mock.export_result;
    }
    require(request == SYNC_IOC_FILE_INFO && fd == mock.exported, "Query the exported sync-file FD");
    auto info = static_cast<sync_file_info *>(arg);
    require(!info->num_fences && !info->flags && !info->pad && !info->sync_fence_info, "Query aggregate fence status with zeroed fields");
    ++mock.query_calls;
    mock.query_while_open = fcntl(fd, F_GETFD) >= 0 && !mock.close_calls;
    info->status = mock.status;
    return mock.query_result;
  }

  int sync_poll(pollfd *fds, nfds_t count, int timeout) {
    auto &mock = *sync_mock;
    require(count == 1 && fds->fd == mock.exported && fds->events == POLLIN && timeout >= 0 && timeout <= 2000, "Bounded poll of exported fence");
    ++mock.poll_calls;
    if (mock.interruptions-- > 0) {
      errno = EINTR;
      return -1;
    }
    fds->revents = mock.revents;
    return mock.poll_result;
  }

  int sync_close(int fd) {
    ++sync_mock->close_calls;
    return close(fd);
  }

  void test_producer_wait() {
    owned_fd_t source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    require(source.fd >= 0, "Create mock DMA-BUF source FD");
    const producer_sync_api_t api {sync_ioctl, sync_poll, sync_close};
    // POLLIN is deliberately identical in every case: it cannot distinguish a
    // successful fence from an errored Linux sync_file.
    for (int status : {1, -EIO, 0, 2}) {
      sync_mock_t mock;
      sync_mock = &mock;
      mock.source = source.fd;
      mock.status = status;
      if (status == 1) {
        mock.interruptions = 1;
        wait_producer(source.fd, api);
        require(mock.poll_calls == 2, "Retry interrupted poll before successful fence query");
      } else {
        expect_error([&] {
          wait_producer(source.fd, api);
        },
                     "did not complete successfully: status ");
      }
      require(mock.query_calls == 1 && mock.query_while_open, "Query status before closing on success/error/active/unknown");
      require(mock.close_calls == 1 && fcntl(mock.exported, F_GETFD) == -1, "Close fence exactly once after status outcome");
      require(fcntl(source.fd, F_GETFD) >= 0, "Producer wait must preserve source FD");
    }
    for (int failure = 0; failure < 4; ++failure) {
      sync_mock_t mock;
      sync_mock = &mock;
      mock.source = source.fd;
      if (failure == 0) {
        mock.query_result = -1;
      }
      if (failure == 1) {
        mock.poll_result = 0;
      }
      if (failure == 2) {
        mock.revents = POLLIN | POLLERR;
      }
      if (failure == 3) {
        mock.export_result = -1;  // Even a partially supplied FD unwinds.
      }
      expect_error([&] {
        wait_producer(source.fd, api);
      },
                   failure == 0 ? "status query failed" : "producer fence");
      require(mock.close_calls == 1 && fcntl(mock.exported, F_GETFD) == -1, "Close fence exactly once on ioctl/poll/export failure");
      require(mock.query_calls == (failure == 0 ? 1 : 0), "Do not query after unsuccessful poll/export");
      if (failure == 0) {
        require(mock.query_while_open, "Failed query still owns FD until cleanup");
      }
    }
    sync_mock_t mock;
    sync_mock = &mock;
    mock.source = source.fd;
    mock.export_fd = false;
    expect_error([&] {
      wait_producer(source.fd, api);
    },
                 "export unsupported/failed");
    require(!mock.close_calls && !mock.poll_calls && !mock.query_calls, "Reject missing exported FD without closing source or polling");
    std::cout << "producer wait: successful/EINTR, readable -EIO, active/unknown, failed query/poll/export and exactly-once FD cleanup passed\n";
  }

  template<class T>
  T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) {
      return reinterpret_cast<T>(value);
    } else {
      return static_cast<T>(value);
    }
  }

  enum class failure_t {
    none,
    format,
    external,
    planes,
    fd_query,
    create,
    mask,
    allocate,
    bind
  };

  struct import_mock_t {
    layout_t layout;
    VkPhysicalDeviceMemoryProperties properties {};
    failure_t failure = failure_t::none;
    uint32_t image_bits = 7, fd_bits = 2, selected = UINT32_MAX;
    int query_fd = -1, reused_fd = -1;
    int create_calls = 0, destroy_calls = 0, allocate_calls = 0, free_calls = 0, bind_calls = 0, fd_queries = 0;
    std::vector<std::string> cleanup;
  };

  import_mock_t *import_mock;

  void format_properties(VkPhysicalDevice, VkFormat format, VkFormatProperties2 *out) {
    auto &mock = *import_mock;
    require(format == validate_layout(mock.layout), "Query current native framebuffer format");
    auto list = static_cast<VkDrmFormatModifierPropertiesListEXT *>(out->pNext);
    if (list->pDrmFormatModifierProperties) {
      require(list->drmFormatModifierCount == 1, "Modifier query storage count");
      list->pDrmFormatModifierProperties[0] = {mock.layout.modifier, mock.failure == failure_t::planes ? 2u : 1u, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT};
    }
    list->drmFormatModifierCount = 1;
  }

  VkResult image_format_properties(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out) {
    auto &mock = *import_mock;
    auto ext = static_cast<const VkPhysicalDeviceExternalImageFormatInfo *>(info->pNext);
    auto mod = static_cast<const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *>(ext->pNext);
    require(ext->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && mod->drmFormatModifier == mock.layout.modifier && mod->sharingMode == VK_SHARING_MODE_EXCLUSIVE, "Query actual modifier with DMA-BUF handle type");
    require(info->format == validate_layout(mock.layout) && info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && info->usage == VK_IMAGE_USAGE_TRANSFER_SRC_BIT && !info->flags, "Query exact external image format/tiling/copy usage");
    auto external = static_cast<VkExternalImageFormatProperties *>(out->pNext);
    external->externalMemoryProperties = {VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT, 0, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    if (mock.failure == failure_t::external) {
      external->externalMemoryProperties.externalMemoryFeatures = 0;
    }
    out->imageFormatProperties = {{8192, 8192, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, 1u << 28};
    return mock.failure == failure_t::format ? VK_ERROR_FORMAT_NOT_SUPPORTED : VK_SUCCESS;
  }

  void memory_properties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *out) {
    *out = import_mock->properties;
  }

  VkResult fd_properties(VkDevice, VkExternalMemoryHandleTypeFlagBits type, int fd, VkMemoryFdPropertiesKHR *out) {
    auto &mock = *import_mock;
    ++mock.fd_queries;
    mock.query_fd = fd;
    require(type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && fd != mock.layout.fds[0] && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "Query a valid CLOEXEC duplicate, preserving original capture FD");
    out->memoryTypeBits = mock.fd_bits;
    return mock.failure == failure_t::fd_query ? VK_ERROR_INVALID_EXTERNAL_HANDLE : VK_SUCCESS;
  }

  VkResult create_image(VkDevice, const VkImageCreateInfo *info, const VkAllocationCallbacks *, VkImage *image) {
    auto &mock = *import_mock;
    ++mock.create_calls;
    auto ext = static_cast<const VkExternalMemoryImageCreateInfo *>(info->pNext);
    auto mod = static_cast<const VkImageDrmFormatModifierExplicitCreateInfoEXT *>(ext->pNext);
    require(ext->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && mod->drmFormatModifier == mock.layout.modifier && mod->drmFormatModifierPlaneCount == 1, "Native external image carries exact single-plane modifier");
    const auto &plane = mod->pPlaneLayouts[0];
    require(plane.offset == mock.layout.offsets[0] && plane.rowPitch == mock.layout.pitches[0] && !plane.size && !plane.arrayPitch && !plane.depthPitch, "Preserve actual plane pitch/offset with zero unused layout fields");
    require(info->format == validate_layout(mock.layout) && info->usage == VK_IMAGE_USAGE_TRANSFER_SRC_BIT && info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && info->sharingMode == VK_SHARING_MODE_EXCLUSIVE && !info->flags, "Create image with the queried modifier usage/format");
    require(info->extent.width == mock.layout.width && info->extent.height == mock.layout.height && info->extent.depth == 1 && info->mipLevels == 1 && info->arrayLayers == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT, "Native import extent and single subresource");
    *image = handle<VkImage>(mock.failure == failure_t::create ? 99 : 3);  // Failed out parameters are not owned.
    return mock.failure == failure_t::create ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
  }

  void destroy_image(VkDevice, VkImage image, const VkAllocationCallbacks *) {
    require(image == handle<VkImage>(3), "Destroy only the successfully created image");
    ++import_mock->destroy_calls;
    import_mock->cleanup.push_back("image");
  }

  void image_requirements(VkDevice, const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *out) {
    require(info->image == handle<VkImage>(3), "Query the created image's allocation requirements");
    out->memoryRequirements = {16u << 20, 4096, import_mock->image_bits};
    auto dedicated = static_cast<VkMemoryDedicatedRequirements *>(out->pNext);
    dedicated->requiresDedicatedAllocation = dedicated->prefersDedicatedAllocation = VK_TRUE;
  }

  VkResult allocate_memory(VkDevice, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *, VkDeviceMemory *memory) {
    auto &mock = *import_mock;
    ++mock.allocate_calls;
    auto imported = static_cast<const VkImportMemoryFdInfoKHR *>(info->pNext);
    auto dedicated = static_cast<const VkMemoryDedicatedAllocateInfo *>(imported->pNext);
    require(imported->fd == mock.query_fd && fcntl(imported->fd, F_GETFD) >= 0 && imported->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "Allocate using the exact FD queried for compatibility");
    require(dedicated->image == handle<VkImage>(3) && !dedicated->buffer && info->allocationSize == 16u << 20, "Honor dedicated-only import and exact image allocation size");
    mock.selected = info->memoryTypeIndex;
    require(mock.selected < mock.properties.memoryTypeCount && ((mock.image_bits & mock.fd_bits) & (1u << mock.selected)), "Selected allocation index must satisfy both actual masks");
    *memory = handle<VkDeviceMemory>(mock.failure == failure_t::allocate ? 99 : 4);
    if (mock.failure == failure_t::allocate) {
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    close(imported->fd);  // Successful native allocation consumes the duplicate.
    mock.reused_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    require(mock.reused_fd == imported->fd, "Reuse consumed slot to detect a later double close");
    return VK_SUCCESS;
  }

  void free_memory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks *) {
    require(memory == handle<VkDeviceMemory>(4), "Free only successfully allocated import memory");
    ++import_mock->free_calls;
    import_mock->cleanup.push_back("memory");
  }

  VkResult bind_image_memory(VkDevice, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    ++import_mock->bind_calls;
    require(image == handle<VkImage>(3) && memory == handle<VkDeviceMemory>(4) && offset == 0, "Bind dedicated image at zero; plane offset is already in modifier layout");
    return import_mock->failure == failure_t::bind ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
  }

  void test_native_import() {
    owned_fd_t source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    require(source.fd >= 0, "Create mock captured allocation FD");
    const import_api_t api {format_properties, image_format_properties, memory_properties, create_image, destroy_image, image_requirements, fd_properties, allocate_memory, free_memory, bind_image_memory};
    for (auto failure : {failure_t::none, failure_t::format, failure_t::external, failure_t::planes, failure_t::fd_query, failure_t::create, failure_t::mask, failure_t::allocate, failure_t::bind}) {
      import_mock_t mock;
      import_mock = &mock;
      mock.failure = failure;
      mock.layout.width = 1920;
      mock.layout.height = 1080;
      mock.layout.fourcc = DRM_FORMAT_ABGR2101010;
      mock.layout.modifier = DRM_FORMAT_MOD_LINEAR;
      mock.layout.fds[0] = source.fd;
      mock.layout.pitches[0] = 8192;
      mock.layout.offsets[0] = 4096;
      mock.properties.memoryTypeCount = 3;
      mock.properties.memoryTypes[0].propertyFlags = mock.properties.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      mock.properties.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      if (failure == failure_t::mask) {
        mock.image_bits = 1;  // FD allows bit 1 only.
      }
      if (failure == failure_t::none) {
        auto image = import_dma_buf(handle<VkPhysicalDevice>(1), handle<VkDevice>(2), mock.layout, api);
        require(image->image && image->memory && image->image_type_bits == 7 && image->fd_type_bits == 2 && image->type_index == 1, "Commit native image only with compatible allocation");
        require(!mock.destroy_calls && !mock.free_calls && fcntl(mock.reused_fd, F_GETFD) >= 0, "Live import retains resources and does not close reused FD slot");
        image.reset();
      } else {
        const char *expected = failure == failure_t::mask     ? "No compatible DMA-BUF/image memory type" :
                               failure == failure_t::fd_query ? "query actual DMA-BUF FD memory types" :
                               failure == failure_t::create   ? "create actual DMA-BUF image" :
                               failure == failure_t::allocate ? "allocate compatible dedicated DMA-BUF memory" :
                               failure == failure_t::bind     ? "bind DMA-BUF memory" :
                                                                "modifier";
        expect_error([&] {
          import_dma_buf(handle<VkPhysicalDevice>(1), handle<VkDevice>(2), mock.layout, api);
        },
                     expected);
      }
      bool created = failure == failure_t::none || failure == failure_t::mask || failure == failure_t::allocate || failure == failure_t::bind;
      bool allocated = failure == failure_t::none || failure == failure_t::bind;
      require(mock.destroy_calls == int(created) && mock.free_calls == int(allocated), "Transactional image/memory cleanup at every failure stage");
      if (allocated) {
        require(mock.cleanup == std::vector<std::string> {"image", "memory"} && mock.selected == 1, "Destroy image before freeing its compatible dedicated memory");
        require(fcntl(mock.reused_fd, F_GETFD) >= 0, "Allocation consumption prevents double-close on bind failure/destruction");
        close(mock.reused_fd);
      } else if (mock.query_fd >= 0) {
        require(fcntl(mock.query_fd, F_GETFD) == -1, "Failure closes the unconsumed duplicate");
      }
      if (failure == failure_t::mask || failure == failure_t::fd_query || failure == failure_t::create) {
        require(!mock.allocate_calls, "Reject incompatible/query/create failure before allocation");
      }
      require(fcntl(source.fd, F_GETFD) >= 0, "Capture allocation FD survives every native import outcome");
      require(import_memory_type(7, 4, mock.properties) == 2, "Compatible non-device-local memory is an explicit fallback");
      require(import_memory_type(6, 6, mock.properties) == 1, "Prefer compatible device-local memory");
      expect_error([&] {
        import_memory_type(7, 0, mock.properties);
      },
                   "No compatible");
      mock.properties.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_PROTECTED_BIT;
      expect_error([&] {
        import_memory_type(4, 4, mock.properties);
      },
                   "No compatible");
    }
    for (auto fourcc : {DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR2101010, DRM_FORMAT_ARGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_XRGB2101010}) {
      import_mock_t mock;
      import_mock = &mock;
      mock.layout = {2560, 1440, fourcc, 4097};
      mock.layout.fds[0] = source.fd;
      mock.layout.pitches[0] = 10496;
      mock.layout.offsets[0] = 4096;
      mock.properties.memoryTypeCount = 3;
      mock.properties.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      {
        auto image = import_dma_buf(handle<VkPhysicalDevice>(1), handle<VkDevice>(2), mock.layout, api);
        require(mock.fd_queries == 1 && image->fd_type_bits == 2 && image->type_index == 1, "Each supported format rechecks actual modifier and FD memory compatibility");
      }
      require(mock.cleanup == std::vector<std::string> {"image", "memory"}, "Each native format import releases after caller completion");
      close(mock.reused_fd);
    }
    std::cout << "native DMA-BUF import mocks: six formats, actual duplicate mask intersection, dedicated modifier/layout/usage, rejected queries/masks, create/allocate/bind failures and transactional cleanup passed\n";
  }

  struct snapshot_mock_t {
    failure_t failure = failure_t::none;
    bool fail_view = false;
    int created = 0, allocated = 0, bound = 0, views = 0, writes = 0;
    std::vector<std::string> cleanup;
    VkImageView descriptor_view = VK_NULL_HANDLE;
  };

  snapshot_mock_t *snapshot_mock;

  const snapshot_api_t snapshot_api {
    [](VkDevice, const VkImageCreateInfo *info, const VkAllocationCallbacks *, VkImage *image) {
      auto &mock = *snapshot_mock;
      require(info->extent.width == 2560 && info->extent.height == 1440 && info->extent.depth == 1, "Snapshot keeps initial source extent");
      require(info->tiling == VK_IMAGE_TILING_OPTIMAL && info->usage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) && info->mipLevels == 1 && info->arrayLayers == 1, "Owned snapshot is sampled/copied, single-subresource optimal image");
      *image = handle<VkImage>(++mock.created);
      return mock.failure == failure_t::create ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
    },
    [](VkDevice, VkImage image, const VkAllocationCallbacks *) {
      snapshot_mock->cleanup.push_back("image" + std::to_string(reinterpret_cast<uintptr_t>(image)));
    },
    [](VkDevice, VkImage, VkMemoryRequirements *out) {
      *out = {16u << 20, 4096, 7};
    },
    [](VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *out) {
      out->memoryTypeCount = 3;
      out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT;
      out->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    },
    [](VkDevice, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *, VkDeviceMemory *memory) {
      auto &mock = *snapshot_mock;
      require(info->allocationSize == 16u << 20 && info->memoryTypeIndex == 2 && !info->pNext, "Snapshot uses compatible unprotected device-local allocation");
      *memory = handle<VkDeviceMemory>(++mock.allocated);
      return mock.failure == failure_t::allocate ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
    },
    [](VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks *) {
      snapshot_mock->cleanup.push_back("memory" + std::to_string(reinterpret_cast<uintptr_t>(memory)));
    },
    [](VkDevice, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
      auto &mock = *snapshot_mock;
      require(image && memory && !offset, "Bind owned snapshot with zero memory offset");
      ++mock.bound;
      return mock.failure == failure_t::bind ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
    },
    [](VkDevice, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *, VkImageView *view) {
      auto &mock = *snapshot_mock;
      require(info->image && info->viewType == VK_IMAGE_VIEW_TYPE_2D && info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && info->subresourceRange.levelCount == 1 && info->subresourceRange.layerCount == 1, "Alpha view refers to active snapshot color subresource");
      require(info->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || info->format == VK_FORMAT_A2R10G10B10_UNORM_PACK32, "Alpha-bearing snapshot view retains channel order");
      *view = handle<VkImageView>(++mock.views);
      return mock.fail_view ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
    },
    [](VkDevice, VkImageView view, const VkAllocationCallbacks *) {
      snapshot_mock->cleanup.push_back("view" + std::to_string(reinterpret_cast<uintptr_t>(view)));
    },
    [](VkDevice, uint32_t count, const VkWriteDescriptorSet *writes, uint32_t copies, const VkCopyDescriptorSet *) {
      auto &mock = *snapshot_mock;
      require(count == 1 && !copies && writes->dstSet == handle<VkDescriptorSet>(50) && writes->dstBinding == 0 && writes->descriptorCount == 1 && writes->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, "Rebind only alpha image descriptor on the existing set");
      require(writes->pImageInfo->sampler == handle<VkSampler>(51) && writes->pImageInfo->imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "Alpha samples current snapshot in shader-read layout");
      mock.descriptor_view = writes->pImageInfo->imageView;
      ++mock.writes;
    }
  };

  void test_snapshot_transitions() {
    snapshot_mock_t mock;
    snapshot_mock = &mock;
    {
      snapshots_t snapshots(VK_NULL_HANDLE, VK_NULL_HANDLE, snapshot_api);
      layout_t previous;
      previous.width = 2560;
      previous.height = 1440;
      previous.fourcc = DRM_FORMAT_XRGB8888;
      previous.modifier = DRM_FORMAT_MOD_LINEAR;
      previous.fds[0] = 10;
      previous.pitches[0] = 10240;
      auto &first = snapshots.select(2560, 1440, validate_live_layout(previous, nullptr), true);
      first.initialized = true;
      for (int repeat = 0; repeat < 100; ++repeat) {
        for (auto fourcc : {DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR2101010, DRM_FORMAT_ARGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_XRGB2101010}) {
          auto next = previous;
          next.fourcc = fourcc;
          next.pitches[0] = 10240 + repeat * 256;
          next.offsets[0] = repeat * 4096;
          next.modifier = DRM_FORMAT_MOD_LINEAR + repeat;
          const auto format = validate_live_layout(next, &previous);
          auto &active = snapshots.select(next.width, next.height, format, true);
          if (fourcc == DRM_FORMAT_XRGB8888) {
            require(&active == &first && active.initialized, "Return to earlier format reuses image and shader-read initialization state");
          }
          if (requires_opaque_alpha(fourcc)) {
            active.bind_alpha(handle<VkDescriptorSet>(50), handle<VkSampler>(51), format);
            require(mock.descriptor_view == active.alpha_view, "Alpha descriptor follows each A2B/A2R transition");
          }
          previous = next;
          require(mock.cleanup.empty(), "Keep snapshots/views/memory alive across codec view retention and repeated transitions");
        }
      }
      require(mock.created == 4 && mock.allocated == 4 && mock.bound == 4 && mock.views == 2 && mock.writes == 200, "Six supported fourccs and modifier/layout changes remain bounded to four images and two alpha views");
      for (auto bad : {VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16B16A16_SFLOAT}) {
        expect_error([&] {
          snapshots.select(2560, 1440, bad, true);
        },
                     "Unsupported snapshot VkFormat");
      }
      expect_error([&] {
        snapshots.select(1920, 1080, VK_FORMAT_B8G8R8A8_UNORM, true);
      },
                   "reconnect required to update input mapping");
      require(mock.created == 4 && mock.cleanup.empty(), "Reject invalid transitions before altering resource ownership");
    }
    require(mock.cleanup == std::vector<std::string> {"view2", "image4", "memory4", "view1", "image3", "memory3", "image2", "memory2", "image1", "memory1"}, "Destroy alpha view before image before memory, exactly once per retained snapshot");
    for (auto failure : {failure_t::create, failure_t::allocate, failure_t::bind}) {
      snapshot_mock_t failing;
      snapshot_mock = &failing;
      snapshots_t snapshots(VK_NULL_HANDLE, VK_NULL_HANDLE, snapshot_api);
      auto &first = snapshots.select(2560, 1440, VK_FORMAT_B8G8R8A8_UNORM, true);
      failing.failure = failure;
      expect_error([&] {
        snapshots.select(2560, 1440, VK_FORMAT_R8G8B8A8_UNORM, true);
      },
                   "snapshot");
      require(&snapshots.select(2560, 1440, VK_FORMAT_B8G8R8A8_UNORM, true) == &first, "Failed transition preserves old snapshot");
      const std::vector<std::string> expected = failure == failure_t::create   ? std::vector<std::string> {} :
                                                failure == failure_t::allocate ? std::vector<std::string> {"image2"} :
                                                                                 std::vector<std::string> {"image2", "memory2"};
      require(failing.cleanup == expected, "Failed snapshot output handles are not owned; unwind only successfully allocated resources");
    }
    snapshot_mock_t view_failure;
    snapshot_mock = &view_failure;
    {
      snapshots_t snapshots(VK_NULL_HANDLE, VK_NULL_HANDLE, snapshot_api);
      auto &active = snapshots.select(2560, 1440, VK_FORMAT_A2B10G10R10_UNORM_PACK32, true);
      view_failure.fail_view = true;
      expect_error([&] {
        active.bind_alpha(handle<VkDescriptorSet>(50), handle<VkSampler>(51), VK_FORMAT_A2B10G10R10_UNORM_PACK32);
      },
                   "alpha snapshot view");
      require(!active.alpha_view && !view_failure.writes, "Failed alpha view never reaches descriptors or destruction");
    }
    require(view_failure.cleanup == std::vector<std::string> {"image1", "memory1"}, "Alpha-view failure preserves image ownership through cleanup");
    snapshot_mock_t diagnostic;
    snapshot_mock = &diagnostic;
    snapshots_t snapshots(VK_NULL_HANDLE, VK_NULL_HANDLE, snapshot_api);
    snapshots.select(2560, 1440, VK_FORMAT_B8G8R8A8_UNORM, false);
    expect_error([&] {
      snapshots.select(2560, 1440, VK_FORMAT_R8G8B8A8_UNORM, false);
    },
                 "Diagnostic framebuffer VkFormat changed");
    std::cout << "snapshot transitions: six fourccs, bounded reuse/layouts, alpha rebinding, transactional allocation/view failures, fixed extent and diagnostic guards passed\n";
  }

  void test_live_layout_rejections() {
    layout_t valid;
    valid.width = 2560;
    valid.height = 1440;
    valid.fourcc = DRM_FORMAT_XRGB8888;
    valid.modifier = DRM_FORMAT_MOD_LINEAR;
    valid.fds[0] = 10;
    valid.pitches[0] = 10240;
    const auto description = describe_layout(valid);
    require(description.find("2560x1440 fourcc=XR24(0x34325258) modifier=0x0 plane0 pitch=10240 offset=0") != std::string::npos, "Layout diagnostic identifies exact format and layout without FD numbers");
    auto reused = valid;
    reused.fds[0] = 100;
    reused.pitches[1] = 123;  // Unrepresented plane fields are unspecified.
    require(same_layout(valid, reused), "FD churn and inactive-plane garbage are not framebuffer changes");
    for (int failure = 0; failure < 8; ++failure) {
      auto bad = valid;
      switch (failure) {
        case 0:
          bad.fourcc = DRM_FORMAT_NV12;
          break;
        case 1:
          bad.modifier = DRM_FORMAT_MOD_INVALID;
          break;
        case 2:
          bad.fds[1] = 20;
          break;
        case 3:
          bad.pitches[0] = 100;
          break;
        case 4:
          bad.width = 4096;
          bad.height = 2304;
          bad.pitches[0] = 16384;
          break;
        case 5:
          bad.width = 1920;
          bad.height = 1080;
          break;
        case 6:
          bad.height = 1400;
          break;
        case 7:
          bad.fds[0] = -1;
          break;
      }
      expect_error([&] {
        validate_live_layout(bad, &valid);
      },
                   describe_layout(bad).c_str());
    }
    std::cout << "live layout: unsupported fourcc/modifier/planes/pitch/FD, oversized/non-16:9/changed extents fail closed with details passed\n";
  }
}  // namespace

namespace pyrowave_diag {
  void test_live_import_contracts() {
    test_producer_wait();
    test_native_import();
    test_snapshot_transitions();
    test_live_layout_rejections();
  }
}  // namespace pyrowave_diag
