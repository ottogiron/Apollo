/** @brief Immutable app policy and session-owned display orchestration. */
#pragma once

#include "pyrowave_lifetime.h"

#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace session_display {
  struct Policy {
    std::string prepare;
    std::string recover;
    std::chrono::milliseconds timeout {30000};
  };

  // Validate the entire ownership combination before launching any command.
  inline std::optional<Policy> parse_policy(const nlohmann::json &app) {
    if (!app.contains("session-display")) {
      return {};
    }
    const auto &value = app.at("session-display");
    if (!value.is_object()) {
      throw std::invalid_argument("session-display must be an object");
    }
    for (const auto &item : value.items()) {
      if (item.key() != "codec" && item.key() != "prepare" && item.key() != "recover" && item.key() != "timeout-ms") {
        throw std::invalid_argument("Unknown session-display field: " + item.key());
      }
    }
    if (value.value("codec", "") != "pyrowave") {
      throw std::invalid_argument("session-display requires codec=pyrowave");
    }
    Policy policy {value.at("prepare").get<std::string>(), value.at("recover").get<std::string>()};
    for (const auto *command : {&policy.prepare, &policy.recover}) {
      if (command->find_first_not_of(" \t\r\n") == std::string::npos || command->find('\0') != std::string::npos) {
        throw std::invalid_argument("session-display commands must be nonempty and contain no NUL");
      }
    }
    if (value.contains("timeout-ms")) {
      if (!value.at("timeout-ms").is_number_integer()) {
        throw std::invalid_argument("session-display timeout-ms must be an integer");
      }
      auto timeout = value.at("timeout-ms").get<int64_t>();
      if (timeout < 100 || timeout > 60000) {
        throw std::invalid_argument("session-display timeout-ms must be 100..60000");
      }
      policy.timeout = std::chrono::milliseconds(timeout);
    }
    if (!app.value("exclude-global-prep-cmd", false) || !app.value("exclude-global-state-cmd", false) || app.value("terminate-on-pause", false) || app.value("allow-client-commands", true) || app.value("virtual-display", false)) {
      throw std::invalid_argument("session-display requires excluded global prep/state, terminate-on-pause=false, allow-client-commands=false and virtual-display=false");
    }
    for (auto key : {"prep-cmd", "state-cmd"}) {
      if (app.contains(key) && (!app.at(key).is_array() || !app.at(key).empty())) {
        throw std::invalid_argument("session-display cannot coexist with app prep/state commands");
      }
    }
    return policy;
  }

  struct Snapshot {
    std::optional<Policy> policy;
    std::vector<std::string> environment;
    std::string working_directory;
    std::shared_ptr<std::atomic_bool> valid = std::make_shared<std::atomic_bool>(true);
  };

  struct CommandResult {
    bool success;
    bool settled;
    std::string error;
  };

  using Runner = std::function<CommandResult(const std::string &, const Snapshot &, std::chrono::milliseconds)>;

  // Linux owns process groups; other platforms reject this Linux-only policy.
  CommandResult run_command(const std::string &, const Snapshot &, std::chrono::milliseconds);
  void reap_unclaimed_children();

  class Lifecycle {
  public:
    Lifecycle(pyrowave::CaptureGate &gate, std::shared_ptr<const Snapshot> snapshot, Runner runner = run_command):
        gate(gate),
        snapshot(std::move(snapshot)),
        runner(std::move(runner)) {}

    // Both production startup and CPU tests use this orchestration. Settle must
    // stop/join partial transport, drain packets and destroy capture resources.
    bool start(bool custom, const std::function<void()> &capture, const std::function<void()> &transport, const std::function<void()> &settle, std::string &error) {
      if (snapshot && !snapshot->valid->load()) {
        error = "Application launch was cancelled or exited";
        return false;
      }
      if (snapshot && snapshot->policy && !custom) {
        error = "This application requires Pyrowave; select the matching Pyrowave codec";
        return false;
      }
      ownership = gate.acquire(custom);
      if (!ownership) {
        error = gate.error();
        if (error.empty()) {
          error = "Pyrowave requires exclusive capture; capture or app display commands are active";
        }
        return false;
      }
      try {
        if (snapshot && snapshot->policy) {
          armed = true;  // Partial prepare is also a transaction.
          const auto &policy = *snapshot->policy;
          auto result = runner(policy.prepare, *snapshot, policy.timeout);
          helper_settled = result.settled;
          if (!result.success || !result.settled) {
            throw std::runtime_error("Display prepare failed: " + result.error);
          }
        }
        check_valid();
        capture();
        check_valid();
        transport();
        check_valid();
        started = true;
        return true;
      } catch (const std::exception &e) {
        error = e.what();
      } catch (...) {
        error = "Unknown session startup failure";
      }
      try {
        auto recovery = finish(settle);
        if (!recovery.empty()) {
          error += "; " + recovery;
        }
      } catch (...) {
        error += "; " + gate.error();
      }
      return false;
    }

    std::string finish(const std::function<void()> &settle) {
      if (!ownership) {
        return {};
      }
      // A failed settle cannot safely be followed by a display change.
      try {
        settle();
      } catch (...) {
        gate.poison("Session cleanup failed; display recovery withheld until host restart");
        ownership.reset();
        throw;
      }
      std::string error;
      if (armed) {
        armed = false;
        if (!helper_settled) {
          error = "Display helper process group did not settle; recovery withheld until host restart";
        } else {
          try {
            const auto &policy = *snapshot->policy;
            auto result = runner(policy.recover, *snapshot, policy.timeout);
            if (!result.success || !result.settled) {
              error = "Display recovery failed: " + result.error;
            }
          } catch (const std::exception &e) {
            error = "Display recovery failed: " + std::string(e.what());
          } catch (...) {
            error = "Display recovery failed with an unknown exception";
          }
        }
        if (!error.empty()) {
          gate.poison(error + "; capture and app display commands blocked until host restart/recovery");
        }
      }
      ownership.reset();
      started = false;
      return error;
    }

    bool active() const {
      return started;
    }

  private:
    void check_valid() {
      if (snapshot && !snapshot->valid->load()) {
        throw std::runtime_error("Application launch was cancelled or exited during startup");
      }
    }

    pyrowave::CaptureGate &gate;
    std::shared_ptr<const Snapshot> snapshot;
    Runner runner;
    std::shared_ptr<void> ownership;
    bool armed = false;
    bool helper_settled = true;
    bool started = false;
  };
}  // namespace session_display
