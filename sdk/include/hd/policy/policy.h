/// @file policy.h
/// @brief Client-side routing policy evaluation.

#ifndef HD_POLICY_H_
#define HD_POLICY_H_

#include <string>

#include "hd/sdk/config.h"

namespace hd::policy {

/// Re-export Intent from sdk config.
using Intent = hd::sdk::Intent;

/// Evaluate routing decision.
/// Resolves client intent against defaults.
struct RoutingDecision {
  Intent intent;
  bool allow_upgrade;
  bool allow_downgrade;
  bool try_ice;         // Should we attempt ICE?
  bool require_direct;  // Fail if direct fails?
  std::string reason;
};

/// Resolve tunnel options against client defaults.
inline RoutingDecision Evaluate(
    const hd::sdk::ClientConfig& defaults,
    const hd::sdk::TunnelOptions& opts) {
  RoutingDecision d{};
  d.intent = opts.routing.value_or(defaults.default_routing);
  d.allow_upgrade = opts.allow_upgrade.value_or(
      defaults.allow_upgrade);
  d.allow_downgrade = opts.allow_downgrade.value_or(
      defaults.allow_downgrade);
  d.reason = opts.reason;

  switch (d.intent) {
    case Intent::PreferDirect:
      d.try_ice = true;
      d.require_direct = false;
      break;
    case Intent::RequireDirect:
      d.try_ice = true;
      d.require_direct = true;
      break;
    case Intent::PreferRelay:
      d.try_ice = false;
      d.require_direct = false;
      break;
    case Intent::RequireRelay:
      d.try_ice = false;
      d.require_direct = false;
      d.allow_upgrade = false;
      break;
  }
  return d;
}

}  // namespace hd::policy

#endif  // HD_POLICY_H_
