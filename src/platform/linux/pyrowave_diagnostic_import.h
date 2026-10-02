/** @brief Native, transactional DMA-BUF import on the device borrowed by Pyrowave. */
#pragma once

#include "pyrowave_diagnostic_fd.h"
#include "pyrowave_diagnostic_support.h"

#include <memory>

namespace pyrowave_diag {
  // The live adapter supplies Vulkan entry points; CPU regressions supply mocks
  // to execute the same query/allocation/binding and cleanup path without a GPU.
  struct import_api_t {
    PFN_vkGetPhysicalDeviceFormatProperties2 format_properties;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_format_properties;
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements2 image_requirements;
    PFN_vkGetMemoryFdPropertiesKHR fd_properties;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
  };

  inline void import_checked(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
      throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
    }
  }

  inline uint32_t import_memory_type(uint32_t image_bits, uint32_t fd_bits, const VkPhysicalDeviceMemoryProperties &properties) {
    uint32_t compatible = image_bits & fd_bits;
    for (auto preferred : {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VkMemoryPropertyFlagBits(0)}) {
      for (uint32_t i = 0; i < std::min(properties.memoryTypeCount, uint32_t(VK_MAX_MEMORY_TYPES)); ++i) {
        auto flags = properties.memoryTypes[i].propertyFlags;
        if ((compatible & (1u << i)) && !(flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) && (flags & preferred) == preferred) {
          return i;
        }
      }
    }
    throw std::runtime_error("No compatible DMA-BUF/image memory type (image mask " + std::to_string(image_bits) + ", FD mask " + std::to_string(fd_bits) + ")");
  }

  class dma_buf_image_t {
  public:
    dma_buf_image_t(VkDevice device, const import_api_t &api):
        device(device),
        api(api) {}

    dma_buf_image_t(const dma_buf_image_t &) = delete;
    dma_buf_image_t &operator=(const dma_buf_image_t &) = delete;

    ~dma_buf_image_t() {
      // Owner must finish GPU use before releasing a committed import. Local
      // creation failures have submitted no commands and unwind immediately.
      if (image) {
        api.destroy_image(device, image, nullptr);
      }
      if (memory) {
        api.free_memory(device, memory, nullptr);
      }
    }

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t image_type_bits = 0, fd_type_bits = 0, type_index = 0;

  private:
    VkDevice device;
    import_api_t api;
  };

  inline std::unique_ptr<dma_buf_image_t> import_dma_buf(VkPhysicalDevice physical, VkDevice device, const layout_t &layout, const import_api_t &api) {
    auto format = validate_layout(layout);
    if (!api.fd_properties) {
      throw std::runtime_error("vkGetMemoryFdPropertiesKHR is unavailable; refusing unchecked DMA-BUF import");
    }
    VkDrmFormatModifierPropertiesListEXT list {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
    api.format_properties(physical, format, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = modifiers.data();
    api.format_properties(physical, format, &props);
    auto found = std::find_if(modifiers.begin(), modifiers.end(), [&](const auto &m) {
      return m.drmFormatModifier == layout.modifier;
    });
    if (found == modifiers.end() || found->drmFormatModifierPlaneCount != 1 || !(found->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
      throw std::runtime_error("Actual DRM modifier is unsupported, lacks transfer-source support, or needs unrepresented auxiliary planes");
    }

    // Only the external image's copy usage is queried. Pyrowave samples the
    // separate owned snapshot, never this compositor-owned image.
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT query_modifier {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    query_modifier.drmFormatModifier = layout.modifier;
    query_modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo external_query {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &query_modifier};
    external_query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 query {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external_query};
    query.format = format;
    query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    query.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkExternalImageFormatProperties external_properties {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 supported {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &external_properties};
    import_checked(api.image_format_properties(physical, &query, &supported), "query actual modifier/usage/DMA-BUF support");
    const auto &external = external_properties.externalMemoryProperties;
    const auto &limits = supported.imageFormatProperties;
    if (!(external.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) || !(external.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) || layout.width > limits.maxExtent.width || layout.height > limits.maxExtent.height || !limits.maxExtent.depth || !limits.maxMipLevels || !limits.maxArrayLayers || !(limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
      throw std::runtime_error("Actual modifier/usage/extent is not supported for DMA-BUF import");
    }

    // Query the very duplicate subsequently consumed by vkAllocateMemory.
    import_fd_t duplicate(layout.fds[0]);
    VkMemoryFdPropertiesKHR fd_properties {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    import_checked(api.fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, duplicate.fd, &fd_properties), "query actual DMA-BUF FD memory types");
    auto result = std::make_unique<dma_buf_image_t>(device, api);
    VkSubresourceLayout plane {};
    plane.offset = layout.offsets[0];
    plane.rowPitch = layout.pitches[0];
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modifier.drmFormatModifier = layout.modifier;
    modifier.drmFormatModifierPlaneCount = 1;
    modifier.pPlaneLayouts = &plane;
    VkExternalMemoryImageCreateInfo external_image {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modifier};
    external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo image {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external_image};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {layout.width, layout.height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image.usage = query.usage;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkImage created = VK_NULL_HANDLE;
    import_checked(api.create_image(device, &image, nullptr, &created), "create actual DMA-BUF image");
    result->image = created;
    VkMemoryDedicatedRequirements dedicated_requirements {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated_requirements};
    VkImageMemoryRequirementsInfo2 requirements_info {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    requirements_info.image = result->image;
    api.image_requirements(device, &requirements_info, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties {};
    api.memory_properties(physical, &memory_properties);
    result->image_type_bits = requirements.memoryRequirements.memoryTypeBits;
    result->fd_type_bits = fd_properties.memoryTypeBits;
    result->type_index = import_memory_type(result->image_type_bits, result->fd_type_bits, memory_properties);
    VkMemoryDedicatedAllocateInfo dedicated {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = result->image;  // Always dedicated, including DEDICATED_ONLY modifiers.
    VkImportMemoryFdInfoKHR import {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &dedicated};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = duplicate.fd;
    VkMemoryAllocateInfo allocation {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
    allocation.allocationSize = requirements.memoryRequirements.size;
    allocation.memoryTypeIndex = result->type_index;
    VkDeviceMemory allocated = VK_NULL_HANDLE;
    import_checked(api.allocate_memory(device, &allocation, nullptr, &allocated), "allocate compatible dedicated DMA-BUF memory");
    duplicate.consumed();  // Vulkan takes the FD only after successful allocation.
    result->memory = allocated;
    import_checked(api.bind_image_memory(device, result->image, result->memory, 0), "bind DMA-BUF memory");
    return result;
  }
}  // namespace pyrowave_diag
