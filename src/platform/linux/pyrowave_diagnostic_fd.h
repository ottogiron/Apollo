/** @brief Exactly-once ownership of FDs used by the native Vulkan import path. */
#pragma once
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace pyrowave_diag {
  class owned_fd_t {
  public:
    owned_fd_t(const owned_fd_t &) = delete;
    owned_fd_t &operator=(const owned_fd_t &) = delete;

    explicit owned_fd_t(int fd, int (*close_call)(int) = ::close):
        fd(fd),
        close_call(close_call) {}

    ~owned_fd_t() {
      if (fd >= 0) {
        close_call(fd);
      }
    }

    int fd = -1;

    void consumed() {
      fd = -1;
    }

  private:
    int (*close_call)(int);
  };

  class import_fd_t: public owned_fd_t {
  public:
    explicit import_fd_t(int source):
        owned_fd_t(fcntl(source, F_DUPFD_CLOEXEC, 0)) {
      if (fd < 0) {
        throw std::runtime_error("Cannot duplicate import FD");
      }
    }
  };
}  // namespace pyrowave_diag
