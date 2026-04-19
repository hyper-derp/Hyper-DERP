/// @file metrics.h
/// @brief HTTP metrics server: Prometheus endpoint, health
///   check, and debug introspection.

#ifndef INCLUDE_HYPER_DERP_METRICS_H_
#define INCLUDE_HYPER_DERP_METRICS_H_

#include <atomic>
#include <cstdint>
#include <string>

#include "hyper_derp/hd_peers.h"
#include "hyper_derp/types.h"

namespace hyper_derp {

/// Metrics server configuration.
struct MetricsConfig {
  uint16_t port = 0;  // 0 = disabled
  std::string tls_cert;
  std::string tls_key;
  /// Enable /debug/* endpoints (exposes peer keys).
  bool enable_debug = false;
};

/// Opaque metrics server handle.
struct MetricsServer;

/// Server-level HD counters passed to the metrics server.
struct HdServerCounters {
  std::atomic<uint64_t>* enrollments = nullptr;
  std::atomic<uint64_t>* auth_failures = nullptr;
};

/// @brief Start the metrics HTTP server on a background
///   thread.
/// @param config Server configuration.
/// @param ctx Data plane context (read-only stats access).
/// @param hd_peers HD peer registry (nullptr if HD
///   protocol is disabled).
/// @param hd_counters Server-level HD counters (optional).
/// @returns Opaque handle, or nullptr on failure.
MetricsServer* MetricsStart(const MetricsConfig& config,
                            Ctx* ctx,
                            HdPeerRegistry* hd_peers,
                            HdServerCounters hd_counters =
                                {});

/// @brief Stop the metrics server and free resources.
/// @param server Handle from MetricsStart.
void MetricsStop(MetricsServer* server);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_METRICS_H_
