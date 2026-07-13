#include "proxy.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "http.hpp"
#include "log.hpp"

namespace ts {

namespace {

std::string random_sid() {
  static const char *chars = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string s;
  s.reserve(24);
  for (int i = 0; i < 24; ++i) s.push_back(chars[std::rand() % 36]);
  return s;
}

// Write the entire buffer, retrying on partial sends.
bool send_all(int fd, const char *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool send_all(int fd, const std::string &s) {
  return send_all(fd, s.data(), s.size());
}

int connect_loopback(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

// Replace the request-target in an HTTP head's first line, keeping method and
// version, and returning the full head with the remaining header lines intact.
std::string rewrite_target(const HttpRequest &req, const std::string &new_target) {
  size_t eol = req.raw_head.find("\r\n");
  std::string rest =
      eol == std::string::npos ? "\r\n\r\n" : req.raw_head.substr(eol);
  return req.method + " " + new_target + " " + req.version + rest;
}

// Insert a Set-Cookie header right after the status line of a response head.
std::string inject_set_cookie(const std::string &resp_head,
                              const std::string &sid) {
  size_t eol = resp_head.find("\r\n");
  if (eol == std::string::npos) return resp_head;
  std::string cookie = "Set-Cookie: tabler_sid=" + sid +
                       "; Path=/; HttpOnly; SameSite=Lax\r\n";
  return resp_head.substr(0, eol + 2) + cookie + resp_head.substr(eol + 2);
}

// Rewrite the header block so the request uses `Connection: close`. This is
// essential: the proxy routes only the first request on a TCP connection, so we
// must not let the browser (or worker) keep the connection alive and pipe
// further requests straight to the worker, bypassing routing. Must NOT be used
// on WebSocket upgrades, whose `Connection: Upgrade` has to be preserved.
std::string force_connection_close(const std::string &head) {
  size_t end = head.find("\r\n\r\n");
  if (end == std::string::npos) return head;
  std::string block = head.substr(0, end);  // request line + headers
  std::string tail = head.substr(end);      // "\r\n\r\n"

  auto starts_with_ci = [](const std::string &s, const char *p) {
    size_t n = std::strlen(p);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
      if (std::tolower(static_cast<unsigned char>(s[i])) !=
          std::tolower(static_cast<unsigned char>(p[i]))) {
        return false;
      }
    }
    return true;
  };

  std::string result;
  size_t pos = 0;
  bool first_line = true;
  while (pos < block.size()) {
    size_t nl = block.find("\r\n", pos);
    std::string line =
        (nl == std::string::npos) ? block.substr(pos) : block.substr(pos, nl - pos);
    bool drop = !first_line &&
                (starts_with_ci(line, "connection:") ||
                 starts_with_ci(line, "keep-alive:") ||
                 starts_with_ci(line, "proxy-connection:"));
    if (!drop) {
      if (!result.empty()) result += "\r\n";
      result += line;
    }
    first_line = false;
    if (nl == std::string::npos) break;
    pos = nl + 2;
  }
  result += "\r\nConnection: close";
  result += tail;
  return result;
}

std::string http_response(int status, const std::string &reason,
                          const std::string &ctype, const std::string &body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
         "Content-Type: " + ctype + "\r\n" +
         "Content-Length: " + std::to_string(body.size()) + "\r\n" +
         "Connection: close\r\n\r\n" + body;
}

// Directory-listing index page served at "/".
std::string app_index(const std::string &apps_dir) {
  std::string items;
  DIR *d = ::opendir(apps_dir.c_str());
  if (d) {
    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr) {
      std::string name = ent->d_name;
      if (name == "." || name == "..") continue;
      std::string appjs = apps_dir + "/" + name + "/app.R";
      struct stat st{};
      if (::stat(appjs.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        items += "<li><a href=\"/" + name + "\">" + name + "</a></li>";
      }
    }
    ::closedir(d);
  }
  if (items.empty()) items = "<li><em>No apps found</em></li>";
  std::string body =
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<title>tabler-server</title>"
      "<style>body{font-family:system-ui,sans-serif;max-width:40rem;margin:4rem "
      "auto;padding:0 1rem}h1{font-weight:600}li{margin:.4rem 0}</style>"
      "</head><body><h1>tabler-server</h1><p>Available apps:</p><ul>" +
      items + "</ul></body></html>";
  return http_response(200, "OK", "text/html; charset=utf-8", body);
}

// Splice bytes both ways between client and backend until either side closes.
void splice_bidirectional(int a, int b) {
  struct pollfd fds[2];
  fds[0].fd = a;
  fds[1].fd = b;
  char buf[16384];

  for (;;) {
    fds[0].events = POLLIN;
    fds[1].events = POLLIN;
    fds[0].revents = 0;
    fds[1].revents = 0;

    int rc = ::poll(fds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < 2; ++i) {
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        int from = fds[i].fd;
        int to = fds[1 - i].fd;
        ssize_t n = ::recv(from, buf, sizeof(buf), 0);
        if (n <= 0) return;  // EOF or error on either side ends the splice
        if (!send_all(to, buf, static_cast<size_t>(n))) return;
      }
    }
  }
}

}  // namespace

Proxy::Proxy(const Config &cfg, WorkerManager &workers)
    : cfg_(cfg), workers_(workers) {}

void Proxy::stop() {
  running_ = false;
  if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
}

bool Proxy::listen_and_serve() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    TS_ERROR("socket() failed: %s", std::strerror(errno));
    return false;
  }
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg_.port);
  if (inet_pton(AF_INET, cfg_.listen.c_str(), &addr.sin_addr) != 1) {
    TS_ERROR("invalid listen address: %s", cfg_.listen.c_str());
    return false;
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    TS_ERROR("bind %s:%u failed: %s", cfg_.listen.c_str(), cfg_.port,
             std::strerror(errno));
    return false;
  }
  if (::listen(listen_fd_, 128) != 0) {
    TS_ERROR("listen() failed: %s", std::strerror(errno));
    return false;
  }
  TS_INFO("tabler-server listening on %s:%u, apps in %s", cfg_.listen.c_str(),
          cfg_.port, cfg_.apps_dir.c_str());

  while (running_) {
    int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) {
      if (!running_) break;
      if (errno == EINTR) continue;
      continue;
    }
    std::thread([this, client] {
      handle_connection(client);
      ::close(client);
    }).detach();
  }
  ::close(listen_fd_);
  listen_fd_ = -1;
  return true;
}

void Proxy::handle_connection(int client_fd) {
  int one = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  std::string head, leftover;
  if (!read_http_head(client_fd, head, leftover)) return;

  HttpRequest req;
  if (!parse_http_request(head, req)) {
    send_all(client_fd, http_response(400, "Bad Request", "text/plain", "Bad Request"));
    return;
  }
  req.leftover = leftover;

  std::string path = req.path();

  // Root: list available apps.
  if (path == "/" || path.empty()) {
    send_all(client_fd, app_index(cfg_.apps_dir));
    return;
  }

  // Is this the page load for an app? Only "/<app>" or "/<app>/" names an app.
  std::string candidate = path.substr(1);
  if (!candidate.empty() && candidate.back() == '/') candidate.pop_back();
  bool page_load = workers_.app_exists(candidate) &&
                   (path == "/" + candidate || path == "/" + candidate + "/");

  std::shared_ptr<Worker> worker;
  std::string sid;
  bool set_cookie = false;
  std::string out_head = req.raw_head;

  if (page_load) {
    // A page load always starts a fresh session (reload == new session).
    sid = random_sid();
    set_cookie = true;
    worker = workers_.ensure_session(sid, candidate);
    if (!worker) {
      send_all(client_fd, http_response(503, "Service Unavailable", "text/plain",
                                        "Failed to start app worker"));
      return;
    }
    // The worker serves the page at "/"; rewrite the target, keep any query.
    size_t q = req.target.find('?');
    std::string new_target = (q == std::string::npos) ? "/" : "/" + req.target.substr(q);
    out_head = rewrite_target(req, new_target);
  } else {
    // Asset / WebSocket / plot / widget: route by session cookie.
    sid = req.cookie("tabler_sid");
    if (sid.empty()) {
      send_all(client_fd, http_response(400, "Bad Request", "text/plain",
                                        "No session"));
      return;
    }
    worker = workers_.get_session(sid);
    if (!worker) {
      // Session was reaped or never existed; tell the client to reload.
      send_all(client_fd, http_response(404, "Not Found", "text/plain",
                                        "Session expired; reload the page"));
      return;
    }
  }

  int backend = connect_loopback(worker->port);
  if (backend < 0) {
    send_all(client_fd, http_response(502, "Bad Gateway", "text/plain",
                                      "Worker not reachable"));
    return;
  }

  bool is_ws = req.is_websocket_upgrade();
  workers_.conn_opened(worker, is_ws);

  // Force one-request-per-connection for plain HTTP so keep-alive can't pipe a
  // later request (e.g. a reload of /example1, or a navigation to /) straight
  // to this worker and bypass routing. WebSocket upgrades are left intact.
  if (!is_ws) out_head = force_connection_close(out_head);

  bool ok = true;
  // Forward the (possibly rewritten) request head and any buffered body bytes.
  ok = ok && send_all(backend, out_head);
  if (ok && !req.leftover.empty()) ok = send_all(backend, req.leftover);

  if (ok && set_cookie) {
    // Read the worker's response head so we can attach the session cookie.
    std::string resp_head, resp_left;
    if (read_http_head_generic(backend, resp_head, resp_left)) {
      resp_head = inject_set_cookie(resp_head, sid);
      ok = send_all(client_fd, resp_head);
      if (ok && !resp_left.empty()) ok = send_all(client_fd, resp_left);
    } else {
      ok = false;
    }
  }

  if (ok) splice_bidirectional(client_fd, backend);

  ::close(backend);
  workers_.conn_closed(worker);
}

}  // namespace ts
