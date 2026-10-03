/** @brief Pyrowave-only, compositor-mediated DMA-BUF screencopy; never conventional capture. */
#pragma once
#include "pyrowave_capture.h"
#include "pyrowave_wayland_allocation.h"

namespace platf {
  std::unique_ptr<kms_diagnostic_source_t> make_pyrowave_wayland_source(const std::string &output_name, const std::string &app_name, pyrowave_wl::validator_t validate);
}
