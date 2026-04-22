/// @file hd_resolver.h
/// @brief Five-layer routing policy resolver.
///
/// The resolver is a pure function. Each of the five
/// layers (fleet, federation, relay, peer, client)
/// contributes a view made of an allowed-mode bitmask
/// and an optional pinned intent. The resolver
/// intersects the bitmasks, applies pins in strict layer
/// order (fleet binds strongest), and returns the final
/// decision.
///
/// Phase 2 populates only the client view; the other
/// layers default to "allow everything, pin nothing" and
/// are filled in by Phases 3-5.

#ifndef INCLUDE_HYPER_DERP_HD_RESOLVER_H_
#define INCLUDE_HYPER_DERP_HD_RESOLVER_H_

#include <cstdint>
#include <optional>

#include "hyper_derp/hd_protocol.h"

namespace hyper_derp {

/// Bitmask of permitted modes.
inline constexpr uint8_t kModeDirect = 0x01;
inline constexpr uint8_t kModeRelayed = 0x02;
inline constexpr uint8_t kModeAny =
    kModeDirect | kModeRelayed;

/// Per-layer view. `allowed` is the set of permitted
/// modes; `pinned_intent` forces a specific intent when
/// present (used by peer `override_client` and by relay
/// or fleet pins).
struct HdLayerView {
  uint8_t allowed = kModeAny;
  std::optional<HdIntent> pinned_intent;
};

/// Client-layer view: direct copy of what the client
/// asked for in OpenConnection. `allow_upgrade` and
/// `allow_downgrade` flow through to the state machine
/// via the Decision.
struct HdClientView {
  HdIntent intent = HdIntent::kPreferDirect;
  bool allow_upgrade = true;
  bool allow_downgrade = true;
};

/// Capability view. Set by the ICE / NAT layer when it
/// knows the peer pair cannot achieve direct. Phase 2:
/// default true; Phase 5 populates from probe results.
struct HdCapability {
  bool can_direct = true;
};

/// Resolver output.
struct HdDecision {
  HdConnMode mode = HdConnMode::kDenied;
  HdDenyReason deny_reason = HdDenyReason::kNone;
  uint8_t sub_reason = 0;
  /// Copy of the intent that was resolved (may differ
  /// from client_intent when a higher layer pinned).
  HdIntent resolved_intent = HdIntent::kPreferDirect;
  bool allow_upgrade = true;
  bool allow_downgrade = true;
  /// Per-layer allowed-mode bitmasks after intersection.
  /// Indexed by HdLayer. Used for audit log chain.
  uint8_t layer_allowed[5] = {kModeAny, kModeAny,
                               kModeAny, kModeAny,
                               kModeAny};
};

/// Layer identifiers (also array indices in
/// HdDecision::layer_allowed).
enum class HdLayer : uint8_t {
  kFleet = 0,
  kFederation = 1,
  kRelay = 2,
  kPeer = 3,
  kClient = 4,
};

/// @brief Resolves a connection mode from the five
///   policy layers plus capability.
///
/// Evaluation order (top-down, each layer can only
/// restrict, never widen):
///   1. Apply each layer's `allowed` bitmask via AND
///   2. Apply the first non-empty pinned_intent
///      (fleet wins over federation wins over relay
///      wins over peer)
///   3. Filter by capability (no direct if !can_direct)
///   4. Apply client intent within the remaining set
///
/// @param fleet Fleet-layer view.
/// @param federation Federation-layer view.
/// @param relay Relay-layer view.
/// @param peer Peer-layer view.
/// @param client Client-layer view.
/// @param cap Capability view.
/// @param target_relay_id 0 = local; non-zero currently
///   produces kFleetRoutingNotImplemented until Phase 5.
/// @returns Resolved decision.
HdDecision HdResolve(const HdLayerView& fleet,
                     const HdLayerView& federation,
                     const HdLayerView& relay,
                     const HdLayerView& peer,
                     const HdClientView& client,
                     const HdCapability& cap,
                     uint16_t target_relay_id);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_RESOLVER_H_
