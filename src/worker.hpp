#pragma once

#include <sys/types.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config.hpp"

namespace ts {

using Clock = std::chrono::steady_clock;

// A single R worker process running one tabler app session.
struct Worker {
  std::string sid;        // session id (matches the tabler_sid cookie)
  std::string app;        // app name
  pid_t pid = -1;
  uint16_t port = 0;      // loopback port the worker's httpuv listens on
  int active_conns = 0;   // live proxied connections (HTTP + WS)
  bool ws_seen = false;   // a WebSocket has connected at least once
  Clock::time_point last_active;
  Clock::time_point disconnected_at;  // when active_conns last hit 0
};

// Owns the worker processes and the reaper thread.
class WorkerManager {
 public:
  explicit WorkerManager(const Config &cfg);
  ~WorkerManager();

  // Returns true if `app` is a directory containing app.R under apps_dir.
  bool app_exists(const std::string &app) const;

  // Create (or return existing) worker for a session. Spawns an R process and
  // waits until it accepts connections. Returns nullptr on failure.
  std::shared_ptr<Worker> ensure_session(const std::string &sid,
                                         const std::string &app);

  // Look up an existing session's worker by sid, or nullptr.
  std::shared_ptr<Worker> get_session(const std::string &sid);

  // Connection accounting used to drive idle/disconnect reaping.
  void conn_opened(const std::shared_ptr<Worker> &w, bool is_ws);
  void conn_closed(const std::shared_ptr<Worker> &w);
  void touch(const std::shared_ptr<Worker> &w);

  void start_reaper();
  void stop();

 private:
  uint16_t allocate_port();     // caller must hold mutex_
  void release_port(uint16_t p);
  bool wait_for_port(uint16_t port, int timeout_s) const;
  void kill_worker(const std::shared_ptr<Worker> &w);  // caller holds mutex_
  void reaper_loop();

  const Config &cfg_;
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Worker>> sessions_;  // sid -> worker
  std::map<uint16_t, bool> ports_in_use_;
  std::thread reaper_;
  bool running_ = true;
};

}  // namespace ts
