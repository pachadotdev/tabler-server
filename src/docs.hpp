#pragma once

#include <cstdint>
#include <string>

namespace ts {

// A tiny, read-only static file server for a documentation site (no R
// workers involved). It listens on its own port and serves files out of a
// root directory, defaulting to `index.html` for directory requests.
// Multiple instances can be run side by side (e.g. tabler-server docs on
// 3010, R package docs on 3020).
class DocsServer {
 public:
  DocsServer(std::string listen, uint16_t port, std::string dir,
             std::string label = "docs");

  // Bind + listen. Blocks until stop(). Returns false on failure.
  bool listen_and_serve();

  // Ask the accept loop to stop (call from a signal handler / another thread).
  void stop();

 private:
  void handle_connection(int client_fd);

  std::string listen_;
  uint16_t port_;
  std::string dir_;
  std::string label_;
  int listen_fd_ = -1;
  volatile bool running_ = true;
};

}  // namespace ts
