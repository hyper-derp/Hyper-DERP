/// @file fleet_controller.h
/// @brief Relay-side client for the signed fleet policy
///   control plane.
///
/// The FleetController polls an HTTPS endpoint every
/// ~60s (±10s jitter), verifies the bundle's Ed25519
/// signature against a fleet public key pinned at
/// config time, and on each successful fetch atomically
/// swaps the HdFleetPolicy + HdFederationPolicy +
/// revocations into the HdPeerRegistry.
///
/// Any failure (network, bad signature, version
/// regression, fleet_id mismatch) leaves the last-
/// known-good state in place. A relay that successfully
/// loads a bundle once will never unload it; a fresh
/// relay without a last-known-good refuses to serve
/// routing decisions that depend on fleet state.

#ifndef INCLUDE_HYPER_DERP_FLEET_CONTROLLER_H_
#define INCLUDE_HYPER_DERP_FLEET_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "hyper_derp/hd_peers.h"

namespace hyper_derp {

/// Configuration for the fleet controller.
struct FleetControllerConfig {
  /// Base URL of the policy server (e.g.
  /// "https://fleet.example.com").
  std::string url;
  /// Base64-encoded Ed25519 verify key pinned at
  /// provisioning time.
  std::string signing_pubkey_b64;
  /// mTLS client cert + key paths. Empty disables
  /// mTLS (not recommended in production).
  std::string client_cert;
  std::string client_key;
  /// Path to a CA bundle for server cert verification.
  /// Empty uses the system default trust store.
  std::string ca_bundle;
  /// Poll cadence in seconds. Actual cadence jitters
  /// by ±10s to avoid thundering-herd on server
  /// restart.
  int poll_interval_secs = 60;
  /// Expected fleet_id. Bundles with a different
  /// fleet_id are rejected.
  std::string fleet_id;
  /// Local path where the last-known-good bundle is
  /// cached across restarts.
  std::string bundle_cache_path;
};

/// Reason for the last bundle application attempt.
enum class FleetApplyStatus : uint8_t {
  kNever = 0,
  kOk = 1,
  kNetworkError = 2,
  kBadSignature = 3,
  kStaleVersion = 4,
  kFleetMismatch = 5,
  kParseError = 6,
  kRevoked = 7,
};

struct FleetController {
  FleetControllerConfig config;
  HdPeerRegistry* hd_peers = nullptr;
  /// Last version successfully applied.
  std::atomic<int64_t> applied_version{0};
  /// Most recent apply status (relaxed; observability).
  std::atomic<FleetApplyStatus> last_status{
      FleetApplyStatus::kNever};
  /// Set by the revocation handler when the bundle
  /// revokes this relay's own relay_id. The control
  /// thread reads this and triggers self-termination.
  std::atomic<bool> self_revoked{false};
  /// Main loop state.
  std::atomic<int> running{0};
  std::thread thread;
};

/// @brief Starts the fleet controller in a background
///   thread. Loads the bundle cache (if present) into
///   the registry before returning, so even if the
///   first poll fails the relay has fleet state.
/// @returns True on successful start.
bool FleetControllerStart(FleetController* fc,
                          HdPeerRegistry* hd_peers,
                          const FleetControllerConfig&
                              config);

/// @brief Stops the controller thread.
void FleetControllerStop(FleetController* fc);

/// @brief Verifies and applies a bundle payload. Exposed
///   for testing without the HTTPS transport.
/// @param fc Controller state.
/// @param body Raw bytes of bundle.json.
/// @param body_len Length of body.
/// @returns Apply status; side-effects the registry on
///   success.
FleetApplyStatus FleetControllerApplyBundle(
    FleetController* fc, const uint8_t* body,
    int body_len);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_FLEET_CONTROLLER_H_
