#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "config.hpp"
#include "worker.hpp"

namespace ts {

// A tiny, read-only HTTP server that exposes an htop-like status dashboard for
// the running workers plus process- and system-wide resource use. It listens on
// its own port (default 3001) and serves:
//   GET /            -> the HTML dashboard (self-refreshing)
//   GET /stats.json  -> the JSON the dashboard polls
class AdminServer {
 public:
  AdminServer(const Config &cfg, WorkerManager &workers);

  // Bind + listen. Blocks until stop(). Returns false on failure.
  bool listen_and_serve();

  // Ask the accept loop to stop (call from a signal handler / another thread).
  void stop();

 private:
  void handle_connection(int client_fd);
  std::string build_stats_json();

  // Per-pid CPU accounting: last (proc_ticks, total_ticks) sample so we can
  // compute a %CPU over the interval between two dashboard polls.
  struct CpuSample {
    uint64_t proc_ticks = 0;
    uint64_t total_ticks = 0;
  };

  const Config &cfg_;
  WorkerManager &workers_;
  int listen_fd_ = -1;
  volatile bool running_ = true;

  std::mutex cpu_mutex_;
  std::map<pid_t, CpuSample> cpu_samples_;
};

}  // namespace ts
