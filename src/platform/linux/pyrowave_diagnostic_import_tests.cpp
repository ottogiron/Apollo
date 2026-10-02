/** @brief CPU mocks exercise the actual live wait/import helpers; no real capture. */
#include "pyrowave_diagnostic_import.h"
#include "pyrowave_diagnostic_sync.h"

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
    require(format == VK_FORMAT_A2B10G10R10_UNORM_PACK32, "Native AB30 format query");
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
    require(info->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && info->usage == VK_IMAGE_USAGE_TRANSFER_SRC_BIT && !info->flags, "Query exact external image format/tiling/copy usage");
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
    require(info->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && info->usage == VK_IMAGE_USAGE_TRANSFER_SRC_BIT && info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && info->sharingMode == VK_SHARING_MODE_EXCLUSIVE && !info->flags, "Create image with the queried modifier usage/format");
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
    std::cout << "native DMA-BUF import mocks: actual duplicate mask intersection, dedicated modifier/layout/usage, rejected queries/masks, create/allocate/bind failures and transactional cleanup passed\n";
  }
}  // namespace

namespace pyrowave_diag {
  void test_live_import_contracts() {
    test_producer_wait();
    test_native_import();
  }
}  // namespace pyrowave_diag
