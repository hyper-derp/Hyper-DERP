/// @file ice.h
/// @brief ICE agent for Level 2 NAT traversal (RFC 8445).
///   Gathers candidates, exchanges them via HD PeerInfo
///   frames, performs connectivity checks (STUN Binding),
///   and manages upgrade/downgrade between connection
///   levels.

#ifndef INCLUDE_HYPER_DERP_ICE_H_
#define INCLUDE_HYPER_DERP_ICE_H_

#include <cstdint>
#include <mutex>

#include "hyper_derp/protocol.h"

namespace hyper_derp {

// -- ICE candidate types -----------------------------------

/// ICE candidate types.
enum class IceCandidateType : uint8_t {
  kHost = 0,             // Local interface address
  kServerReflexive = 1,  // STUN-discovered address
  kRelay = 2,            // TURN relay address
};

/// Single ICE candidate.
struct IceCandidate {
  IceCandidateType type = IceCandidateType::kHost;
  uint32_t ip = 0;       // Network byte order
  uint16_t port = 0;     // Network byte order
  uint32_t priority = 0;
  uint8_t occupied = 0;
};

/// Maximum candidates per peer.
inline constexpr int kIceMaxCandidates = 16;

// -- ICE connection state ----------------------------------

/// ICE connection state.
enum class IceState : uint8_t {
  kNew = 0,
  kGathering = 1,
  kChecking = 2,
  kConnected = 3,
  kFailed = 4,
  kClosed = 5,
};

// -- ICE candidate pair ------------------------------------

/// ICE candidate pair (local + remote).
struct IceCandidatePair {
  IceCandidate local;
  IceCandidate remote;
  uint64_t priority = 0;
  uint8_t nominated = 0;
  uint8_t succeeded = 0;
  uint8_t in_progress = 0;
  uint8_t failed = 0;
  uint8_t transaction_id[12]{};
};

/// Maximum candidate pairs to check.
inline constexpr int kIceMaxPairs = 64;

// -- Per-peer ICE session ----------------------------------

/// Per-peer ICE session.
struct IceSession {
  Key peer_key{};
  IceState state = IceState::kNew;
  IceCandidate local_candidates[kIceMaxCandidates]{};
  int local_count = 0;
  IceCandidate remote_candidates[kIceMaxCandidates]{};
  int remote_count = 0;
  IceCandidatePair pairs[kIceMaxPairs]{};
  int pair_count = 0;
  int nominated_pair = -1;  // Index into pairs[]
  uint64_t last_check_ns = 0;
  int check_interval_ms = 50;  // Ta timer (ms)
  uint8_t occupied = 0;
};

/// Maximum concurrent ICE sessions.
inline constexpr int kIceMaxSessions = 256;

// -- ICE agent ---------------------------------------------

/// ICE agent managing all sessions.
struct IceAgent {
  IceSession sessions[kIceMaxSessions]{};
  int session_count = 0;
  uint32_t relay_ip = 0;     // For server-reflexive check
  uint16_t stun_port = 3478;
  std::mutex mutex;
};

// -- Functions ---------------------------------------------

/// Initialize the ICE agent.
void IceInit(IceAgent* agent, uint32_t relay_ip,
             uint16_t stun_port);

/// Start a new ICE session for a peer pair.
/// Called when two HD peers are both connected.
/// Returns nullptr if the session table is full.
IceSession* IceStartSession(IceAgent* agent,
                            const Key& peer_key);

/// Add a local candidate to a session.
/// Returns false if the candidate table is full.
bool IceAddLocalCandidate(IceSession* session,
                          IceCandidateType type,
                          uint32_t ip, uint16_t port);

/// Add a remote candidate received via PeerInfo frame.
/// Returns false if the candidate table is full.
bool IceAddRemoteCandidate(IceSession* session,
                           IceCandidateType type,
                           uint32_t ip, uint16_t port);

/// Generate candidate pairs from local + remote lists.
/// Sorts by priority descending (RFC 8445 priority formula).
void IceFormPairs(IceSession* session);

/// Get the next connectivity check to send.
/// Returns nullptr if no check is due.
IceCandidatePair* IceNextCheck(IceSession* session);

/// Process a STUN Binding Response for a pair.
/// Returns true if a pair was nominated (Level 2 ready).
bool IceProcessResponse(IceSession* session,
                        const uint8_t* transaction_id,
                        uint32_t mapped_ip,
                        uint16_t mapped_port);

/// Mark a check as failed (timeout or error).
void IceCheckFailed(IceSession* session,
                    const uint8_t* transaction_id);

/// Get the nominated pair (Level 2 endpoint).
/// Returns nullptr if not yet nominated.
const IceCandidatePair* IceGetNominated(
    const IceSession* session);

/// Close an ICE session.
void IceCloseSession(IceAgent* agent,
                     const Key& peer_key);

/// Find a session by peer key.
IceSession* IceFindSession(IceAgent* agent,
                           const uint8_t* peer_key);

/// Serialize local candidates into a PeerInfo payload.
/// Format: [1B count][per candidate: 1B type, 4B ip, 2B
///   port]. 7 bytes per candidate + 1 byte count header.
/// Returns bytes written, or 0 on error.
int IceSerializeCandidates(const IceSession* session,
                           uint8_t* buf, int buf_size);

/// Parse remote candidates from a PeerInfo payload.
/// Returns number of candidates parsed, or -1 on error.
int IceParseCandidates(IceSession* session,
                       const uint8_t* data, int len);

/// Calculate ICE candidate priority per RFC 8445 S5.1.2.
/// priority = (2^24)*type_pref + (2^8)*local_pref
///          + (2^0)*(256 - component_id)
uint32_t IceCalcPriority(IceCandidateType type,
                         uint16_t local_pref,
                         int component);

/// Calculate pair priority (controlling agent formula).
/// pair_priority = 2^32*min(G,D) + 2*max(G,D)
///              + (G > D ? 1 : 0)
uint64_t IceCalcPairPriority(uint32_t local_prio,
                             uint32_t remote_prio);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_ICE_H_
