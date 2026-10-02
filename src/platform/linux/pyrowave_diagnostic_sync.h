/** @brief Producer reservation-fence wait shared by the live path and CPU mocks. */
#pragma once

#include "pyrowave_diagnostic_fd.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>

namespace pyrowave_diag {
  struct producer_sync_api_t {
    int (*ioctl_call)(int, unsigned long, void *) = [](int fd, unsigned long request, void *arg) {
      return ::ioctl(fd, request, arg);
    };
    int (*poll_call)(pollfd *, nfds_t, int) = ::poll;
    int (*close_call)(int) = ::close;
  };

  inline void wait_producer(int fd, const producer_sync_api_t &api = {}) {
    dma_buf_export_sync_file request {};
    request.flags = DMA_BUF_SYNC_READ;
    request.fd = -1;
    int exported = api.ioctl_call(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request);
    owned_fd_t fence(request.fd, api.close_call);
    if (exported || fence.fd < 0) {
      throw std::runtime_error("DMA-BUF producer fence export unsupported/failed; no implicit-sync guess is allowed");
    }
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    pollfd poll_fd {fence.fd, POLLIN, 0};
    int result;
    do {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
      result = api.poll_call(&poll_fd, 1, int(std::max<int64_t>(0, left)));
    } while (result < 0 && errno == EINTR && std::chrono::steady_clock::now() < end);
    if (result != 1 || (poll_fd.revents & (POLLERR | POLLNVAL | POLLHUP)) || !(poll_fd.revents & POLLIN)) {
      throw std::runtime_error("DMA-BUF producer fence failed or timed out");
    }
    // Linux also reports errored fences as readable. Query while the FD is
    // still owned; only status 1 means successful completion (0 is active).
    sync_file_info info {};
    if (api.ioctl_call(fence.fd, SYNC_IOC_FILE_INFO, &info)) {
      throw std::runtime_error("DMA-BUF producer fence status query failed");
    }
    if (info.status != 1) {
      throw std::runtime_error("DMA-BUF producer fence did not complete successfully: status " + std::to_string(info.status));
    }
    // Current writers have finished. This does NOT lease the compositor buffer
    // or prevent subsequent writes during the non-atomic snapshot bridge.
  }
}  // namespace pyrowave_diag
