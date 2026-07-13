#pragma once

#include <cstdint>
#include <atomic>

namespace ts {

// Process-wide network counters, accumulated by the proxy while it splices
// bytes between clients and workers. Read by the admin dashboard.
//   bytes_in  : bytes received from clients (client -> server)
//   bytes_out : bytes sent to clients       (server -> client)
struct GlobalMetrics {
  std::atomic<uint64_t> bytes_in{0};
  std::atomic<uint64_t> bytes_out{0};
};

// Singleton accessor (C++17 inline function with a function-local static).
inline GlobalMetrics &metrics() {
  static GlobalMetrics m;
  return m;
}

}  // namespace ts
