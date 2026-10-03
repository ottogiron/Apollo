#include "session_display.h"

#ifndef __linux__
namespace session_display {
  CommandResult run_command(const std::string &, const Snapshot &, std::chrono::milliseconds) {
    return {false, true, "Session display policy is supported only on Linux"};
  }
}  // namespace session_display
#endif
