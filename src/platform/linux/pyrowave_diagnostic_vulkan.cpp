/** @brief Explicit DMA-BUF import, same-GPU checks and measured snapshot copies. */
#include "pyrowave_diagnostic_vulkan.h"

#include "pyrowave_capture.h"
#include "pyrowave_diagnostic_sync.h"

#include <chrono>
#include <cstring>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace pyrowave_diag {
  void checked(pyrowave_result result, const char *operation) {
    if (result != PYROWAVE_SUCCESS) {
      throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
    }
  }

  void checked_vk(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
      throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
    }
  }

  namespace {
    bool has_extension(VkPhysicalDevice physical, const char *name) {
      uint32_t count = 0;
      checked_vk(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "enumerate extensions");
      std::vector<VkExtensionProperties> props(count);
      checked_vk(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, props.data()), "read extensions");
      return std::any_of(props.begin(), props.end(), [&](const auto &p) {
        return !strcmp(p.extensionName, name);
      });
    }

    bool matches(VkPhysicalDevice physical, const platf::kms_diagnostic_info_t &identity) {
      VkPhysicalDeviceProperties2 props {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      if (has_extension(physical, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
        VkPhysicalDeviceDrmPropertiesEXT drm {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        props.pNext = &drm;
        vkGetPhysicalDeviceProperties2(physical, &props);
        if (drm.hasPrimary && drm.primaryMajor == major(identity.primary_device) && drm.primaryMinor == minor(identity.primary_device)) {
          return true;
        }
      }
      if (identity.has_pci && has_extension(physical, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) {
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        props.pNext = &pci;
        vkGetPhysicalDeviceProperties2(physical, &props);
        return pci.pciDomain == identity.pci_domain && pci.pciBus == identity.pci_bus &&
               pci.pciDevice == identity.pci_device && pci.pciFunction == identity.pci_function;
      }
      return false;
    }

    void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access, uint32_t src_family = VK_QUEUE_FAMILY_IGNORED, uint32_t dst_family = VK_QUEUE_FAMILY_IGNORED) {
      VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = src_access;
      barrier.dstAccessMask = dst_access;
      barrier.oldLayout = old_layout;
      barrier.newLayout = new_layout;
      barrier.srcQueueFamilyIndex = src_family;
      barrier.dstQueueFamilyIndex = dst_family;
      barrier.image = image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

  }  // namespace

  gpu_t::~gpu_t() {
    if (device) {
      vkDeviceWaitIdle(device);
    }
    release_import();
    if (decoder) {
      pyrowave_decoder_destroy(decoder);
    }
    if (encoder) {
      pyrowave_encoder_destroy(encoder);
    }
    if (pyro) {
      pyrowave_device_destroy(pyro);
    }
    if (device) {
      if (mapped) {
        vkUnmapMemory(device, buffer_memory);
      }
      if (fence) {
        vkDestroyFence(device, fence, nullptr);
      }
      if (completion) {
        vkDestroySemaphore(device, completion, nullptr);
      }
      if (copy_release) {
        vkDestroySemaphore(device, copy_release, nullptr);
      }
      if (pool) {
        vkDestroyCommandPool(device, pool, nullptr);
      }
      if (buffer) {
        vkDestroyBuffer(device, buffer, nullptr);
      }
      if (owned_image) {
        vkDestroyImage(device, owned_image, nullptr);
      }
      if (buffer_memory) {
        vkFreeMemory(device, buffer_memory, nullptr);
      }
      if (image_memory) {
        vkFreeMemory(device, image_memory, nullptr);
      }
      vkDestroyDevice(device, nullptr);
    }
    if (instance) {
      vkDestroyInstance(instance, nullptr);
    }
  }

  void gpu_t::init(const platf::kms_diagnostic_info_t *identity) {
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "apollo-pyrowave-diagnostic";
    instance_info.pApplicationInfo = &app;
    checked_vk(vkCreateInstance(&instance_info, nullptr, &instance), "create Vulkan instance");
    uint32_t count = 0;
    checked_vk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPUs");
    std::vector<VkPhysicalDevice> devices(count);
    checked_vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "read GPUs");
    for (auto candidate : devices) {
      VkPhysicalDeviceProperties properties {};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      if (properties.apiVersion < VK_API_VERSION_1_3 || (identity && !matches(candidate, *identity))) {
        continue;
      }
      if (physical) {
        throw std::runtime_error("Ambiguous same-GPU ICD match; select a single Vulkan ICD externally");
      }
      physical = candidate;
      if (!identity) {
        break;
      }
    }
    if (!physical) {
      throw std::runtime_error("No Vulkan 1.3 device matches the capture DRM node or PCI identity");
    }
    same_gpu_checked = identity != nullptr;
    extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    for (auto ext : extensions) {
      if (!has_extension(physical, ext)) {
        throw std::runtime_error(std::string("Required Vulkan extension unsupported: ") + ext);
      }
    }
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues.data());
    auto it = std::find_if(queues.begin(), queues.end(), [](const auto &q) {
      return q.queueCount && (q.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    });
    if (it == queues.end()) {
      throw std::runtime_error("No graphics+compute queue");
    }
    family = uint32_t(it - queues.begin());
    features.pNext = &features11;
    features11.pNext = &features12;
    features12.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!features.features.shaderInt16 || !features12.storageBuffer8BitAccess || !features12.shaderFloat16 || !features12.timelineSemaphore || !features13.subgroupSizeControl || !features13.computeFullSubgroups || !features13.synchronization2) {
      throw std::runtime_error("Required Pyrowave/Vulkan shader or synchronization features unavailable");
    }
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = uint32_t(extensions.size());
    device_info.ppEnabledExtensionNames = extensions.data();
    checked_vk(vkCreateDevice(physical, &device_info, nullptr, &device), "create Vulkan device");
    vkGetDeviceQueue(device, family, 0, &queue);
    pyrowave_device_create_info info {};
    info.GetInstanceProcAddr = vkGetInstanceProcAddr;
    info.instance = instance;
    info.physical_device = physical;
    info.device = device;
    info.instance_create_info = &instance_info;
    info.device_create_info = &device_info;
    checked(pyrowave_create_device(&info, &pyro), "borrow diagnostic Vulkan device");
    pyrowave_encoder_create_info encode_info {pyro, output_width, output_height, PYROWAVE_CHROMA_SUBSAMPLING_420};
    checked(pyrowave_encoder_create(&encode_info, &encoder), "create encoder");
    pyrowave_decoder_create_info decode_info {pyro, output_width, output_height, PYROWAVE_CHROMA_SUBSAMPLING_420, false};
    checked(pyrowave_decoder_create(&decode_info, &decoder), "create decoder");
    VkCommandPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    checked_vk(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "create command pool");
    VkCommandBufferAllocateInfo allocate {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    checked_vk(vkAllocateCommandBuffers(device, &allocate, &command), "allocate command buffer");
    VkFenceCreateInfo fence_info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    checked_vk(vkCreateFence(device, &fence_info, nullptr, &fence), "create copy fence");
    VkSemaphoreTypeCreateInfo type {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore_info {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    checked_vk(vkCreateSemaphore(device, &semaphore_info, nullptr, &completion), "create encode completion timeline");
    VkPhysicalDeviceExternalSemaphoreInfo external_info {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    external_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalSemaphoreProperties semaphore_properties {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &external_info, &semaphore_properties);
    if (!(semaphore_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
      throw std::runtime_error("SYNC_FD copy-completion export is unsupported");
    }
    VkExportSemaphoreCreateInfo export_info {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo copy_info {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &export_info};
    checked_vk(vkCreateSemaphore(device, &copy_info, nullptr, &copy_release), "create copy-completion export semaphore");
  }

  uint32_t gpu_t::memory_type(uint32_t mask, VkMemoryPropertyFlags flags, VkMemoryPropertyFlags preferred) const {
    VkPhysicalDeviceMemoryProperties properties {};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (auto desired : {flags | preferred, flags}) {
      for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((mask & (1u << i)) && (properties.memoryTypes[i].propertyFlags & desired) == desired) {
          return i;
        }
      }
    }
    throw std::runtime_error("Required Vulkan memory type unavailable");
  }

  void gpu_t::prepare(uint32_t w, uint32_t h, VkFormat f) {
    if (owned_image) {
      if (w != width || h != height || f != format) {
        throw std::runtime_error("Framebuffer layout changed; stop and rerun");
      }
      return;
    }
    width = w;
    height = h;
    format = f;
    VkImageCreateInfo image {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {width, height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checked_vk(vkCreateImage(device, &image, nullptr, &owned_image), "create owned snapshot");
    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device, owned_image, &requirements);
    VkMemoryAllocateInfo alloc {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checked_vk(vkAllocateMemory(device, &alloc, nullptr, &image_memory), "allocate snapshot memory");
    checked_vk(vkBindImageMemory(device, owned_image, image_memory, 0), "bind snapshot memory");
    VkBufferCreateInfo buf {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf.size = uint64_t(width) * height * 4;
    buf.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checked_vk(vkCreateBuffer(device, &buf, nullptr, &buffer), "create reference buffer");
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    alloc.allocationSize = requirements.size;
    // Reference/alpha readback is consumed by the CPU. Prefer cached host
    // memory instead of repeatedly scanning an uncached/coherent BAR mapping.
    alloc.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    reference_memory_properties = memory_properties.memoryTypes[alloc.memoryTypeIndex].propertyFlags;
    checked_vk(vkAllocateMemory(device, &alloc, nullptr, &buffer_memory), "allocate reference buffer memory");
    checked_vk(vkBindBufferMemory(device, buffer, buffer_memory, 0), "bind reference buffer");
    checked_vk(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map reference buffer");
  }

  void gpu_t::import(const layout_t &layout) {
    auto f = validate_layout(layout);
    prepare(layout.width, layout.height, f);
    if (imported) {
      throw std::runtime_error("Previous import was not released after encode completion");
    }
    import_api_t api {vkGetPhysicalDeviceFormatProperties2, vkGetPhysicalDeviceImageFormatProperties2, vkGetPhysicalDeviceMemoryProperties, vkCreateImage, vkDestroyImage, vkGetImageMemoryRequirements2, reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR")), vkAllocateMemory, vkFreeMemory, vkBindImageMemory};
    auto candidate = import_dma_buf(physical, device, layout, api);
    auto begin = std::chrono::steady_clock::now();
    wait_producer(layout.fds[0]);
    producer_wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    import_image_type_bits = candidate->image_type_bits;
    import_fd_type_bits = candidate->fd_type_bits;
    import_type_index = candidate->type_index;
    imported = std::move(candidate);  // Commit only a bound image with a successful writer wait.
    capture_dma_fd = layout.fds[0];
  }

  void gpu_t::snapshot(const std::vector<uint8_t> *synthetic) {
    if (synthetic) {
      if (synthetic->size() != uint64_t(width) * height * 4) {
        throw std::runtime_error("Invalid synthetic upload size");
      }
      memcpy(mapped, synthetic->data(), synthetic->size());
    } else if (!imported) {
      throw std::runtime_error("No DMA-BUF image to snapshot");
    }
    checked_vk(vkResetCommandBuffer(command, 0), "reset copy command");
    checked_vk(vkResetFences(device, 1, &fence), "reset copy fence");
    VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked_vk(vkBeginCommandBuffer(command, &begin), "begin snapshot command");
    image_barrier(command, owned_image, initialized_image ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, initialized_image ? VK_ACCESS_SHADER_READ_BIT : 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    if (synthetic) {
      vkCmdCopyBufferToImage(command, buffer, owned_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
      auto external = imported->image;
      image_barrier(command, external, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_FOREIGN_EXT, family);
      VkImageCopy copy {};
      copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.extent = {width, height, 1};
      vkCmdCopyImage(command, external, VK_IMAGE_LAYOUT_GENERAL, owned_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      image_barrier(command, external, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, family, VK_QUEUE_FAMILY_FOREIGN_EXT);
    }
    image_barrier(command, owned_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    checked_vk(vkEndCommandBuffer(command), "end snapshot command");
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (!synthetic) {
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &copy_release;
    }
    checked_vk(vkQueueSubmit(queue, 1, &submit, fence), "submit snapshot copy");
    if (!synthetic) {
      auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
      if (!get_fd) {
        throw std::runtime_error("Cannot load SYNC_FD export function");
      }
      VkSemaphoreGetFdInfoKHR info {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
      info.semaphore = copy_release;
      info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      int release_fd = -1;
      checked_vk(get_fd(device, &info, &release_fd), "export snapshot read-completion fence");
      // -1 means the SYNC_FD is already signaled: nothing pending to publish.
      if (release_fd >= 0) {
        dma_buf_import_sync_file publish {};
        publish.flags = DMA_BUF_SYNC_READ;
        publish.fd = release_fd;
        int result = ioctl(capture_dma_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &publish);
        close(release_fd);  // The ioctl borrows rather than consumes this FD.
        if (result) {
          throw std::runtime_error("Publishing DMA-BUF READ completion fence failed");
        }
      }
      // Export/wait/submit/publication is not atomic. This is not a compositor lease.
    }
    checked_vk(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ULL), "wait snapshot copy (5 second timeout)");
    initialized_image = true;
  }

  std::vector<uint8_t> gpu_t::reference_pixels() const {
    auto start = static_cast<const uint8_t *>(mapped);
    return {start, start + size_t(width) * height * 4};
  }

  void gpu_t::readback_snapshot() {
    checked_vk(vkResetCommandBuffer(command, 0), "reset reference command");
    checked_vk(vkResetFences(device, 1, &fence), "reset reference fence");
    VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked_vk(vkBeginCommandBuffer(command, &begin), "begin reference command");
    image_barrier(command, owned_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command, owned_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    VkMemoryBarrier host {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    image_barrier(command, owned_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    checked_vk(vkEndCommandBuffer(command), "end reference command");
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    checked_vk(vkQueueSubmit(queue, 1, &submit, fence), "submit reference readback");
    checked_vk(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ULL), "wait reference readback");
  }

  pyrowave_image_view gpu_t::snapshot_view() const {
    return {owned_image, width, height, format, format, 0, 0, VK_IMAGE_ASPECT_COLOR_BIT, VK_COMPONENT_SWIZZLE_IDENTITY, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }

  void gpu_t::release_import() {
    imported.reset();
    capture_lifetime.reset();
    capture_dma_fd = -1;
  }

  void gpu_t::wait_encode(uint64_t value) {
    VkSemaphoreWaitInfo info {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &completion;
    info.pValues = &value;
    checked_vk(vkWaitSemaphores(device, &info, 5'000'000'000ULL), "wait encode timeline (5 second timeout)");
  }
}  // namespace pyrowave_diag
