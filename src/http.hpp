#pragma once

#include <map>
#include <string>

namespace ts {

// Parsed HTTP request head (request line + headers). Header names are stored
// lower-cased for case-insensitive lookup.
struct HttpRequest {
  std::string method;
  std::string target;   // e.g. "/example1" or "/ws?x=1"
  std::string version;  // e.g. "HTTP/1.1"
  std::map<std::string, std::string> headers;
  std::string raw_head;  // full head including the trailing CRLFCRLF
  std::string leftover;  // bytes read past the head (start of body/frames)

  std::string header(const std::string &name) const;
  // Path portion of the target (without the query string).
  std::string path() const;
  // Value of a cookie by name, or "" if absent.
  std::string cookie(const std::string &name) const;
  bool is_websocket_upgrade() const;
};

// Read an HTTP message head (up to and including "\r\n\r\n") from a socket fd.
// Returns false on EOF/error before a complete head is read.
bool read_http_head(int fd, std::string &head, std::string &leftover);

// Parse a previously read head into `req`. Returns false on malformed input.
bool parse_http_request(const std::string &head, HttpRequest &req);

// Read an HTTP response head from a socket fd (status line + headers).
bool read_http_head_generic(int fd, std::string &head, std::string &leftover);

}  // namespace ts
