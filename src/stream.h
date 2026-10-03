/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "pyrowave_protocol.h"
#include "session_display.h"
#include "video.h"

namespace pyrowave {
  class Session;
}

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
    pyrowave::Limits pyrowave_limits;
  };

  namespace session {
#ifdef SUNSHINE_TESTS
    // CPU integration seam: production start/stop/join own all sequencing,
    // state, threads and accounting; tests replace hardware and transport I/O.
    struct TestHooks {
      session_display::Runner commands;
      std::function<std::unique_ptr<pyrowave::Session>()> capture;
      std::function<void()> transport;
      std::function<std::thread(bool)> thread;
      std::function<void()> before_commit;
    };

    void set_test_hooks(session_t &, std::shared_ptr<TestHooks>);
    unsigned test_running_sessions();
    int test_wait_initial_ping(session_t &, std::chrono::milliseconds);
#endif
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t& session);
    bool uuid_match(const session_t& session, const std::string_view& uuid);
    bool update_device_info(session_t& session, const std::string& name, const crypto::PERM& newPerm);
    int start(session_t &session, const std::string &addr_string, std::string *startup_error = nullptr);
    void stop(session_t &session);
    void graceful_stop(session_t& session);
    void join(session_t &session);
    state_e state(session_t &session);
    inline bool send(session_t& session, const std::string_view &payload);
  }  // namespace session
}  // namespace stream
