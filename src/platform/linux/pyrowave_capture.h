/**
 * @file src/platform/linux/pyrowave_capture.h
 * @brief Private capture interface for opted-in Pyrowave sources and the KMS diagnostic.
 */
#pragma once

#include "graphics.h"

#include <chrono>
#include <memory>
#include <string>
#include <sys/types.h>

namespace platf {
  struct kms_diagnostic_info_t {
    dev_t primary_device {};
    dev_t render_device {};
    bool has_pci = false;
    uint32_t pci_domain {}, pci_bus {}, pci_device {}, pci_function {};
  };

  class kms_diagnostic_source_t {
  public:
    virtual ~kms_diagnostic_source_t() = default;
    virtual std::shared_ptr<egl::img_descriptor_t> next() = 0;

    // next() with the wait for capture readiness bounded by the caller's
    // remaining budget. Sources that do not wait on a compositor ignore it.
    virtual std::shared_ptr<egl::img_descriptor_t> next_within(std::chrono::milliseconds) {
      return next();
    }

    virtual kms_diagnostic_info_t info() const = 0;
    virtual platf::touch_port_t viewport() const = 0;
    virtual std::pair<int, int> desktop_size() const = 0;

    virtual bool compositor_owned() const {
      return false;
    }

    virtual void snapshot_complete() {}

    virtual void encode_complete() {}

    virtual std::string connector() const {
      return {};
    }
  };

  // Bypasses conventional encoder validation and RAM fallback. No mode setting.
  std::unique_ptr<kms_diagnostic_source_t> make_kms_diagnostic_source(const std::string &display_name, bool live = false);
}  // namespace platf
