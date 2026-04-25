/// @file harness.h
/// @brief Integration test harness: process management and
///   core pinning for fork-based relay testing.

#ifndef TESTS_HARNESS_H_
#define TESTS_HARNESS_H_

#include <cstdint>
#include <sys/types.h>

#include "hyper_derp/hd_client.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {
namespace test {

/// TCP-connect + TLS-upgrade a client to an HD relay that
/// was started via StartHdRelay. Since HD mode requires TLS
/// end-to-end, plain HdClientConnect won't talk to the test
/// relay — use this instead.
auto ConnectHdClient(HdClient* c, const char* host,
                     uint16_t port)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Find an available TCP port via bind(:0).
/// @returns Available port number, or 0 on error.
uint16_t FindFreePort();

/// @brief Start relay server in a forked child process.
/// @param port Port to listen on.
/// @param num_workers Number of data plane workers.
/// @param tls_cert Optional TLS cert path for kTLS.
/// @param tls_key Optional TLS key path for kTLS.
/// @returns Child PID, or -1 on error.
pid_t StartRelay(uint16_t port, int num_workers,
                 const char* tls_cert = nullptr,
                 const char* tls_key = nullptr);

/// @brief Start relay with HD protocol enabled.
/// @param port Port to listen on.
/// @param num_workers Number of data plane workers.
/// @param relay_key 32-byte shared secret for enrollment.
/// @param metrics_port If non-zero, also starts the
///   metrics/admin HTTP server on that port. Tests use
///   this to poke the REST API mid-flight.
/// @param hd_relay_id Fleet identifier for this relay
///   (0 = standalone, no fleet).
/// @param seed_host:seed_port Optional seed relay to
///   connect to on startup. Ignored when empty.
/// @param local_fleet_id Federation identifier used in
///   outbound FleetOpenConnection envelopes.
/// @param accept_fleet_id If non-empty, federation
///   accept_from is seeded with a single rule allowing
///   this fleet_id with no destination restriction.
/// @returns Child PID, or -1 on error.
pid_t StartHdRelay(uint16_t port, int num_workers,
                   const Key& relay_key,
                   uint16_t metrics_port = 0,
                   uint16_t hd_relay_id = 0,
                   const char* seed_host = "",
                   uint16_t seed_port = 0,
                   const char* local_fleet_id = "",
                   const char* accept_fleet_id = "",
                   const char* einheit_ctl = "",
                   const char* einheit_pub = "");

/// @brief Wait until relay accepts TCP connections.
/// @param port Relay port.
/// @param timeout_ms Maximum wait time in milliseconds.
/// @returns 0 if ready, -1 on timeout.
int WaitRelayReady(uint16_t port, int timeout_ms);

/// @brief Stop and reap a relay child process.
/// @param pid Relay process PID.
void StopRelay(pid_t pid);

/// @brief Pin calling process to a specific CPU core.
/// @param core CPU core ID.
/// @returns 0 on success, -1 on error.
int PinToCore(int core);

}  // namespace test
}  // namespace hyper_derp

#endif  // TESTS_HARNESS_H_
