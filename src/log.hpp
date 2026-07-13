// Tiny logging helpers. Everything goes to stderr with a level + timestamp so
// systemd/journald captures it. No log framework on purpose.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace ts {

inline std::mutex &log_mutex() {
  static std::mutex m;
  return m;
}

inline void log_line(const char *level, const char *fmt, ...) {
  std::lock_guard<std::mutex> lock(log_mutex());
  char ts[32];
  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&now, &tm_buf);
  std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
  std::fprintf(stderr, "[%s] %-5s ", ts, level);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

}  // namespace ts

#define TS_INFO(...) ::ts::log_line("INFO", __VA_ARGS__)
#define TS_WARN(...) ::ts::log_line("WARN", __VA_ARGS__)
#define TS_ERROR(...) ::ts::log_line("ERROR", __VA_ARGS__)
