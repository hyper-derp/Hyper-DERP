/// @file ice.cc
/// @brief ICE agent implementation (RFC 8445).

#include "hyper_derp/ice.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace hyper_derp {

namespace {

/// RFC 8445 Section 5.1.2 type preference values.
constexpr uint32_t kTypePrefHost = 126;
constexpr uint32_t kTypePrefSrflx = 100;
constexpr uint32_t kTypePrefRelay = 0;

/// Map candidate type to its type preference value.
uint32_t TypePref(IceCandidateType type) {
  switch (type) {
    case IceCandidateType::kHost:
      return kTypePrefHost;
    case IceCandidateType::kServerReflexive:
      return kTypePrefSrflx;
    case IceCandidateType::kRelay:
      return kTypePrefRelay;
  }
  return 0;
}

}  // namespace

void IceInit(IceAgent* agent, uint32_t relay_ip,
             uint16_t stun_port) {
  std::memset(agent->sessions, 0,
              sizeof(agent->sessions));
  agent->session_count = 0;
  agent->relay_ip = relay_ip;
  agent->stun_port = stun_port;
}

IceSession* IceStartSession(IceAgent* agent,
                            const Key& peer_key) {
  if (agent->session_count >= kIceMaxSessions) {
    return nullptr;
  }
  // Find an unoccupied slot.
  for (int i = 0; i < kIceMaxSessions; ++i) {
    if (agent->sessions[i].occupied == 0) {
      IceSession* s = &agent->sessions[i];
      std::memset(s, 0, sizeof(IceSession));
      s->peer_key = peer_key;
      s->state = IceState::kGathering;
      s->nominated_pair = -1;
      s->check_interval_ms = 50;
      s->occupied = 1;
      ++agent->session_count;
      return s;
    }
  }
  return nullptr;
}

bool IceAddLocalCandidate(IceSession* session,
                          IceCandidateType type,
                          uint32_t ip, uint16_t port) {
  if (session->local_count >= kIceMaxCandidates) {
    return false;
  }
  IceCandidate* c =
      &session->local_candidates[session->local_count];
  c->type = type;
  c->ip = ip;
  c->port = port;
  c->priority = IceCalcPriority(type, 65535, 1);
  c->occupied = 1;
  ++session->local_count;
  return true;
}

bool IceAddRemoteCandidate(IceSession* session,
                           IceCandidateType type,
                           uint32_t ip, uint16_t port) {
  if (session->remote_count >= kIceMaxCandidates) {
    return false;
  }
  IceCandidate* c =
      &session->remote_candidates[session->remote_count];
  c->type = type;
  c->ip = ip;
  c->port = port;
  c->priority = IceCalcPriority(type, 65535, 1);
  c->occupied = 1;
  ++session->remote_count;
  return true;
}

void IceFormPairs(IceSession* session) {
  session->pair_count = 0;
  for (int l = 0; l < session->local_count; ++l) {
    for (int r = 0; r < session->remote_count; ++r) {
      if (session->pair_count >= kIceMaxPairs) break;
      IceCandidatePair* p =
          &session->pairs[session->pair_count];
      std::memset(p, 0, sizeof(IceCandidatePair));
      p->local = session->local_candidates[l];
      p->remote = session->remote_candidates[r];
      p->priority = IceCalcPairPriority(
          p->local.priority, p->remote.priority);
      ++session->pair_count;
    }
    if (session->pair_count >= kIceMaxPairs) break;
  }
  // Sort by priority descending.
  std::sort(
      session->pairs,
      session->pairs + session->pair_count,
      [](const IceCandidatePair& a,
         const IceCandidatePair& b) {
        return a.priority > b.priority;
      });
  session->state = IceState::kChecking;
}

IceCandidatePair* IceNextCheck(IceSession* session) {
  // Find the highest-priority pair that is not yet
  // in_progress, succeeded, or failed. Pairs are already
  // sorted by priority descending.
  for (int i = 0; i < session->pair_count; ++i) {
    IceCandidatePair* p = &session->pairs[i];
    if (p->in_progress || p->succeeded || p->failed) {
      continue;
    }
    p->in_progress = 1;
    randombytes_buf(p->transaction_id,
                    sizeof(p->transaction_id));
    return p;
  }
  return nullptr;
}

bool IceProcessResponse(IceSession* session,
                        const uint8_t* transaction_id,
                        uint32_t mapped_ip,
                        uint16_t mapped_port) {
  (void)mapped_ip;
  (void)mapped_port;
  for (int i = 0; i < session->pair_count; ++i) {
    IceCandidatePair* p = &session->pairs[i];
    if (sodium_memcmp(p->transaction_id, transaction_id,
                      12) == 0 &&
        p->in_progress) {
      p->succeeded = 1;
      p->in_progress = 0;
      // Nominate the first succeeded pair.
      if (session->nominated_pair < 0) {
        p->nominated = 1;
        session->nominated_pair = i;
        session->state = IceState::kConnected;
        return true;
      }
      return false;
    }
  }
  return false;
}

void IceCheckFailed(IceSession* session,
                    const uint8_t* transaction_id) {
  for (int i = 0; i < session->pair_count; ++i) {
    IceCandidatePair* p = &session->pairs[i];
    if (sodium_memcmp(p->transaction_id, transaction_id,
                      12) == 0 &&
        p->in_progress) {
      p->failed = 1;
      p->in_progress = 0;
      break;
    }
  }
  // If all pairs are failed or succeeded (with none
  // nominated), mark session as failed.
  bool any_pending = false;
  for (int i = 0; i < session->pair_count; ++i) {
    IceCandidatePair* p = &session->pairs[i];
    if (!p->succeeded && !p->failed) {
      any_pending = true;
      break;
    }
  }
  if (!any_pending && session->nominated_pair < 0) {
    session->state = IceState::kFailed;
  }
}

const IceCandidatePair* IceGetNominated(
    const IceSession* session) {
  if (session->nominated_pair < 0) return nullptr;
  return &session->pairs[session->nominated_pair];
}

void IceCloseSession(IceAgent* agent,
                     const Key& peer_key) {
  for (int i = 0; i < kIceMaxSessions; ++i) {
    IceSession* s = &agent->sessions[i];
    if (s->occupied &&
        sodium_memcmp(s->peer_key.data(),
                      peer_key.data(), kKeySize) == 0) {
      s->state = IceState::kClosed;
      s->occupied = 0;
      --agent->session_count;
      return;
    }
  }
}

IceSession* IceFindSession(IceAgent* agent,
                           const uint8_t* peer_key) {
  for (int i = 0; i < kIceMaxSessions; ++i) {
    IceSession* s = &agent->sessions[i];
    if (s->occupied &&
        sodium_memcmp(s->peer_key.data(), peer_key,
                      kKeySize) == 0) {
      return s;
    }
  }
  return nullptr;
}

int IceSerializeCandidates(const IceSession* session,
                           uint8_t* buf, int buf_size) {
  // Format: [1B count][per candidate: 1B type, 4B ip,
  //   2B port] = 7 bytes per candidate + 1 header byte.
  int needed = 1 + session->local_count * 7;
  if (buf_size < needed) return 0;
  buf[0] = static_cast<uint8_t>(session->local_count);
  int off = 1;
  for (int i = 0; i < session->local_count; ++i) {
    const IceCandidate* c = &session->local_candidates[i];
    buf[off] = static_cast<uint8_t>(c->type);
    ++off;
    std::memcpy(buf + off, &c->ip, 4);
    off += 4;
    std::memcpy(buf + off, &c->port, 2);
    off += 2;
  }
  return off;
}

int IceParseCandidates(IceSession* session,
                       const uint8_t* data, int len) {
  if (len < 1) return -1;
  int count = data[0];
  int needed = 1 + count * 7;
  if (len < needed) return -1;
  int off = 1;
  int parsed = 0;
  for (int i = 0; i < count; ++i) {
    auto type =
        static_cast<IceCandidateType>(data[off]);
    ++off;
    uint32_t ip;
    std::memcpy(&ip, data + off, 4);
    off += 4;
    uint16_t port;
    std::memcpy(&port, data + off, 2);
    off += 2;
    if (IceAddRemoteCandidate(session, type, ip, port)) {
      ++parsed;
    }
  }
  return parsed;
}

uint32_t IceCalcPriority(IceCandidateType type,
                         uint16_t local_pref,
                         int component) {
  // RFC 8445 Section 5.1.2:
  // priority = (2^24)*type_pref + (2^8)*local_pref
  //          + (2^0)*(256 - component_id)
  uint32_t tp = TypePref(type);
  return (tp << 24) |
         (static_cast<uint32_t>(local_pref) << 8) |
         static_cast<uint32_t>(256 - component);
}

uint64_t IceCalcPairPriority(uint32_t local_prio,
                             uint32_t remote_prio) {
  // RFC 8445 Section 6.1.2.3 (controlling agent):
  // pair_priority = 2^32*min(G,D) + 2*max(G,D)
  //              + (G > D ? 1 : 0)
  // G = controlling (local), D = controlled (remote).
  uint64_t g = local_prio;
  uint64_t d = remote_prio;
  uint64_t mn = std::min(g, d);
  uint64_t mx = std::max(g, d);
  return (mn << 32) | (mx << 1) | (g > d ? 1 : 0);
}

}  // namespace hyper_derp
