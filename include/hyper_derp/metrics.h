/// @file metrics.h
/// @brief HTTP metrics server: Prometheus endpoint, health
///   check, and debug introspection.

#ifndef INCLUDE_HYPER_DERP_METRICS_H_
#define INCLUDE_HYPER_DERP_METRICS_H_

#include <cstdint>
#include <string>

#include "hyper_derp/types.h"

namespace hyper_derp {

/// Metrics server configuration.
struct MetricsConfig {
  uint16_t port = 0;  // 0 = disabled
  std::string tls_cert;
  std::string tls_key;
};

/// Opaque metrics server handle.
struct MetricsServer;

/// @brief Start the metrics HTTP server on a background
///   thread.
/// @param config Server configuration.
/// @param ctx Data plane context (read-only stats access).
/// @returns Opaque handle, or nullptr on failure.
MetricsServer* MetricsStart(const MetricsConfig& config,
                            Ctx* ctx);

/// @brief Stop the metrics server and free resources.
/// @param server Handle from MetricsStart.
void MetricsStop(MetricsServer* server);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_METRICS_H_
