/// @file ice_agent.h
/// @brief ICE NAT traversal agent for the SDK.
///
/// Wraps the relay's ice.h/stun.h for client-side use.
/// Handles candidate gathering, STUN binding, nomination.

#ifndef HD_ICE_AGENT_H_
#define HD_ICE_AGENT_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hd/sdk/error.h"

namespace hd::ice {

struct Candidate {
  enum Type { kHost, kServerReflexive, kRelay };
  Type type;
  uint32_t ip;    // Network byte order.
  uint16_t port;  // Network byte order.
};

struct IceConfig {
  std::vector<std::string> stun_servers;
  int timeout_ms = 5000;
  uint16_t local_port = 0;  // Port to bind for STUN.
};

using CandidateCallback =
    std::function<void(const std::vector<Candidate>&)>;
using NominationCallback =
    std::function<void(uint32_t ip, uint16_t port)>;

/// ICE agent. Gathers candidates, runs checks, nominates.
class IceAgent {
 public:
  explicit IceAgent(const IceConfig& config);
  ~IceAgent();

  IceAgent(const IceAgent&) = delete;
  IceAgent& operator=(const IceAgent&) = delete;

  /// Gather local + STUN candidates.
  hd::sdk::Result<> GatherCandidates(CandidateCallback cb);

  /// Set callback for when a pair is nominated.
  void OnNomination(NominationCallback cb);

  /// Add remote candidates from peer.
  void AddRemoteCandidates(
      const std::vector<Candidate>& candidates);

  /// Get the STUN socket fd (for port inheritance).
  int StunFd() const;

  /// Close the STUN socket (before wg.ko binds).
  void CloseStunSocket();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace hd::ice

#endif  // HD_ICE_AGENT_H_
