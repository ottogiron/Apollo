/** @brief Opt-in video producer; audio, control and transport remain session-owned. */
#pragma once

#include "config.h"
#include "pyrowave_protocol.h"
#include "video.h"

namespace pyrowave {
  inline bool enabled() {
#ifdef APOLLO_ENABLE_PYROWAVE
    return config::video.experimental_pyrowave && config::video.capture == "kms";
#else
    return false;
#endif
  }

  class Session {
  public:
    virtual ~Session() = default;
    virtual void run(safe::mail_t mail, void *channel_data) = 0;
    virtual void drain(void *channel_data) = 0;
  };

#ifdef APOLLO_ENABLE_PYROWAVE
  // Performs a real first import/snapshot/encode before ANNOUNCE returns success.
  std::unique_ptr<Session> make_session(const Limits &limits);
#else
  inline std::unique_ptr<Session> make_session(const Limits &) {
    throw std::runtime_error("Pyrowave was not enabled in this build");
  }
#endif
}  // namespace pyrowave
