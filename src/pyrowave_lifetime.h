/** @brief Capture exclusion and bounded packet ownership for live sessions. */
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace pyrowave {
  class CaptureGate {
  public:
    std::shared_ptr<void> acquire(bool exclusive) {
      std::lock_guard lock(mutex);
      if (!failure.empty() || pyrowave_active || (exclusive && active)) {
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

    void poison(std::string reason) {
      std::lock_guard lock(mutex);
      failure = std::move(reason);
    }

    std::string error() {
      std::lock_guard lock(mutex);
      return failure;
    }

  private:
    std::mutex mutex;
    size_t active = 0;
    bool pyrowave_active = false;
    std::string failure;
  };

  inline CaptureGate &capture_gate() {
    static CaptureGate gate;
    return gate;
  }

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
