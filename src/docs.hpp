#pragma once

#include "config.hpp"

namespace ts {

// A tiny, read-only static file server for the tabler-server documentation
// site (no R workers involved). It listens on its own port (default 3010)
// and serves files out of `cfg.docs_dir`, defaulting to `index.html` for
// directory requests.
class DocsServer {
 public:
  explicit DocsServer(const Config &cfg);

  // Bind + listen. Blocks until stop(). Returns false on failure.
  bool listen_and_serve();

  // Ask the accept loop to stop (call from a signal handler / another thread).
  void stop();

 private:
  void handle_connection(int client_fd);

  const Config &cfg_;
  int listen_fd_ = -1;
  volatile bool running_ = true;
};

}  // namespace ts
