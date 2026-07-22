#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace ts {

namespace {

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

int to_int(const std::string &v, int fallback) {
  try {
    return std::stoi(v);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

bool Config::load(const std::string &path, Config &out, std::string &error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot open config file: " + path;
    return false;
  }

  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    std::string s = trim(line);
    if (s.empty()) continue;

    // key <whitespace> value...
    size_t sp = s.find_first_of(" \t");
    std::string key = (sp == std::string::npos) ? s : s.substr(0, sp);
    std::string value = (sp == std::string::npos) ? "" : trim(s.substr(sp + 1));

    if (key == "listen") {
      out.listen = value;
    } else if (key == "port") {
      out.port = static_cast<uint16_t>(to_int(value, out.port));
    } else if (key == "apps_dir") {
      out.apps_dir = value;
    } else if (key == "rscript") {
      out.rscript = value;
    } else if (key == "worker_script") {
      out.worker_script = value;
    } else if (key == "run_as") {
      out.run_as = value;
    } else if (key == "log_dir") {
      out.log_dir = value;
    } else if (key == "admin_enabled") {
      out.admin_enabled = to_int(value, out.admin_enabled ? 1 : 0) != 0;
    } else if (key == "admin_listen") {
      out.admin_listen = value;
    } else if (key == "admin_port") {
      out.admin_port = static_cast<uint16_t>(to_int(value, out.admin_port));
    } else if (key == "docs_enabled") {
      out.docs_enabled = to_int(value, out.docs_enabled ? 1 : 0) != 0;
    } else if (key == "docs_listen") {
      out.docs_listen = value;
    } else if (key == "docs_port") {
      out.docs_port = static_cast<uint16_t>(to_int(value, out.docs_port));
    } else if (key == "docs_dir") {
      out.docs_dir = value;
    } else if (key == "docs_r_enabled") {
      out.docs_r_enabled = to_int(value, out.docs_r_enabled ? 1 : 0) != 0;
    } else if (key == "docs_r_listen") {
      out.docs_r_listen = value;
    } else if (key == "docs_r_port") {
      out.docs_r_port = static_cast<uint16_t>(to_int(value, out.docs_r_port));
    } else if (key == "docs_r_dir") {
      out.docs_r_dir = value;
    } else if (key == "worker_port_base") {
      out.worker_port_base = static_cast<uint16_t>(to_int(value, out.worker_port_base));
    } else if (key == "worker_port_count") {
      out.worker_port_count = static_cast<uint16_t>(to_int(value, out.worker_port_count));
    } else if (key == "idle_timeout") {
      out.idle_timeout = to_int(value, out.idle_timeout);
    } else if (key == "disconnect_grace") {
      out.disconnect_grace = to_int(value, out.disconnect_grace);
    } else if (key == "worker_start_timeout") {
      out.worker_start_timeout = to_int(value, out.worker_start_timeout);
    } else {
      error = "unknown config key '" + key + "' at line " + std::to_string(lineno);
      return false;
    }
  }
  return true;
}

}  // namespace ts
