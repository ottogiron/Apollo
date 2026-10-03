/** @brief Owned Vulkan capture/encode resources shared by the opt-in session and diagnostic. */
#pragma once

#include "pyrowave_diagnostic_import.h"
#include "pyrowave_diagnostic_support.h"
#include "pyrowave_snapshot.h"

#include <memory>
#include <pyrowave.h>

namespace platf {
  struct kms_diagnostic_info_t;
}

namespace pyrowave_diag {
  void checked(pyrowave_result result, const char *operation);
  void checked_vk(VkResult result, const char *operation);

  class gpu_t {
  public:
    gpu_t() = default;
    gpu_t(const gpu_t &) = delete;
    gpu_t &operator=(const gpu_t &) = delete;
    ~gpu_t();
    // nullptr is synthetic-only selection. Live requires a DRM/PCI identity match.
    void init(const platf::kms_diagnostic_info_t *identity, bool diagnostic = true, int encode_width = output_width, int encode_height = output_height);
    void prepare(uint32_t width, uint32_t height, VkFormat format);
    void import(const layout_t &layout);
    void snapshot(const std::vector<uint8_t> *synthetic = nullptr);
    void readback_snapshot();
    std::vector<uint8_t> reference_pixels() const;
    pyrowave_image_view snapshot_view() const;
    void release_import();

    VkSemaphore completion_semaphore() const {
      return completion;
    }

    void wait_encode(uint64_t value);
    void validate_alpha();  // GPU reduction of owned snapshot; no full image readback.

    pyrowave_device pyro = nullptr;
    pyrowave_encoder encoder = nullptr;
    pyrowave_decoder decoder = nullptr;
    std::shared_ptr<void> capture_lifetime;
    bool same_gpu_checked = false;
    double producer_wait_ms = 0;
    VkMemoryPropertyFlags reference_memory_properties = 0;
    uint32_t import_image_type_bits = 0, import_fd_type_bits = 0, import_type_index = 0;

  private:
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags flags, VkMemoryPropertyFlags preferred = 0) const;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDeviceVulkan11Features features11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features features12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    std::vector<const char *> extensions;
    VkImage owned_image = VK_NULL_HANDLE;
    std::unique_ptr<snapshots_t> snapshots;
    snapshot_t *active_snapshot = nullptr;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore completion = VK_NULL_HANDLE;
    VkSemaphore copy_release = VK_NULL_HANDLE;
    int capture_dma_fd = -1;
    std::unique_ptr<dma_buf_image_t> imported;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool initialized_image = false;
    bool diagnostic = true;
    VkImage alpha_bound_image = VK_NULL_HANDLE;
    VkSampler alpha_sampler = VK_NULL_HANDLE;
    VkBuffer alpha_buffer = VK_NULL_HANDLE;
    VkDeviceMemory alpha_memory = VK_NULL_HANDLE;
    void *alpha_mapped = nullptr;
    VkDescriptorSetLayout alpha_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool alpha_pool = VK_NULL_HANDLE;
    VkDescriptorSet alpha_set = VK_NULL_HANDLE;
    VkPipelineLayout alpha_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline alpha_pipeline = VK_NULL_HANDLE;
    void prepare_alpha();
  };
}  // namespace pyrowave_diag
