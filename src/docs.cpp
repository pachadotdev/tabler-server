#include "docs.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "http.hpp"
#include "log.hpp"

namespace ts {

namespace {

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

bool send_all(int fd, const std::string &s) { return send_all(fd, s.data(), s.size()); }

std::string http_response_head(int status, const std::string &reason,
                               const std::string &ctype, size_t body_len,
                               const char *extra_headers = "") {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
         "Content-Type: " + ctype + "\r\n" +
         "Content-Length: " + std::to_string(body_len) + "\r\n" + extra_headers +
         "Connection: close\r\n\r\n";
}

std::string http_response(int status, const std::string &reason,
                          const std::string &ctype, const std::string &body) {
  return http_response_head(status, reason, ctype, body.size()) + body;
}

// Guess a Content-Type from a file extension. Falls back to a generic binary
// type for anything unrecognized.
std::string guess_content_type(const std::string &path) {
  size_t dot = path.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
  for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "js" || ext == "mjs") return "application/javascript; charset=utf-8";
  if (ext == "json" || ext == "map") return "application/json; charset=utf-8";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "ico") return "image/x-icon";
  if (ext == "woff") return "font/woff";
  if (ext == "woff2") return "font/woff2";
  if (ext == "ttf") return "font/ttf";
  if (ext == "eot") return "application/vnd.ms-fontobject";
  if (ext == "txt") return "text/plain; charset=utf-8";
  if (ext == "xml") return "application/xml; charset=utf-8";
  if (ext == "pdf") return "application/pdf";
  return "application/octet-stream";
}

// Strip the query string / fragment and percent-decode a request path.
std::string decode_path(const std::string &raw) {
  std::string no_query = raw.substr(0, raw.find_first_of("?#"));
  std::string out;
  out.reserve(no_query.size());
  for (size_t i = 0; i < no_query.size(); ++i) {
    if (no_query[i] == '%' && i + 2 < no_query.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int hi = hex(no_query[i + 1]);
      int lo = hex(no_query[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(no_query[i]);
  }
  return out;
}

// Resolve a decoded request path to a filesystem path under `root`, rejecting
// any attempt to escape it via ".." segments (no symlink/canonicalization is
// needed: rejecting ".." outright keeps every resolved path inside `root`).
// Returns false if the path is unsafe.
bool safe_join(const std::string &root, const std::string &decoded_path,
              std::string &out) {
  std::string rel;
  std::stringstream ss(decoded_path);
  std::string seg;
  std::vector<std::string> segs;
  while (std::getline(ss, seg, '/')) {
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") return false;
    segs.push_back(seg);
  }
  for (const auto &s : segs) {
    rel += "/" + s;
  }
  out = root + rel;
  return true;
}

bool read_file(const std::string &path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

DocsServer::DocsServer(std::string listen, uint16_t port, std::string dir,
                       std::string label)
    : listen_(std::move(listen)),
      port_(port),
      dir_(std::move(dir)),
      label_(std::move(label)) {}

void DocsServer::stop() {
  running_ = false;
  if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
}

void DocsServer::handle_connection(int client_fd) {
  std::string head, leftover;
  if (!read_http_head(client_fd, head, leftover)) return;

  HttpRequest req;
  if (!parse_http_request(head, req)) {
    send_all(client_fd, http_response(400, "Bad Request", "text/plain", "Bad Request"));
    return;
  }

  if (req.method != "GET" && req.method != "HEAD") {
    send_all(client_fd,
             http_response(405, "Method Not Allowed", "text/plain", "Method Not Allowed"));
    return;
  }

  std::string decoded = decode_path(req.path());
  std::string fs_path;
  if (!safe_join(dir_, decoded, fs_path)) {
    send_all(client_fd, http_response(400, "Bad Request", "text/plain", "Bad Request"));
    return;
  }

  struct stat st{};
  if (::stat(fs_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    if (!fs_path.empty() && fs_path.back() != '/') fs_path += "/";
    fs_path += "index.html";
  }

  std::string body;
  if (!read_file(fs_path, body)) {
    send_all(client_fd, http_response(404, "Not Found", "text/plain", "Not Found"));
    return;
  }

  std::string ctype = guess_content_type(fs_path);
  if (req.method == "HEAD") {
    send_all(client_fd, http_response_head(200, "OK", ctype, body.size()));
  } else {
    send_all(client_fd, http_response(200, "OK", ctype, body));
  }
}

bool DocsServer::listen_and_serve() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    TS_ERROR("docs socket() failed: %s", std::strerror(errno));
    return false;
  }
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (inet_pton(AF_INET, listen_.c_str(), &addr.sin_addr) != 1) {
    TS_ERROR("invalid %s listen address: %s", label_.c_str(), listen_.c_str());
    return false;
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    TS_ERROR("%s bind %s:%u failed: %s", label_.c_str(), listen_.c_str(), port_,
             std::strerror(errno));
    return false;
  }
  if (::listen(listen_fd_, 16) != 0) {
    TS_ERROR("%s listen() failed: %s", label_.c_str(), std::strerror(errno));
    return false;
  }
  TS_INFO("%s site on http://%s:%u, serving %s", label_.c_str(), listen_.c_str(),
          port_, dir_.c_str());

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

}  // namespace ts
