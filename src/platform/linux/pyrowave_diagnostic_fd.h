/** @brief Ownership guard for a duplicate handed to the pinned Vulkan importer. */
#pragma once
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace pyrowave_diag {
  class import_fd_t {
  public:
    import_fd_t(const import_fd_t &) = delete;
    import_fd_t &operator=(const import_fd_t &) = delete;

    explicit import_fd_t(int source) {
      fd = fcntl(source, F_DUPFD_CLOEXEC, 0);
      if (fd < 0 || fstat(fd, &identity)) {
        if (fd >= 0) {
          close(fd);
        }
        throw std::runtime_error("Cannot duplicate import FD");
      }
    }

    ~import_fd_t() {
      // The pinned path consumes on successful allocation, but a later failure
      // may also consume. Do not blindly close a slot reused by the library.
      struct stat current {};
      if (fd >= 0 && !fstat(fd, &current) && current.st_dev == identity.st_dev && current.st_ino == identity.st_ino) {
        close(fd);
      }
    }

    int fd = -1;

    void consumed() {
      fd = -1;
    }

  private:
    struct stat identity {};
  };
}  // namespace pyrowave_diag
