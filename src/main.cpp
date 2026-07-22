#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "config.hpp"
#include "log.hpp"
#include "proxy.hpp"
#include "worker.hpp"
#include "admin.hpp"
#include "docs.hpp"

namespace {

ts::Proxy *g_proxy = nullptr;
ts::WorkerManager *g_workers = nullptr;
ts::AdminServer *g_admin = nullptr;
ts::DocsServer *g_docs = nullptr;
ts::DocsServer *g_docs_r = nullptr;

void on_signal(int) {
  if (g_proxy) g_proxy->stop();
  if (g_admin) g_admin->stop();
  if (g_docs) g_docs->stop();
  if (g_docs_r) g_docs_r->stop();
}

void print_usage(const char *argv0) {
  std::fprintf(stderr,
               "tabler-server - minimal server for tabler R apps\n\n"
               "Usage: %s --config <file>\n\n"
               "Options:\n"
               "  -c, --config <file>   configuration file (required)\n"
               "  -h, --help            show this help\n",
               argv0);
}

}  // namespace

int main(int argc, char **argv) {
  std::string config_path;

  static const struct option long_opts[] = {
      {"config", required_argument, nullptr, 'c'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "c:h", long_opts, nullptr)) != -1) {
    switch (opt) {
      case 'c':
        config_path = optarg;
        break;
      case 'h':
        print_usage(argv[0]);
        return 0;
      default:
        print_usage(argv[0]);
        return 2;
    }
  }

  if (config_path.empty()) {
    print_usage(argv[0]);
    return 2;
  }

  ts::Config cfg;
  std::string err;
  if (!ts::Config::load(config_path, cfg, err)) {
    TS_ERROR("config error: %s", err.c_str());
    return 1;
  }

  // Best-effort creation of the log directory for worker logs.
  ::mkdir(cfg.log_dir.c_str(), 0755);

  // SIGPIPE is handled per-send with MSG_NOSIGNAL; ignore it globally too.
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  ts::WorkerManager workers(cfg);
  ts::Proxy proxy(cfg, workers);
  g_proxy = &proxy;
  g_workers = &workers;

  workers.start_reaper();

  // Optional htop-like status dashboard on its own port.
  ts::AdminServer admin(cfg, workers);
  std::thread admin_thread;
  if (cfg.admin_enabled) {
    g_admin = &admin;
    admin_thread = std::thread([&admin] { admin.listen_and_serve(); });
  }

  // Optional static documentation site on its own port.
  ts::DocsServer docs(cfg.docs_listen, cfg.docs_port, cfg.docs_dir, "docs");
  std::thread docs_thread;
  if (cfg.docs_enabled) {
    g_docs = &docs;
    docs_thread = std::thread([&docs] { docs.listen_and_serve(); });
  }

  // Optional static R package documentation site on its own port.
  ts::DocsServer docs_r(cfg.docs_r_listen, cfg.docs_r_port, cfg.docs_r_dir, "docs-r");
  std::thread docs_r_thread;
  if (cfg.docs_r_enabled) {
    g_docs_r = &docs_r;
    docs_r_thread = std::thread([&docs_r] { docs_r.listen_and_serve(); });
  }

  bool ok = proxy.listen_and_serve();

  TS_INFO("shutting down; stopping workers");
  if (cfg.admin_enabled) {
    admin.stop();
    if (admin_thread.joinable()) admin_thread.join();
  }
  if (cfg.docs_enabled) {
    docs.stop();
    if (docs_thread.joinable()) docs_thread.join();
  }
  if (cfg.docs_r_enabled) {
    docs_r.stop();
    if (docs_r_thread.joinable()) docs_r_thread.join();
  }
  workers.stop();
  return ok ? 0 : 1;
}
