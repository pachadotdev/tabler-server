#pragma once

#include "config.hpp"
#include "worker.hpp"

namespace ts {

// The front HTTP/WebSocket reverse proxy. Owns the listening socket and hands
// each accepted connection to a worker thread.
class Proxy {
 public:
  Proxy(const Config &cfg, WorkerManager &workers);

  // Bind + listen. Returns false on failure.
  bool listen_and_serve();

  // Ask the accept loop to stop (call from a signal handler / another thread).
  void stop();

 private:
  void handle_connection(int client_fd);

  const Config &cfg_;
  WorkerManager &workers_;
  int listen_fd_ = -1;
  volatile bool running_ = true;
};

}  // namespace ts
