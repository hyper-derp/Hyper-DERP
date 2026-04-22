/// @file hd_resolver.cc
/// @brief Five-layer routing policy resolver.

#include "hyper_derp/hd_resolver.h"

namespace hyper_derp {

namespace {

// Maps an intent to the required mode bit. "Prefer"
// intents accept either mode; "require" intents narrow
// to one.
uint8_t IntentNeed(HdIntent intent) {
  switch (intent) {
    case HdIntent::kPreferDirect:
      return kModeAny;
    case HdIntent::kRequireDirect:
      return kModeDirect;
    case HdIntent::kPreferRelay:
      return kModeAny;
    case HdIntent::kRequireRelay:
      return kModeRelayed;
  }
  return kModeAny;
}

// Maps an intent to its preferred mode when the allowed
// set contains both bits.
HdConnMode IntentPreferred(HdIntent intent) {
  switch (intent) {
    case HdIntent::kPreferDirect:
    case HdIntent::kRequireDirect:
      return HdConnMode::kDirect;
    case HdIntent::kPreferRelay:
    case HdIntent::kRequireRelay:
      return HdConnMode::kRelayed;
  }
  return HdConnMode::kRelayed;
}

}  // namespace

HdDecision HdResolve(const HdLayerView& fleet,
                     const HdLayerView& federation,
                     const HdLayerView& relay,
                     const HdLayerView& peer,
                     const HdClientView& client,
                     const HdCapability& cap,
                     uint16_t target_relay_id) {
  HdDecision d;
  d.allow_upgrade = client.allow_upgrade;
  d.allow_downgrade = client.allow_downgrade;

  // Phase 2 guard: cross-relay routing arrives in
  // Phase 5. Reject non-zero target_relay_id cleanly so
  // older relays don't mis-route a Phase 5 request.
  if (target_relay_id != 0) {
    d.mode = HdConnMode::kDenied;
    d.deny_reason =
        HdDenyReason::kFleetRoutingNotImplemented;
    return d;
  }

  // Step 1: intersect allowed-mode bitmasks.
  uint8_t allowed = fleet.allowed & federation.allowed &
                    relay.allowed & peer.allowed;
  d.layer_allowed[static_cast<int>(HdLayer::kFleet)] =
      fleet.allowed;
  d.layer_allowed[static_cast<int>(
      HdLayer::kFederation)] = federation.allowed;
  d.layer_allowed[static_cast<int>(HdLayer::kRelay)] =
      relay.allowed;
  d.layer_allowed[static_cast<int>(HdLayer::kPeer)] =
      peer.allowed;

  // Step 2: pick the pinned intent if any layer set one.
  // Higher layers win (fleet > federation > relay >
  // peer).
  HdIntent effective = client.intent;
  bool pinned = false;
  if (fleet.pinned_intent) {
    effective = *fleet.pinned_intent;
    pinned = true;
  } else if (federation.pinned_intent) {
    effective = *federation.pinned_intent;
    pinned = true;
  } else if (relay.pinned_intent) {
    effective = *relay.pinned_intent;
    pinned = true;
  } else if (peer.pinned_intent) {
    effective = *peer.pinned_intent;
    pinned = true;
  }
  d.resolved_intent = effective;

  // Step 3: capability filter. Remove direct if the
  // pair cannot achieve it.
  if (!cap.can_direct) {
    allowed &= ~kModeDirect;
    if (effective == HdIntent::kRequireDirect) {
      d.mode = HdConnMode::kDenied;
      d.deny_reason = HdDenyReason::kNatIncompatible;
      return d;
    }
  }

  // Step 4: apply intent against the remaining allowed
  // set.
  uint8_t want = IntentNeed(effective);
  uint8_t feasible = allowed & want;
  d.layer_allowed[static_cast<int>(HdLayer::kClient)] =
      want;

  if (feasible == 0) {
    d.mode = HdConnMode::kDenied;
    d.deny_reason = HdDenyReason::kPolicyForbids;
    if (pinned) {
      d.sub_reason = static_cast<uint8_t>(
          static_cast<uint16_t>(
              HdDenyReason::kPeerOverride) &
          0xFF);
    }
    return d;
  }

  HdConnMode preferred = IntentPreferred(effective);
  uint8_t pref_bit = preferred == HdConnMode::kDirect
                         ? kModeDirect
                         : kModeRelayed;
  if (feasible & pref_bit) {
    d.mode = preferred;
  } else {
    d.mode = (feasible & kModeDirect)
                 ? HdConnMode::kDirect
                 : HdConnMode::kRelayed;
  }
  d.deny_reason = HdDenyReason::kNone;
  return d;
}

HdIntent HdApplyPeerPolicyIntent(
    const HdPeerPolicy& policy,
    HdIntent wire_intent) {
  if (policy.has_pin && policy.override_client) {
    return policy.pinned_intent;
  }
  return wire_intent;
}

HdLayerView HdBuildPeerView(
    const HdPeerPolicy& policy,
    HdIntent effective_intent) {
  HdLayerView v;
  if (effective_intent == HdIntent::kRequireDirect ||
      effective_intent == HdIntent::kRequireRelay) {
    v.pinned_intent = effective_intent;
  } else if (effective_intent ==
             HdIntent::kPreferRelay) {
    v.allowed = kModeRelayed;
  }
  if (policy.has_pin && !policy.override_client) {
    switch (policy.pinned_intent) {
      case HdIntent::kRequireRelay:
      case HdIntent::kPreferRelay:
        v.allowed =
            static_cast<uint8_t>(v.allowed & kModeRelayed);
        break;
      case HdIntent::kRequireDirect:
      case HdIntent::kPreferDirect:
        v.allowed =
            static_cast<uint8_t>(v.allowed & kModeDirect);
        break;
    }
  }
  return v;
}

}  // namespace hyper_derp
