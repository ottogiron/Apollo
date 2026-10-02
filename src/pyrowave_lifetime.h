/** @brief Capture exclusion and bounded packet ownership for live sessions. */
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace pyrowave {
  class CaptureGate {
  public:
    std::shared_ptr<void> acquire(bool exclusive) {
      std::lock_guard lock(mutex);
      if (pyrowave_active || (exclusive && active)) {
        return {};
      }
      ++active;
      pyrowave_active = exclusive;
      return std::shared_ptr<void>(this, [this, exclusive](void *) {
        std::lock_guard lock(mutex);
        --active;
        if (exclusive) {
          pyrowave_active = false;
        }
      });
    }

  private:
    std::mutex mutex;
    size_t active = 0;
    bool pyrowave_active = false;
  };

  class FrameWindow: public std::enable_shared_from_this<FrameWindow> {
  public:
    std::shared_ptr<void> acquire() {
      std::lock_guard lock(mutex);
      if (pending == 2 || closed) {
        return {};
      }
      ++pending;
      return std::shared_ptr<void>(this, [self = shared_from_this()](void *) {
        std::lock_guard lock(self->mutex);
        --self->pending;
        self->cv.notify_all();
      });
    }

    void close() {
      std::lock_guard lock(mutex);
      closed = true;
    }

    bool wait_drained(std::chrono::milliseconds timeout) {
      std::unique_lock lock(mutex);
      return cv.wait_for(lock, timeout, [this]() {
        return pending == 0;
      });
    }

  private:
    std::mutex mutex;
    std::condition_variable cv;
    size_t pending = 0;
    bool closed = false;
  };
}  // namespace pyrowave
