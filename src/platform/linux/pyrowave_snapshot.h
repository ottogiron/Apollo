/** @brief Bounded snapshot ownership shared by Vulkan capture and CPU fault injection. */
#pragma once

#include "pyrowave_diagnostic_import.h"

namespace pyrowave_diag {
  struct snapshot_api_t {
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements image_requirements;
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
    PFN_vkCreateImageView create_view;
    PFN_vkDestroyImageView destroy_view;
    PFN_vkUpdateDescriptorSets update_descriptors;
  };

  struct snapshot_t {
    snapshot_t(VkDevice device, const snapshot_api_t &api):
        device(device),
        api(api) {}

    snapshot_t(const snapshot_t &) = delete;
    snapshot_t &operator=(const snapshot_t &) = delete;

    ~snapshot_t() {
      if (alpha_view) {
        api.destroy_view(device, alpha_view, nullptr);
      }
      if (image) {
        api.destroy_image(device, image, nullptr);
      }
      if (memory) {
        api.free_memory(device, memory, nullptr);
      }
    }

    // Called only after the previous copy/alpha/encode waits have succeeded.
    void bind_alpha(VkDescriptorSet set, VkSampler sampler, VkFormat format) {
      if (!alpha_view) {
        VkImageViewCreateInfo info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView candidate = VK_NULL_HANDLE;
        import_checked(api.create_view(device, &info, nullptr, &candidate), "create alpha snapshot view");
        alpha_view = candidate;
      }
      VkDescriptorImageInfo info {sampler, alpha_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = set;
      write.dstBinding = 0;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &info;
      api.update_descriptors(device, 1, &write, 0, nullptr);
    }

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView alpha_view = VK_NULL_HANDLE;
    bool initialized = false;

  private:
    VkDevice device;
    snapshot_api_t api;
  };

  class snapshots_t {
  public:
    snapshots_t(VkPhysicalDevice physical, VkDevice device, const snapshot_api_t &api):
        physical(physical),
        device(device),
        api(api) {}

    snapshot_t &select(uint32_t w, uint32_t h, VkFormat format, bool live) {
      // Six accepted DRM formats map to exactly four VkFormats. Retain each
      // snapshot until Pyrowave device destruction drains Granite's deferred
      // VkImageViews; an encode timeline wait alone does not destroy those views.
      size_t index;
      switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
          index = 0;
          break;
        case VK_FORMAT_R8G8B8A8_UNORM:
          index = 1;
          break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
          index = 2;
          break;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
          index = 3;
          break;
        default:
          throw std::runtime_error("Unsupported snapshot VkFormat: " + std::to_string(format));
      }
      if (!w || !h || w > 8192 || h > 8192) {
        throw std::runtime_error("Invalid snapshot extent: " + std::to_string(w) + "x" + std::to_string(h));
      }
      if (width && (w != width || h != height)) {
        throw std::runtime_error("Framebuffer extent changed from " + std::to_string(width) + "x" + std::to_string(height) + " to " + std::to_string(w) + "x" + std::to_string(h) + "; reconnect required to update input mapping");
      }
      if (!live && width && index != initial_index) {
        throw std::runtime_error("Diagnostic framebuffer VkFormat changed; stop and rerun");
      }
      if (images[index]) {
        return *images[index];
      }
      auto candidate = std::make_unique<snapshot_t>(device, api);
      VkImageCreateInfo info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = format;
      info.extent = {w, h, 1};
      info.mipLevels = info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      VkImage image = VK_NULL_HANDLE;
      import_checked(api.create_image(device, &info, nullptr, &image), "create owned snapshot");
      candidate->image = image;
      VkMemoryRequirements requirements {};
      api.image_requirements(device, image, &requirements);
      VkPhysicalDeviceMemoryProperties properties {};
      api.memory_properties(physical, &properties);
      uint32_t type = import_memory_type(requirements.memoryTypeBits, requirements.memoryTypeBits, properties);
      if (!(properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        throw std::runtime_error("Required device-local snapshot memory unavailable");
      }
      VkMemoryAllocateInfo allocate {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocate.allocationSize = requirements.size;
      allocate.memoryTypeIndex = type;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      import_checked(api.allocate_memory(device, &allocate, nullptr, &memory), "allocate snapshot memory");
      candidate->memory = memory;
      import_checked(api.bind_image_memory(device, image, memory, 0), "bind snapshot memory");
      if (!width) {
        width = w;
        height = h;
        initial_index = index;
      }
      images[index] = std::move(candidate);
      return *images[index];
    }

  private:
    VkPhysicalDevice physical;
    VkDevice device;
    snapshot_api_t api;
    uint32_t width = 0, height = 0;
    size_t initial_index = 0;
    std::array<std::unique_ptr<snapshot_t>, 4> images;
  };
}  // namespace pyrowave_diag
