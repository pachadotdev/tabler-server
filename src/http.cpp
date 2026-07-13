#include "http.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ts {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Shared implementation: read until "\r\n\r\n" is seen (bounded).
bool read_head_impl(int fd, std::string &head, std::string &leftover) {
  head.clear();
  leftover.clear();
  std::string buf;
  char tmp[4096];
  const size_t kMaxHead = 64 * 1024;  // guard against unbounded heads

  while (buf.size() < kMaxHead) {
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;  // EOF or error before full head
    buf.append(tmp, static_cast<size_t>(n));
    size_t pos = buf.find("\r\n\r\n");
    if (pos != std::string::npos) {
      head = buf.substr(0, pos + 4);
      leftover = buf.substr(pos + 4);
      return true;
    }
  }
  return false;
}

}  // namespace

std::string HttpRequest::header(const std::string &name) const {
  auto it = headers.find(lower(name));
  return it == headers.end() ? std::string() : it->second;
}

std::string HttpRequest::path() const {
  size_t q = target.find('?');
  return q == std::string::npos ? target : target.substr(0, q);
}

std::string HttpRequest::cookie(const std::string &name) const {
  std::string c = header("cookie");
  if (c.empty()) return "";
  // Cookies are "; "-separated name=value pairs.
  size_t pos = 0;
  while (pos < c.size()) {
    size_t semi = c.find(';', pos);
    std::string pair = c.substr(pos, semi == std::string::npos ? std::string::npos
                                                               : semi - pos);
    pair = trim(pair);
    size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == name) {
      return pair.substr(eq + 1);
    }
    if (semi == std::string::npos) break;
    pos = semi + 1;
  }
  return "";
}

bool HttpRequest::is_websocket_upgrade() const {
  return lower(header("upgrade")).find("websocket") != std::string::npos;
}

bool read_http_head(int fd, std::string &head, std::string &leftover) {
  return read_head_impl(fd, head, leftover);
}

bool read_http_head_generic(int fd, std::string &head, std::string &leftover) {
  return read_head_impl(fd, head, leftover);
}

bool parse_http_request(const std::string &head, HttpRequest &req) {
  std::istringstream in(head);
  std::string line;
  if (!std::getline(in, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();

  std::istringstream rl(line);
  if (!(rl >> req.method >> req.target >> req.version)) return false;

  req.headers.clear();
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = lower(trim(line.substr(0, colon)));
    std::string value = trim(line.substr(colon + 1));
    req.headers[name] = value;
  }
  req.raw_head = head;
  return true;
}

}  // namespace ts
