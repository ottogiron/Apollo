/** @brief CPU integration tests: production session orchestration, injected I/O. */
#ifdef __linux__
  #include "src/process.h"
  #include "src/pyrowave_session.h"
  #include "src/session_display.h"
  #include "src/stream.h"
  #include "tests/tests_common.h"

  #include <condition_variable>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <sys/wait.h>
  #include <unistd.h>

using namespace std::chrono_literals;

namespace {
  std::atomic_bool allow_cpu_probe {false};
  std::atomic_bool inject_legacy_display_io {false};
  std::atomic_uint cpu_probes {0};
  std::atomic_uint legacy_recoveries {0};
  std::function<void()> legacy_recovery_io;
}  // namespace

extern "C" int __wrap__ZN5video14probe_encodersEv() {
  if (allow_cpu_probe.load()) {
    ++cpu_probes;
    return 0;  // Inject successful conventional probe I/O, without hardware.
  }
  ADD_FAILURE() << "CPU test unexpectedly attempted a live encoder probe";
  return -1;
}

namespace {
  std::atomic_bool forbid_legacy_recovery {false};
}

extern "C" void __real__ZN14display_device20revert_configurationEv();

extern "C" void __wrap__ZN14display_device20revert_configurationEv() {
  ++legacy_recoveries;
  if (forbid_legacy_recovery.load()) {
    ADD_FAILURE() << "Pyrowave app recovery escaped its session display transaction";
    return;
  }
  if (inject_legacy_display_io.load()) {
    if (legacy_recovery_io) {
      legacy_recovery_io();
    }
    return;
  }
  __real__ZN14display_device20revert_configurationEv();
}

namespace {
  struct Latch {
    std::mutex mutex;
    std::condition_variable cv;
    bool signalled = false;

    void open() {
      std::lock_guard lock(mutex);
      signalled = true;
      cv.notify_all();
    }

    bool wait() {
      std::unique_lock lock(mutex);
      return cv.wait_for(lock, 3s, [&]() {
        return signalled;
      });
    }
  };

  struct Trace {
    std::mutex mutex;
    std::vector<std::string> events;

    void add(std::string event) {
      std::lock_guard lock(mutex);
      events.push_back(std::move(event));
    }

    std::vector<std::string> get() {
      std::lock_guard lock(mutex);
      return events;
    }
  };

  nlohmann::json app_json() {
    return {{"name", "Pyrowave test desktop"}, {"uuid", "00000000-0000-0000-0000-000000000042"}, {"exclude-global-prep-cmd", true}, {"exclude-global-state-cmd", true}, {"allow-client-commands", false}, {"terminate-on-pause", false}, {"session-display", {{"codec", "pyrowave"}, {"prepare", "prepare-test"}, {"recover", "recover-test"}, {"timeout-ms", 100}}}};
  }

  std::shared_ptr<session_display::Snapshot> snapshot() {
    auto value = std::make_shared<session_display::Snapshot>();
    value->policy = session_display::parse_policy(app_json());
    value->environment = {"PATH=/usr/bin:/bin", "TEST_SNAPSHOT=original"};
    return value;
  }

  class Capture final: public pyrowave::Session {
  public:
    Capture(Trace &trace, std::function<void()> drain):
        trace(trace),
        drain_packets(std::move(drain)) {}

    ~Capture() override {
      trace.add("destruct");
    }

    void run(safe::mail_t, void *) override {}

    void record_emitted(size_t) override {}

    void drain(void *) override {
      trace.add("drain");
      if (drain_packets) {
        drain_packets();
      }
    }

  private:
    Trace &trace;
    std::function<void()> drain_packets;
  };

  struct Harness {
    Trace trace;
    std::shared_ptr<session_display::Snapshot> policy = snapshot();
    rtsp_stream::launch_session_t launch {};
    stream::config_t config {};
    std::shared_ptr<stream::session_t> session;
    std::shared_ptr<stream::session::TestHooks> hooks = std::make_shared<stream::session::TestHooks>();
    std::function<void()> drain;
    std::function<void(bool)> helper;
    std::string failure;
    int recoveries = 0;

    Harness(bool custom = true) {
      launch.app_session = policy;
      launch.device_name = "CPU test";
      launch.gcm_key.resize(16);
      launch.iv.resize(16);
      config.monitor.videoFormat = custom ? pyrowave::video_format : 0;
      config.monitor.width = 2560;
      config.monitor.height = 1440;
      if (!custom) {
        launch.app_session.reset();
      }
      session = stream::session::alloc(config, launch);
      hooks->commands = [&](const std::string &command, const auto &snap, auto) {
        bool recovery = command == "recover-test";
        trace.add(recovery ? "restore" : "prepare");
        EXPECT_FALSE(pyrowave::capture_gate().acquire(false));
        EXPECT_EQ(snap.environment, policy->environment);
        if (recovery) {
          ++recoveries;
        }
        if (helper) {
          helper(recovery);
        }
        if (!recovery && failure == "prepare") {
          trace.add("helper-settled");
          return session_display::CommandResult {false, true, "partial prepare"};
        }
        return session_display::CommandResult {true, true, {}};
      };
      hooks->capture = [&]() -> std::unique_ptr<pyrowave::Session> {
        trace.add("capture");
        if (failure == "null-factory") {
          return {};
        }
        if (failure == "factory") {
          throw std::runtime_error("factory exception");
        }
        return std::make_unique<Capture>(trace, [&]() {
          if (drain) {
            drain();
          }
        });
      };
      hooks->transport = [&]() {
        trace.add("broadcast");
        if (failure == "broadcast") {
          throw std::runtime_error("broadcast allocation");
        }
      };
      hooks->thread = [&](bool video) {
        if (video && failure == "thread") {
          throw std::runtime_error("partial thread startup");
        }
        return std::thread([&, video]() {
          trace.add(video ? "video-joined" : "audio-joined");
        });
      };
      hooks->before_commit = [&]() {
        if (failure == "commit") {
          throw std::runtime_error("late startup failure");
        }
      };
      stream::session::set_test_hooks(*session, hooks);
    }

    ~Harness() {
      // A failed HTTP assertion must settle sessions borrowing this harness's
      // injected capture/runner before its trace and callbacks are destroyed.
      rtsp_stream::terminate_sessions();
      if (stream::session::state(*session) == stream::session::state_e::RUNNING) {
        stream::session::stop(*session);
        stream::session::join(*session);
      }
    }

    int start(std::string *error = nullptr) {
      return stream::session::start(*session, "127.0.0.1", error);
    }

    void bind_launch(rtsp_stream::launch_session_t &app_launch) {
      policy = std::make_shared<session_display::Snapshot>(*app_launch.app_session);
      session = stream::session::alloc(config, app_launch);
      stream::session::set_test_hooks(*session, hooks);
    }

    void stop() {
      stream::session::stop(*session);
      stream::session::join(*session);
    }
  };

  class SessionDisplay: public testing::Test {
  protected:
    void SetUp() override {
      task_pool.start(1);
    }

    void TearDown() override {
      EXPECT_EQ(stream::session::test_running_sessions(), 0u);
      EXPECT_TRUE(pyrowave::capture_gate().acquire(true));
      task_pool.stop();
      task_pool.join();
    }
  };
}  // namespace

TEST_F(SessionDisplay, PrepareCaptureDrainDestructRestoreReleaseWithLatches) {
  Harness h;
  Latch prepare_entered, prepare_done, drain_entered, packet_done, restore_entered, restore_done;
  h.helper = [&](bool recovery) {
    (recovery ? restore_entered : prepare_entered).open();
    EXPECT_TRUE((recovery ? restore_done : prepare_done).wait());
  };
  h.drain = [&]() {
    drain_entered.open();
    EXPECT_TRUE(packet_done.wait());
    h.trace.add("packet-released");
  };
  auto startup = std::async(std::launch::async, [&]() {
    return h.start();
  });
  EXPECT_TRUE(prepare_entered.wait());
  EXPECT_EQ(h.trace.get(), (std::vector<std::string> {"prepare"}));
  EXPECT_FALSE(pyrowave::capture_gate().acquire(false));
  prepare_done.open();
  ASSERT_EQ(startup.get(), 0);
  auto shutdown = std::async(std::launch::async, [&]() {
    h.stop();
  });
  EXPECT_TRUE(drain_entered.wait());
  EXPECT_FALSE(pyrowave::capture_gate().acquire(true));
  packet_done.open();
  EXPECT_TRUE(restore_entered.wait());
  auto events = h.trace.get();
  EXPECT_LT(std::find(events.begin(), events.end(), "capture"), std::find(events.begin(), events.end(), "drain"));
  EXPECT_LT(std::find(events.begin(), events.end(), "drain"), std::find(events.begin(), events.end(), "packet-released"));
  EXPECT_LT(std::find(events.begin(), events.end(), "packet-released"), std::find(events.begin(), events.end(), "destruct"));
  EXPECT_LT(std::find(events.begin(), events.end(), "destruct"), std::find(events.begin(), events.end(), "restore"));
  EXPECT_FALSE(pyrowave::capture_gate().acquire(false));
  // An immediate RTSP reconnect cannot enter while recovery is running.
  Harness reconnect;
  EXPECT_NE(reconnect.start(), 0);
  EXPECT_TRUE(reconnect.trace.get().empty());
  restore_done.open();
  shutdown.get();
  EXPECT_EQ(h.recoveries, 1);
  EXPECT_EQ(reconnect.start(), 0);
  reconnect.stop();
}

TEST_F(SessionDisplay, EveryFailedStartupRollsBackOnceAndJoinsOnlyStartedThreads) {
  for (const auto &failure : {"prepare", "factory", "null-factory", "broadcast", "thread", "commit"}) {
    Harness h;
    h.failure = failure;
    std::string error;
    EXPECT_NE(h.start(&error), 0) << failure;
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(h.recoveries, 1) << failure;
    EXPECT_EQ(stream::session::test_running_sessions(), 0u);
    EXPECT_EQ(stream::session::state(*h.session), stream::session::state_e::STOPPED);
    stream::session::join(*h.session);  // A failed, uncounted start is safe to join.
    EXPECT_EQ(h.recoveries, 1);
    auto events = h.trace.get();
    if (h.failure == "prepare") {
      EXPECT_EQ(events, (std::vector<std::string> {"prepare", "helper-settled", "restore"}));
    } else if (h.failure != "factory" && h.failure != "null-factory") {
      EXPECT_LT(std::find(events.begin(), events.end(), "destruct"), std::find(events.begin(), events.end(), "restore"));
    }
    EXPECT_TRUE(pyrowave::capture_gate().acquire(true));
  }
}

TEST_F(SessionDisplay, CancelDuringPrepareSettlesBeforeRollbackAndNeverCaptures) {
  Harness h;
  h.helper = [&](bool recovery) {
    if (!recovery) {
      h.policy->valid->store(false);
    }
  };
  EXPECT_NE(h.start(), 0);
  EXPECT_EQ(h.trace.get(), (std::vector<std::string> {"prepare", "restore"}));
  EXPECT_EQ(h.recoveries, 1);
}

TEST_F(SessionDisplay, CodecRejectionAndAbandonedHandshakeHaveNoDisplayEffects) {
  Harness abandoned;
  EXPECT_TRUE(abandoned.trace.get().empty());  // Allocating/abandoning a launch has no transaction.
  Harness incompatible(false);
  incompatible.launch.app_session = incompatible.policy;
  incompatible.session = stream::session::alloc(incompatible.config, incompatible.launch);
  stream::session::set_test_hooks(*incompatible.session, incompatible.hooks);
  std::string error;
  EXPECT_NE(incompatible.start(&error), 0);
  EXPECT_NE(error.find("requires Pyrowave"), std::string::npos);
  EXPECT_TRUE(incompatible.trace.get().empty());
  abandoned.policy->valid->store(false);  // HTTP cancel before ANNOUNCE.
  EXPECT_NE(abandoned.start(), 0);
  EXPECT_TRUE(abandoned.trace.get().empty());
}

TEST_F(SessionDisplay, MissingPingsStopAndOrdinaryAppsKeepSharedCapture) {
  Harness custom;
  ASSERT_EQ(custom.start(), 0);
  // Transport shutdown (including initial/control ping timeout) uses the real
  // stop/join path. There is no dependency on app termination for recovery.
  custom.stop();
  EXPECT_EQ(custom.recoveries, 1);
  Harness ordinary(false);
  ASSERT_EQ(ordinary.start(), 0);
  EXPECT_TRUE(pyrowave::capture_gate().acquire(false));
  EXPECT_FALSE(pyrowave::capture_gate().acquire(true));
  ordinary.stop();
  EXPECT_EQ(ordinary.recoveries, 0);
  auto events = ordinary.trace.get();
  EXPECT_EQ(std::count(events.begin(), events.end(), "capture"), 0);
}

TEST_F(SessionDisplay, LegacyPreparationLeaseSerializesWithAnnounce) {
  auto legacy = pyrowave::capture_gate().acquire(false);
  Harness h;
  EXPECT_NE(h.start(), 0);
  EXPECT_TRUE(h.trace.get().empty());
  legacy.reset();
  EXPECT_EQ(h.start(), 0);
  EXPECT_FALSE(pyrowave::capture_gate().acquire(false));
  h.stop();
}

TEST(SessionDisplayPolicy, MalformedOwnershipIsRejectedAndOrdinaryPolicyIsUnchanged) {
  EXPECT_FALSE(session_display::parse_policy({{"prep-cmd", {{{"do", "ordinary-prep"}}}}}));
  const auto good = app_json();
  ASSERT_TRUE(session_display::parse_policy(good));
  for (auto key : {"exclude-global-prep-cmd", "exclude-global-state-cmd"}) {
    auto app = good;
    app[key] = false;
    EXPECT_THROW(session_display::parse_policy(app), std::exception);
  }
  for (auto key : {"terminate-on-pause", "allow-client-commands", "virtual-display"}) {
    auto app = good;
    app[key] = true;
    EXPECT_THROW(session_display::parse_policy(app), std::exception);
  }
  for (const auto &bad : {nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json {{"codec", "hevc"}}, nlohmann::json {{"codec", "pyrowave"}, {"prepare", ""}, {"recover", "ok"}}}) {
    auto app = good;
    app["session-display"] = bad;
    EXPECT_THROW(session_display::parse_policy(app), std::exception);
  }
  for (const auto &bad : {nlohmann::json(0), nlohmann::json(60001), nlohmann::json("1000"), nlohmann::json(100.5)}) {
    auto app = good;
    app["session-display"]["timeout-ms"] = bad;
    EXPECT_THROW(session_display::parse_policy(app), std::exception);
  }
  for (auto key : {"prep-cmd", "state-cmd"}) {
    auto app = good;
    app[key] = {{{"do", "legacy display change"}}};
    EXPECT_THROW(session_display::parse_policy(app), std::exception);
  }
  auto app = good;
  app["session-display"]["typo"] = true;
  EXPECT_THROW(session_display::parse_policy(app), std::exception);
}

TEST(SessionDisplayPolicy, RecoveryFailureAndUnsettledPrepareBlockFutureDisplayWork) {
  for (bool unsettled : {false, true}) {
    pyrowave::CaptureGate gate;
    auto snap = snapshot();
    int recoveries = 0, destroyed = 0;
    session_display::Lifecycle lifecycle(gate, snap, [&](const auto &cmd, const auto &, auto) {
      if (cmd == "prepare-test") {
        return session_display::CommandResult {!unsettled, !unsettled, "unsettled helper"};
      }
      ++recoveries;
      EXPECT_EQ(destroyed, 1);
      return session_display::CommandResult {false, true, "recovery timeout"};
    });
    std::string error;
    bool start = lifecycle.start(true, []() {
    },
                                 []() {
                                 },
                                 [&]() {
                                   ++destroyed;
                                 },
                                 error);
    if (start) {
      error = lifecycle.finish([&]() {
        ++destroyed;
      });
    }
    EXPECT_EQ(recoveries, unsettled ? 0 : 1);
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(gate.acquire(true));
    EXPECT_FALSE(gate.acquire(false));
    EXPECT_FALSE(gate.error().empty());
    EXPECT_TRUE(lifecycle.finish([]() {
                         })
                  .empty());
  }
}

TEST(SessionDisplayCommands, FailuresTimeoutAndBackgroundDescendantsAreSettledAndReaped) {
  session_display::Snapshot snap;
  snap.environment = {"PATH=/usr/bin:/bin", "TEST_SNAPSHOT=original"};
  auto result = session_display::run_command("test \"$TEST_SNAPSHOT\" = original", snap, 1s);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.settled);
  result = session_display::run_command("exit 7", snap, 1s);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.settled);
  EXPECT_NE(result.error.find("7"), std::string::npos);
  result = session_display::run_command("this-test-executable-does-not-exist", snap, 1s);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.settled);
  result = session_display::run_command("trap '' TERM; sleep 30 & wait", snap, 100ms);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.settled);
  EXPECT_NE(result.error.find("timed out"), std::string::npos);
  // Success also cannot leave a background descendant affecting reconnect.
  result = session_display::run_command("sleep 30 & exit 0", snap, 1s);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.settled);
  errno = 0;
  EXPECT_EQ(waitpid(-1, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  snap.working_directory = "/a-session-display-test-directory-that-does-not-exist";
  result = session_display::run_command("exit 0", snap, 1s);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.settled);
  EXPECT_NE(result.error.find("spawn"), std::string::npos);
}

TEST(SessionDisplayCommands, PartialPrepareTimeoutRollsBackOnceWithoutLateChildren) {
  auto directory = std::filesystem::temp_directory_path() / ("apollo-display-rollback-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  auto cleanup = util::fail_guard([&]() {
    std::filesystem::remove_all(directory);
  });
  auto partial = directory / "partial";
  auto recovered = directory / "recovered";
  auto snap = snapshot();
  snap->policy->prepare = "printf partial > '" + partial.string() + "'; trap '' TERM; sleep 30 & wait";
  snap->policy->recover = "test -f '" + partial.string() + "' && printf r >> '" + recovered.string() + "'";
  pyrowave::CaptureGate gate;
  session_display::Lifecycle lifecycle(gate, snap);
  int captures = 0, settled = 0;
  std::string error;
  auto result = std::async(std::launch::async, [&]() {
    return lifecycle.start(true, [&]() {
      ++captures;
    },
                           []() {
                           },
                           [&]() {
                             ++settled;
                           },
                           error);
  });
  // Exercise proc::running()'s real broad reaper concurrently with helper I/O.
  while (result.wait_for(1ms) != std::future_status::ready) {
    session_display::reap_unclaimed_children();
  }
  EXPECT_FALSE(result.get());
  EXPECT_EQ(captures, 0);
  EXPECT_EQ(settled, 1);
  EXPECT_NE(error.find("timed out"), std::string::npos);
  std::ifstream file(recovered);
  std::string contents;
  file >> contents;
  EXPECT_EQ(contents, "r");
  EXPECT_TRUE(lifecycle.finish([]() {
                       })
                .empty());
  EXPECT_TRUE(gate.acquire(true));
  errno = 0;
  EXPECT_EQ(waitpid(-1, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
}

TEST(SessionDisplayCommands, RecoveryFailureRetainsStateAndBlocksReconnect) {
  auto directory = std::filesystem::temp_directory_path() / ("apollo-display-recovery-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  auto cleanup = util::fail_guard([&]() {
    std::filesystem::remove_all(directory);
  });
  auto lock = directory / "recovery-lock";
  auto snap = snapshot();
  snap->policy->prepare = "printf backup > '" + lock.string() + "'";
  snap->policy->recover = "exit 9";
  pyrowave::CaptureGate gate;
  session_display::Lifecycle lifecycle(gate, snap);
  std::string error;
  ASSERT_TRUE(lifecycle.start(true, []() {
  },
                              []() {
                              },
                              []() {
                              },
                              error));
  EXPECT_NE(lifecycle.finish([]() {
                     })
              .find("9"),
            std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(lock));
  EXPECT_FALSE(gate.acquire(true));
  EXPECT_FALSE(gate.acquire(false));
}

  #ifdef APOLLO_ENABLE_PYROWAVE
TEST_F(SessionDisplay, ResumeSameAppAndDetachedContextRetainSessionBoundOwnership) {
  auto old_config = config::video;
  config::video.capture = "kms";
  config::video.experimental_pyrowave = true;
  proc::ctx_t app {};
  app.session_display = session_display::parse_policy(app_json());
  app.name = "CPU placebo desktop";
  app.id = "42";
  app.scale_factor = 100;
  boost::process::v1::environment env = boost::this_process::environment();
  proc::proc_t context(std::move(env), {});
  auto initial = std::make_shared<rtsp_stream::launch_session_t>();
  initial->width = 2560;
  initial->height = 1440;
  initial->fps = 60000;
  initial->scale_factor = 100;
  ASSERT_EQ(context.execute(app, initial), 0);  // Empty app command: no app is launched or probed.
  auto saved = context.session_snapshot();
  ASSERT_TRUE(saved && saved->policy);
  rtsp_stream::launch_session_t resume {}, same_app {};
  resume.width = 1920;
  resume.height = 1080;
  resume.device_name = "resume client";
  same_app.width = 2560;
  same_app.height = 1440;
  context.attach_session_policy(resume);
  context.attach_session_policy(same_app);
  EXPECT_EQ(resume.app_session->policy->prepare, saved->policy->prepare);
  EXPECT_EQ(same_app.app_session->policy->recover, saved->policy->recover);
  EXPECT_NE(resume.app_session.get(), saved.get());
  EXPECT_NE(resume.app_session->environment, saved->environment);

  Harness h;
  h.policy = std::make_shared<session_display::Snapshot>(*resume.app_session);
  h.launch.app_session = h.policy;
  h.session = stream::session::alloc(h.config, h.launch);
  stream::session::set_test_hooks(*h.session, h.hooks);
  ASSERT_EQ(h.start(), 0);
  EXPECT_EQ(context.running(), 42);  // Detached/placebo context cannot observe Steam's exit.
  h.stop();  // Disconnect restores without terminating the app context.
  EXPECT_EQ(context.running(), 42);
  EXPECT_TRUE(saved->valid->load());
  EXPECT_EQ(h.recoveries, 1);

  Harness reconnect;
  reconnect.policy = std::make_shared<session_display::Snapshot>(*same_app.app_session);
  reconnect.launch.app_session = reconnect.policy;
  reconnect.session = stream::session::alloc(reconnect.config, reconnect.launch);
  stream::session::set_test_hooks(*reconnect.session, reconnect.hooks);
  ASSERT_EQ(reconnect.start(), 0);
  EXPECT_TRUE(context.terminate(false, false));  // Direct terminate invalidates, never restores early.
  EXPECT_FALSE(saved->valid->load());
  EXPECT_EQ(reconnect.recoveries, 0);
  reconnect.stop();  // Tracked exit / cancel / direct terminate all settle the same session owner.
  EXPECT_EQ(reconnect.recoveries, 1);
  config::video = old_config;
}
  #endif
#endif

#if defined(__linux__) && defined(APOLLO_ENABLE_PYROWAVE)
  #include "src/nvhttp.h"
  #include "src/uuid.h"

namespace nvhttp {
  using TestResponse = std::shared_ptr<SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using TestRequest = std::shared_ptr<SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  void launch(bool &, TestResponse, TestRequest);
  void resume(bool &, TestResponse, TestRequest);
  void cancel(TestResponse, TestRequest);
}  // namespace nvhttp

namespace {
  // Construct the library's real request/response objects and call the production
  // handlers synchronously. No HTTP listener, TLS handshake or browser is used.
  class HandlerServer: public SimpleWeb::ServerBase<nvhttp::SunshineHTTPS> {
  public:
    HandlerServer():
        ServerBase(0) {}

    std::string request(const std::string &path, const std::string &query) {
      boost::asio::io_context io;
      boost::asio::ssl::context ssl(boost::asio::ssl::context::tls);
      auto connection = create_connection(io, ssl);
      auto session = std::make_shared<Session>(8192, connection);
      auto cert = std::make_shared<crypto::named_cert_t>();
      cert->name = "CPU test client";
      cert->uuid = "00000000-0000-0000-0000-000000000043";
      cert->perm = crypto::PERM::_all;
      session->request->userp = cert;
      session->request->path = path;
      session->request->query_string = query;
      bool host_audio = false;
      std::string result;
      std::function<void(nvhttp::TestResponse, nvhttp::TestRequest)> handler = [&](auto response, auto req) {
        if (path == "/launch") {
          nvhttp::launch(host_audio, response, req);
        } else if (path == "/resume") {
          nvhttp::resume(host_audio, response, req);
        } else {
          nvhttp::cancel(response, req);
        }
        std::ostringstream data;
        data << response->rdbuf();
        result = data.str();
      };
      write(session, handler);
      return result;
    }

  private:
    void accept() override {}
  };

  std::string announce_payload(int format = 3) {
    std::string result = "v=0\r\n";
    auto attribute = [&](const auto &key, const auto &value) {
      result += "a=" + std::string(key) + ":" + std::string(value) + "\r\n";
    };
    for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>> {
           {"x-nv-audio.surround.numChannels", "2"},
           {"x-nv-audio.surround.channelMask", "3"},
           {"x-nv-audio.surround.AudioQuality", "1"},
           {"x-nv-video[0].packetSize", "1392"},
           {"x-nv-video[0].clientViewportHt", "1440"},
           {"x-nv-video[0].clientViewportWd", "2560"},
           {"x-nv-video[0].maxFPS", "60"},
           {"x-nv-vqos[0].bw.maximumBitrateKbps", "400000"},
           {"x-nv-video[0].videoEncoderSlicesPerFrame", "1"},
           {"x-nv-video[0].maxNumReferenceFrames", "1"},
           {"x-nv-video[0].encoderCscMode", "3"},
           {"x-ml-video.configuredBitrateKbps", "400000"}
         }) {
      attribute(key, value);
    }
    attribute("x-nv-vqos[0].bitStreamFormat", std::to_string(format));
    if (format == 3) {
      attribute("x-apollo-pyrowave-version", std::to_string(pyrowave::version));
      attribute("x-apollo-pyrowave-pin", pyrowave::pin);
    }
    return result;
  }

  class SessionDisplayHttp: public SessionDisplay {
  protected:
    config::video_t saved_video = config::video;
    config::stream_t saved_stream = config::stream;
    std::optional<proc::proc_t> saved_proc;
    std::filesystem::path directory;
    HandlerServer http;
    const std::string query = "rikey=00000000000000000000000000000000&rikeyid=1&mode=2560x1440x60&localAudioPlayMode=0&appuuid=00000000-0000-0000-0000-000000000042";

    void SetUp() override {
      SessionDisplay::SetUp();
      config::video.capture = "kms";
      config::video.experimental_pyrowave = true;
      config::stream.lan_encryption_mode = config::ENCRYPTION_MODE_NEVER;
      config::stream.wan_encryption_mode = config::ENCRYPTION_MODE_NEVER;
      config::stream.ping_timeout = 30ms;
      directory = std::filesystem::temp_directory_path() / ("apollo-session-display-" + std::to_string(getpid()));
      std::filesystem::create_directories(directory);
      config::stream.file_apps = (directory / "apps.json").string();
      std::ofstream(config::stream.file_apps) << nlohmann::json {{"version", 2}, {"apps", nlohmann::json::array({app_json()})}}.dump();
      saved_proc.emplace(std::move(proc::proc));
      auto parsed = proc::parse(config::stream.file_apps);
      ASSERT_TRUE(parsed);
      proc::proc = std::move(*parsed);
      forbid_legacy_recovery.store(true);
    }

    void TearDown() override {
      rtsp_stream::terminate_sessions();
      if (auto launch = rtsp_stream::test_pending_launch()) {
        rtsp_stream::launch_session_clear(launch->id);
      }
      proc::proc.terminate(false, false);
      forbid_legacy_recovery.store(false);
      proc::proc = std::move(*saved_proc);
      saved_proc.reset();
      config::video = saved_video;
      config::stream = saved_stream;
      std::filesystem::remove_all(directory);
      SessionDisplay::TearDown();
    }
  };

  class SessionDisplayAppContext: public SessionDisplayHttp {
  protected:
    void SetUp() override {
      SessionDisplayHttp::SetUp();
      forbid_legacy_recovery.store(false);
      allow_cpu_probe.store(true);
      inject_legacy_display_io.store(true);
      config::video.dd.config_revert_on_disconnect = false;
      cpu_probes.store(0);
      legacy_recoveries.store(0);
    }

    void TearDown() override {
      legacy_recovery_io = {};
      SessionDisplayHttp::TearDown();
      allow_cpu_probe.store(false);
      inject_legacy_display_io.store(false);
    }

    static nlohmann::json ordinary_app(bool terminate_on_pause) {
      auto app = app_json();
      app.erase("session-display");
      app["name"] = "Conventional CPU app";
      app["terminate-on-pause"] = terminate_on_pause;
      return app;
    }

    void save_apps(const nlohmann::json &apps) {
      std::ofstream(config::stream.file_apps) << nlohmann::json {{"version", 2}, {"apps", apps}}.dump();
    }

    void configure_apps(const nlohmann::json &apps) {
      save_apps(apps);
      proc::refresh(config::stream.file_apps);
    }
  };
}  // namespace

TEST_F(SessionDisplayHttp, LaunchResumeSameAppRejectAndAbandonUseRealHandlers) {
  EXPECT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto initial = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(initial && initial->app_session && initial->app_session->policy);
  Harness h;
  h.policy = std::make_shared<session_display::Snapshot>(*initial->app_session);
  auto rejection = rtsp_stream::test_announce(*initial, announce_payload(1), h.hooks);
  EXPECT_NE(rejection.find("400"), std::string::npos);
  EXPECT_NE(rejection.find("requires Pyrowave"), std::string::npos);
  EXPECT_TRUE(h.trace.get().empty());
  EXPECT_FALSE(rtsp_stream::test_pending_launch());
  EXPECT_NE(http.request("/resume", query).find("status_code=\"200\""), std::string::npos);
  ASSERT_TRUE(rtsp_stream::test_pending_launch());
  rtsp_stream::test_expire_launch();  // Real launch-event timeout, no ANNOUNCE.
  EXPECT_FALSE(rtsp_stream::test_pending_launch());
  EXPECT_TRUE(h.trace.get().empty());
  EXPECT_GT(proc::proc.running(), 0);

  EXPECT_NE(http.request("/resume", query).find("status_code=\"200\""), std::string::npos);
  auto resume = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(resume && resume->app_session);
  EXPECT_EQ(resume->app_session->policy->prepare, initial->app_session->policy->prepare);
  rtsp_stream::launch_session_clear(resume->id);
  EXPECT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto same = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(same && same->app_session);
  h.policy = std::make_shared<session_display::Snapshot>(*same->app_session);
  auto accepted = rtsp_stream::test_announce(*same, announce_payload(), h.hooks);
  EXPECT_NE(accepted.find("200"), std::string::npos) << accepted;
  EXPECT_EQ(stream::session::test_running_sessions(), 1u);
  rtsp_stream::terminate_sessions();  // Disconnect without cancel keeps the placebo app.
  EXPECT_GT(proc::proc.running(), 0);
  EXPECT_TRUE(same->app_session->valid->load());
  EXPECT_EQ(h.recoveries, 1);
}

TEST_F(SessionDisplayHttp, FailedCaptureCanResumeImmediatelyWithFreshLaunchSnapshot) {
  ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto launch = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(launch);
  Harness h;
  h.policy = std::make_shared<session_display::Snapshot>(*launch->app_session);
  h.failure = "broadcast";
  auto rejected = rtsp_stream::test_announce(*launch, announce_payload(), h.hooks);
  EXPECT_NE(rejected.find("500"), std::string::npos);
  EXPECT_EQ(h.recoveries, 1);
  EXPECT_FALSE(rtsp_stream::test_pending_launch());
  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_GT(proc::proc.running(), 0);
  EXPECT_NE(http.request("/resume", query).find("status_code=\"200\""), std::string::npos);
  auto reconnect = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(reconnect);
  EXPECT_NE(reconnect->id, launch->id);
  h.policy = std::make_shared<session_display::Snapshot>(*reconnect->app_session);
  h.failure.clear();
  EXPECT_NE(rtsp_stream::test_announce(*reconnect, announce_payload(), h.hooks).find("200"), std::string::npos);
  rtsp_stream::terminate_sessions();
  EXPECT_EQ(h.recoveries, 2);
}

TEST_F(SessionDisplayHttp, MissingInitialPingsAndCancelRestoreAfterDrain) {
  ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto launch = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(launch);
  Harness h;
  h.policy = std::make_shared<session_display::Snapshot>(*launch->app_session);
  Latch ping_finished;
  h.hooks->thread = [&](bool video) {
    return std::thread([&, video]() {
      if (video) {
        auto active = rtsp_stream::find_session(launch->unique_id);
        // find_session waits for the ANNOUNCE's atomic startup/slot commit.
        EXPECT_TRUE(active);
        if (active) {
          EXPECT_EQ(stream::session::test_wait_initial_ping(*active, 30ms), -1);
          stream::session::stop(*active);
        }
        ping_finished.open();
      }
    });
  };
  auto accepted = rtsp_stream::test_announce(*launch, announce_payload(), h.hooks);
  ASSERT_NE(accepted.find("200"), std::string::npos) << accepted;
  EXPECT_TRUE(ping_finished.wait());
  EXPECT_EQ(rtsp_stream::session_count(), 0);  // Real stopped-session collection invokes join.
  EXPECT_EQ(h.recoveries, 1);
  EXPECT_GT(proc::proc.running(), 0);
  EXPECT_FALSE(rtsp_stream::test_pending_launch());
  EXPECT_NE(http.request("/resume", query).find("status_code=\"200\""), std::string::npos);
  auto reconnect = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(reconnect);
  EXPECT_NE(reconnect->id, launch->id);
  EXPECT_NE(http.request("/cancel", "").find("status_code=\"200\""), std::string::npos);
  EXPECT_EQ(proc::proc.running(), 0);
  EXPECT_FALSE(launch->app_session->valid->load());
  EXPECT_EQ(h.recoveries, 1);
}

TEST_F(SessionDisplayHttp, CancelCannotMissAnnounceStartupAndDirectTerminateDoesNotRestoreEarly) {
  ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto launch = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(launch);
  Harness h;
  h.policy = std::make_shared<session_display::Snapshot>(*launch->app_session);
  Latch preparing, continue_prepare;
  h.helper = [&](bool recovery) {
    if (!recovery) {
      preparing.open();
      EXPECT_TRUE(continue_prepare.wait());
    }
  };
  auto announce = std::async(std::launch::async, [&]() {
    return rtsp_stream::test_announce(*launch, announce_payload(), h.hooks);
  });
  EXPECT_TRUE(preparing.wait());
  proc::ctx_t legacy {};
  legacy.id = "45";
  legacy.name = "Legacy CPU test app";
  legacy.prep_cmds = {{"false", "false", false}};
  EXPECT_EQ(proc::proc.execute(legacy, std::make_shared<rtsp_stream::launch_session_t>()), 409);
  EXPECT_TRUE(launch->app_session->valid->load());
  auto cancel = std::async(std::launch::async, [&]() {
    return http.request("/cancel", "");
  });
  EXPECT_EQ(cancel.wait_for(20ms), std::future_status::timeout);
  continue_prepare.open();
  EXPECT_NE(announce.get().find("200"), std::string::npos);
  EXPECT_NE(cancel.get().find("status_code=\"200\""), std::string::npos);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_EQ(h.recoveries, 1);
  EXPECT_FALSE(launch->app_session->valid->load());

  ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto next = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(next);
  h.helper = {};
  h.policy = std::make_shared<session_display::Snapshot>(*next->app_session);
  ASSERT_NE(rtsp_stream::test_announce(*next, announce_payload(), h.hooks).find("200"), std::string::npos);
  auto terminate_query = query.substr(0, query.find("&appuuid=")) + "&appuuid=" TERMINATE_APP_UUID;
  EXPECT_NE(http.request("/launch", terminate_query).find("status_code=\"410\""), std::string::npos);
  EXPECT_EQ(h.recoveries, 1);  // Direct app termination has no display ownership.
  EXPECT_FALSE(next->app_session->valid->load());
  rtsp_stream::terminate_sessions();
  EXPECT_EQ(h.recoveries, 2);
}

TEST_F(SessionDisplayHttp, TrackedAndDetachedTestProcessExitsNeverOwnDisplayRecovery) {
  for (bool detached : {false, true}) {
    ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
    auto initial = rtsp_stream::test_pending_launch();
    ASSERT_TRUE(initial);
    auto app = proc::proc.get_apps().front();
    ASSERT_TRUE(app.session_display);
    proc::proc.terminate(false, false);
    rtsp_stream::launch_session_clear(initial->id);
    app.auto_detach = false;
    app.wait_all = false;
    app.exit_timeout = 0s;
    if (detached) {
      app.detached = {"cpu-test-detached-process"};
    } else {
      app.cmd = "cpu-test-tracked-process";
    }
    int control[2];
    ASSERT_EQ(pipe(control), 0);
    pid_t pid = -1;
    proc::proc.test_app_runner = [&](const std::string &) {
      pid = fork();
      if (pid == 0) {
        // No exec/application launch. The injected process only waits on its
        // owned pipe and exits; all post-fork calls are async-signal-safe.
        close(control[1]);
        char byte;
        while (read(control[0], &byte, 1) < 0 && errno == EINTR) {}
        close(control[0]);
        _exit(0);
      }
      if (pid < 0) {
        throw std::runtime_error("CPU process fixture fork failed");
      }
      close(control[0]);
      return boost::process::v1::child(static_cast<boost::process::v1::pid_t>(pid));
    };
    ASSERT_EQ(proc::proc.execute(app, initial), 0);
    auto cleanup = util::fail_guard([&]() {
      if (control[1] >= 0) {
        close(control[1]);
      }
      rtsp_stream::terminate_sessions();
      proc::proc.terminate(false, false);
      // The production running()/terminate path normally owns this wait. On
      // assertion failure, settle the exact fixture PID before teardown.
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    });
    Harness h;
    h.policy = std::make_shared<session_display::Snapshot>(*initial->app_session);
    ASSERT_NE(rtsp_stream::test_announce(*initial, announce_payload(), h.hooks).find("200"), std::string::npos);
    close(control[1]);
    control[1] = -1;
    siginfo_t exit_status {};
    ASSERT_EQ(waitid(P_PID, pid, &exit_status, WEXITED | WNOWAIT), 0);
    EXPECT_EQ(proc::proc.running() > 0, detached);
    EXPECT_EQ(initial->app_session->valid->load(), detached);
    EXPECT_EQ(h.recoveries, 0);
    rtsp_stream::terminate_sessions();
    EXPECT_EQ(h.recoveries, 1);
    proc::proc.terminate(false, false);
  }
}

TEST_F(SessionDisplayAppContext, ConventionalAnnounceDisconnectRunsProductionPausePolicy) {
  for (bool terminate_on_pause : {false, true}) {
    configure_apps(nlohmann::json::array({ordinary_app(terminate_on_pause)}));
    ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
    auto launch = rtsp_stream::test_pending_launch();
    ASSERT_TRUE(launch && launch->app_session);
    auto original_generation = launch->app_session->valid;
    Harness h(false);
    h.bind_launch(*launch);
    ASSERT_NE(rtsp_stream::test_announce(*launch, announce_payload(0), h.hooks).find("200"), std::string::npos);
    rtsp_stream::terminate_sessions();
    EXPECT_EQ(proc::proc.running() > 0, !terminate_on_pause);
    EXPECT_EQ(original_generation->load(), !terminate_on_pause);
    EXPECT_EQ(h.recoveries, 0);
  }
  EXPECT_GT(cpu_probes.load(), 0u);
  EXPECT_GT(legacy_recoveries.load(), 0u);
}

TEST_F(SessionDisplayAppContext, OldConventionalJoinNeverPausesReplacementAppGeneration) {
  configure_apps(nlohmann::json::array({ordinary_app(false)}));
  ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto launch = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(launch && launch->app_session);
  Harness old(false);
  old.bind_launch(*launch);
  ASSERT_EQ(old.start(), 0);
  Latch settled, finish_callbacks;
  old.hooks->after_recovery = [&]() {
    settled.open();
    EXPECT_TRUE(finish_callbacks.wait());
  };
  auto joining = std::async(std::launch::async, [&]() {
    old.stop();
  });
  auto release = util::fail_guard([&]() {
    finish_callbacks.open();
  });
  EXPECT_TRUE(settled.wait());
  auto replacement = ordinary_app(true);
  // Keep the same name/UUID/id: only the generation and pause policy change.
  save_apps(nlohmann::json::array({replacement}));
  EXPECT_NE(http.request("/cancel", "").find("status_code=\"200\""), std::string::npos);
  EXPECT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto replacement_snapshot = proc::proc.session_snapshot();
  EXPECT_TRUE(replacement_snapshot && replacement_snapshot->valid != launch->app_session->valid);
  const auto before_old_callbacks = legacy_recoveries.load();
  finish_callbacks.open();
  joining.get();  // Real join executes its normal post-settlement callbacks.
  EXPECT_GT(proc::proc.running(), 0);
  EXPECT_EQ(proc::proc.get_last_run_app_name(), "Conventional CPU app");
  EXPECT_TRUE(replacement_snapshot && replacement_snapshot->valid->load());
  EXPECT_EQ(legacy_recoveries.load(), before_old_callbacks);
}

TEST_F(SessionDisplayAppContext, PyrowaveGateRemainsHeldThroughProductionCompletionCallbacks) {
  // Use a distinct UUID for the conventional entry to select it explicitly.
  auto conventional = ordinary_app(true);
  conventional["uuid"] = "00000000-0000-0000-0000-000000000043";
  configure_apps(nlohmann::json::array({app_json(), conventional}));
  ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto launch = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(launch && launch->app_session);
  Harness old;
  old.bind_launch(*launch);
  ASSERT_EQ(old.start(), 0);
  Latch recovered, finish_callbacks;
  old.hooks->after_recovery = [&]() {
    recovered.open();
    EXPECT_TRUE(finish_callbacks.wait());
  };
  auto joining = std::async(std::launch::async, [&]() {
    old.stop();
  });
  auto release = util::fail_guard([&]() {
    finish_callbacks.open();
  });
  EXPECT_TRUE(recovered.wait());
  EXPECT_FALSE(pyrowave::capture_gate().acquire(false));
  EXPECT_NE(http.request("/cancel", "").find("status_code=\"200\""), std::string::npos);
  const auto replacement_query = query.substr(0, query.find("&appuuid=")) + "&appuuid=00000000-0000-0000-0000-000000000043";
  EXPECT_NE(http.request("/launch", replacement_query).find("status_code=\"409\""), std::string::npos);
  EXPECT_EQ(old.recoveries, 1);
  finish_callbacks.open();
  joining.get();
  EXPECT_NE(http.request("/launch", replacement_query).find("status_code=\"200\""), std::string::npos);
  EXPECT_GT(proc::proc.running(), 0);
  EXPECT_EQ(old.recoveries, 1);
}

TEST_F(SessionDisplayAppContext, PauseAndHttpReplacementAreSerializedThroughActualAppCleanup) {
  configure_apps(nlohmann::json::array({ordinary_app(true)}));
  ASSERT_NE(http.request("/launch", query).find("status_code=\"200\""), std::string::npos);
  auto launch = rtsp_stream::test_pending_launch();
  ASSERT_TRUE(launch && launch->app_session);
  Harness old(false);
  old.bind_launch(*launch);
  ASSERT_EQ(old.start(), 0);
  auto replacement = ordinary_app(true);
  replacement["name"] = "Next CPU app";
  save_apps(nlohmann::json::array({replacement}));
  Latch pausing, finish_pause;
  legacy_recovery_io = [&]() {
    pausing.open();
    EXPECT_TRUE(finish_pause.wait());
  };
  auto joining = std::async(std::launch::async, [&]() {
    old.stop();
  });
  auto release = util::fail_guard([&]() {
    finish_pause.open();
  });
  EXPECT_TRUE(pausing.wait());  // Production running()->pause()->terminate().
  EXPECT_FALSE(pyrowave::capture_gate().acquire(true));
  auto launching = std::async(std::launch::async, [&]() {
    return http.request("/launch", query);
  });
  EXPECT_EQ(launching.wait_for(20ms), std::future_status::timeout);
  finish_pause.open();
  joining.get();
  EXPECT_NE(launching.get().find("status_code=\"200\""), std::string::npos);
  legacy_recovery_io = {};
  EXPECT_EQ(proc::proc.get_last_run_app_name(), "Next CPU app");
  EXPECT_GT(proc::proc.running(), 0);
  EXPECT_FALSE(launch->app_session->valid->load());
}

TEST_F(SessionDisplayAppContext, BorrowedAppLaunchFailureRefreshesListUsingSavedPolicy) {
  auto selected = app_json();
  selected["cmd"] = "cpu-test-injected-failure";
  configure_apps(nlohmann::json::array({selected}));
  const auto before_failure = legacy_recoveries.load();
  proc::proc.test_app_runner = [&](const std::string &) -> boost::process::v1::child {
    auto refreshed = ordinary_app(false);
    refreshed["name"] = "Refreshed CPU entry";
    save_apps(nlohmann::json::array({refreshed}));
    throw std::runtime_error("Injected CPU launch failure");
  };
  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  {
    auto context = proc::lock_context();
    const auto &borrowed_app = proc::proc.get_apps().front();
    EXPECT_THROW(proc::proc.execute(borrowed_app, launch), std::runtime_error);
    // The borrowed entry is invalid after failure cleanup; do not read it.
  }
  EXPECT_EQ(proc::proc.running(), 0);
  ASSERT_FALSE(proc::proc.get_apps().empty());
  EXPECT_EQ(proc::proc.get_apps().front().name, "Refreshed CPU entry");
  EXPECT_FALSE(proc::proc.get_apps().front().session_display);
  EXPECT_TRUE(launch->app_session && !launch->app_session->valid->load());
  EXPECT_EQ(legacy_recoveries.load(), before_failure);
  EXPECT_TRUE(pyrowave::capture_gate().acquire(true));
}
#endif
