#include "worker.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "log.hpp"

namespace ts {

WorkerManager::WorkerManager(const Config &cfg) : cfg_(cfg) {
  std::srand(static_cast<unsigned>(std::time(nullptr)) ^ getpid());
}

WorkerManager::~WorkerManager() { stop(); }

bool WorkerManager::app_exists(const std::string &app) const {
  // Reject anything that could escape apps_dir.
  if (app.empty() || app.find('/') != std::string::npos ||
      app.find("..") != std::string::npos || app[0] == '.') {
    return false;
  }
  std::string appjs = cfg_.apps_dir + "/" + app + "/app.R";
  struct stat st{};
  return ::stat(appjs.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

uint16_t WorkerManager::allocate_port() {
  for (uint16_t i = 0; i < cfg_.worker_port_count; ++i) {
    uint16_t p = static_cast<uint16_t>(cfg_.worker_port_base + i);
    if (!ports_in_use_[p]) {
      ports_in_use_[p] = true;
      return p;
    }
  }
  return 0;  // exhausted
}

void WorkerManager::release_port(uint16_t p) { ports_in_use_[p] = false; }

bool WorkerManager::wait_for_port(uint16_t port, int timeout_s) const {
  auto deadline = Clock::now() + std::chrono::seconds(timeout_s);
  while (Clock::now() < deadline) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      ::close(fd);
      if (rc == 0) return true;
    }
    usleep(150 * 1000);  // 150ms
  }
  return false;
}

std::shared_ptr<Worker> WorkerManager::get_session(const std::string &sid) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(sid);
  return it == sessions_.end() ? nullptr : it->second;
}

std::vector<WorkerStat> WorkerManager::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = Clock::now();
  std::vector<WorkerStat> out;
  out.reserve(sessions_.size());
  for (const auto &kv : sessions_) {
    const auto &w = kv.second;
    WorkerStat s;
    s.sid = w->sid;
    s.app = w->app;
    s.pid = w->pid;
    s.port = w->port;
    s.active_conns = w->active_conns;
    s.ws_seen = w->ws_seen;
    s.uptime_s = std::chrono::duration_cast<std::chrono::duration<double>>(
                     now - w->started_at)
                     .count();
    s.idle_s = std::chrono::duration_cast<std::chrono::duration<double>>(
                   now - w->last_active)
                   .count();
    s.bytes_in = w->bytes_in.load(std::memory_order_relaxed);
    s.bytes_out = w->bytes_out.load(std::memory_order_relaxed);
    out.push_back(std::move(s));
  }
  return out;
}

std::shared_ptr<Worker> WorkerManager::ensure_session(const std::string &sid,
                                                      const std::string &app) {
  uint16_t port = 0;
  std::shared_ptr<Worker> w;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sid);
    if (it != sessions_.end() && it->second->app == app) return it->second;

    // Stale session for a different app: reap it first.
    if (it != sessions_.end()) kill_worker(it->second);

    port = allocate_port();
    if (port == 0) {
      TS_ERROR("no worker ports available (increase worker_port_count)");
      return nullptr;
    }
    w = std::make_shared<Worker>();
    w->sid = sid;
    w->app = app;
    w->port = port;
    w->started_at = Clock::now();
    w->last_active = Clock::now();
    w->disconnected_at = Clock::now();
    sessions_[sid] = w;
  }

  std::string app_dir = cfg_.apps_dir + "/" + app;
  std::string log_path = cfg_.log_dir + "/" + app + "-" + sid + ".log";

  pid_t pid = ::fork();
  if (pid < 0) {
    TS_ERROR("fork failed: %s", std::strerror(errno));
    std::lock_guard<std::mutex> lock(mutex_);
    release_port(port);
    sessions_.erase(sid);
    return nullptr;
  }

  if (pid == 0) {
    // ---- child ----
    // New session so KillMode=process in systemd can signal the group.
    setsid();

    // Redirect stdout/stderr to a per-session log file.
    int logfd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (logfd >= 0) {
      dup2(logfd, STDOUT_FILENO);
      dup2(logfd, STDERR_FILENO);
      if (logfd > STDERR_FILENO) ::close(logfd);
    }

    // Drop privileges if configured and we are root.
    if (!cfg_.run_as.empty() && geteuid() == 0) {
      struct passwd *pw = getpwnam(cfg_.run_as.c_str());
      if (pw) {
        if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 ||
            setuid(pw->pw_uid) != 0) {
          _exit(127);
        }
        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
      }
    }

    setenv("TABLER_WORKER_PORT", std::to_string(port).c_str(), 1);
    setenv("TABLER_APP_DIR", app_dir.c_str(), 1);

    execlp(cfg_.rscript.c_str(), cfg_.rscript.c_str(), "--vanilla",
           cfg_.worker_script.c_str(), static_cast<char *>(nullptr));
    _exit(127);  // exec failed
  }

  // ---- parent ----
  {
    std::lock_guard<std::mutex> lock(mutex_);
    w->pid = pid;
  }
  TS_INFO("spawned worker app=%s sid=%s pid=%d port=%u", app.c_str(),
          sid.c_str(), pid, port);

  if (!wait_for_port(port, cfg_.worker_start_timeout)) {
    TS_ERROR("worker app=%s sid=%s did not start within %ds", app.c_str(),
             sid.c_str(), cfg_.worker_start_timeout);
    std::lock_guard<std::mutex> lock(mutex_);
    kill_worker(w);
    return nullptr;
  }
  return w;
}

void WorkerManager::conn_opened(const std::shared_ptr<Worker> &w, bool is_ws) {
  if (!w) return;
  std::lock_guard<std::mutex> lock(mutex_);
  w->active_conns++;
  w->last_active = Clock::now();
  if (is_ws) w->ws_seen = true;
}

void WorkerManager::conn_closed(const std::shared_ptr<Worker> &w) {
  if (!w) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (w->active_conns > 0) w->active_conns--;
  w->last_active = Clock::now();
  if (w->active_conns == 0) w->disconnected_at = Clock::now();
}

void WorkerManager::touch(const std::shared_ptr<Worker> &w) {
  if (!w) return;
  std::lock_guard<std::mutex> lock(mutex_);
  w->last_active = Clock::now();
}

void WorkerManager::kill_worker(const std::shared_ptr<Worker> &w) {
  // Precondition: mutex_ held.
  if (w->pid > 0) {
    ::kill(-w->pid, SIGTERM);  // signal the whole process group
    ::kill(w->pid, SIGTERM);
  }
  release_port(w->port);
  sessions_.erase(w->sid);
  TS_INFO("reaped worker app=%s sid=%s pid=%d", w->app.c_str(), w->sid.c_str(),
          w->pid);
}

void WorkerManager::start_reaper() {
  reaper_ = std::thread([this] { reaper_loop(); });
}

void WorkerManager::reaper_loop() {
  while (true) {
    for (int i = 0; i < 20; ++i) {  // ~2s sleep, but responsive to stop()
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
      }
      usleep(100 * 1000);
    }

    // Reap any exited children so they don't linger as zombies.
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto now = Clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      auto w = it->second;
      ++it;  // advance before possible erase in kill_worker

      auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                      now - w->last_active)
                      .count();

      bool idle_expired = idle > cfg_.idle_timeout;

      // A worker whose WebSocket has connected and then fully disconnected is
      // reaped shortly after (closed/reloaded tab), using disconnect_grace.
      bool disconnect_expired = false;
      if (w->active_conns == 0 && w->ws_seen) {
        auto down = std::chrono::duration_cast<std::chrono::seconds>(
                        now - w->disconnected_at)
                        .count();
        disconnect_expired = down > cfg_.disconnect_grace;
      }

      if (idle_expired || disconnect_expired) kill_worker(w);
    }
  }
}

void WorkerManager::stop() {
  std::thread reaper_to_join;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    running_ = false;
    for (auto &kv : sessions_) {
      auto &w = kv.second;
      if (w->pid > 0) {
        ::kill(-w->pid, SIGTERM);
        ::kill(w->pid, SIGTERM);
      }
    }
    sessions_.clear();
  }
  if (reaper_.joinable()) reaper_.join();
}

}  // namespace ts
