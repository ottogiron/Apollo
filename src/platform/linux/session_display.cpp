/** @brief Bounded, synchronous trusted display helper process groups. */
#include "../../session_display.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace session_display {
  namespace {
    std::mutex reaping_mutex;
    unsigned helpers = 0;
  }  // namespace

  // proc::running() historically reaps unclaimed/detached children with
  // waitpid(-1). It must not steal this runner's status or adopted descendants.
  void reap_unclaimed_children() {
    std::lock_guard lock(reaping_mutex);
    if (!helpers) {
      while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    }
  }

  CommandResult run_command(const std::string &command, const Snapshot &snapshot, std::chrono::milliseconds timeout) {
    using namespace std::chrono_literals;
    {
      std::lock_guard lock(reaping_mutex);
      // Adopt and reap orphaned descendants within the helper group. This also
      // lets the existing app reaper collect detached app children afterwards.
      if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
        return {false, true, std::string("subreaper: ") + std::strerror(errno)};
      }
      ++helpers;
    }

    struct Guard {
      ~Guard() {
        std::lock_guard lock(reaping_mutex);
        --helpers;
      }
    } guard;

    std::vector<char *> env;
    for (const auto &entry : snapshot.environment) {
      env.push_back(const_cast<char *>(entry.c_str()));
    }
    env.push_back(nullptr);
    char *args[] = {const_cast<char *>("sh"), const_cast<char *>("-c"), const_cast<char *>(command.c_str()), nullptr};

    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_init(&actions);

    struct SpawnGuard {
      posix_spawnattr_t &attr;
      posix_spawn_file_actions_t &actions;

      ~SpawnGuard() {
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
      }
    } spawn_guard {attr, actions};

    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    posix_spawnattr_setsigmask(&attr, &mask);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    int error = 0;
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
      if (!error) {
        error = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
      }
    }
    if (!error && !snapshot.working_directory.empty()) {
      error = posix_spawn_file_actions_addchdir_np(&actions, snapshot.working_directory.c_str());
    }
    if (!error) {
      error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    }
    pid_t pid = -1;
    if (!error) {
      error = posix_spawn(&pid, "/bin/sh", &actions, &attr, args, env.data());
    }
    if (error) {
      return {false, true, std::string("spawn: ") + std::strerror(error)};
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    siginfo_t status {};
    bool timed_out = false;
    while (true) {
      if (waitid(P_PID, pid, &status, WEXITED | WNOHANG | WNOWAIT) < 0) {
        error = errno;
        break;
      }
      if (status.si_pid == pid) {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        break;
      }
      std::this_thread::sleep_for(5ms);
    }
    // Retain the leader until group signalling is done (WNOWAIT above avoids
    // PID reuse). Even a successful shell may have left background children.
    kill(-pid, SIGTERM);
    std::this_thread::sleep_for(20ms);
    kill(-pid, SIGKILL);
    const auto settle_deadline = std::chrono::steady_clock::now() + 2s;
    bool settled = false;
    while (std::chrono::steady_clock::now() < settle_deadline) {
      while (waitpid(-pid, nullptr, WNOHANG) > 0) {}
      if (kill(-pid, 0) < 0 && errno == ESRCH) {
        settled = true;
        break;
      }
      std::this_thread::sleep_for(5ms);
    }
    if (!settled) {
      return {false, false, "process group did not settle after SIGKILL"};
    }
    if (error) {
      return {false, true, std::string("wait: ") + std::strerror(error)};
    }
    if (timed_out) {
      return {false, true, "command timed out"};
    }
    if (status.si_code != CLD_EXITED || status.si_status != 0) {
      return {false, true, "command exited with status " + std::to_string(status.si_status)};
    }
    return {true, true, {}};
  }
}  // namespace session_display
