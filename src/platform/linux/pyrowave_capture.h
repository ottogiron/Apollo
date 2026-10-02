/**
 * @file src/platform/linux/pyrowave_capture.h
 * @brief Private interface for the separately built KMS diagnostic.
 */
#pragma once

#include "graphics.h"

#include <memory>
#include <string>
#include <sys/types.h>

namespace platf {
  struct kms_diagnostic_info_t {
    dev_t primary_device {};
    bool has_pci = false;
    uint32_t pci_domain {}, pci_bus {}, pci_device {}, pci_function {};
  };

  class kms_diagnostic_source_t {
  public:
    virtual ~kms_diagnostic_source_t() = default;
    virtual std::shared_ptr<egl::img_descriptor_t> next() = 0;
    virtual kms_diagnostic_info_t info() const = 0;
  };

  // Only defined in the diagnostic target; bypasses encoder validation and RAM fallback.
  std::unique_ptr<kms_diagnostic_source_t> make_kms_diagnostic_source(const std::string &display_name);
}  // namespace platf
