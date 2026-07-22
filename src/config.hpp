#pragma once

#include <cstdint>
#include <string>

namespace ts {

// Runtime configuration, parsed from a simple `key value` file.
struct Config {
  std::string listen = "127.0.0.1";
  uint16_t port = 3000;
  std::string apps_dir = "/srv/tabler-server/apps";
  std::string rscript = "Rscript";
  std::string worker_script = "share/worker.R";
  std::string run_as;  // empty = do not switch user
  std::string log_dir = "/var/log/tabler-server";

  // --- Admin dashboard (htop-like status page) ---
  bool admin_enabled = true;
  std::string admin_listen = "127.0.0.1";
  uint16_t admin_port = 3001;

  // --- Static documentation site (served read-only, no R workers) ---
  bool docs_enabled = true;
  std::string docs_listen = "127.0.0.1";
  uint16_t docs_port = 3010;
  std::string docs_dir = "/opt/tabler-server/share/docs";

  // --- Static R package documentation site (served read-only) ---
  bool docs_r_enabled = true;
  std::string docs_r_listen = "127.0.0.1";
  uint16_t docs_r_port = 3020;
  std::string docs_r_dir = "/opt/tabler-server/share/docs-r";

  uint16_t worker_port_base = 34000;
  uint16_t worker_port_count = 256;

  int idle_timeout = 300;          // seconds of inactivity before a worker dies
  int disconnect_grace = 15;       // seconds after WS close before a worker dies
  int worker_start_timeout = 20;   // seconds to wait for a worker to accept

  // Parse a config file. Returns false and sets `error` on failure.
  static bool load(const std::string &path, Config &out, std::string &error);
};

}  // namespace ts
