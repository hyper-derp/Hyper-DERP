/// @file hd_peers.cc
/// @brief HD peer registry implementation.

#include "hyper_derp/hd_peers.h"

#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include <sodium.h>

#include "hyper_derp/key_format.h"

namespace hyper_derp {

namespace {

// Glob match: supports a single trailing '*' wildcard.
// Also matches exactly if no '*' is present.
bool GlobMatch(const std::string& pattern,
               const std::string& text) {
  if (pattern.empty()) return true;
  auto star = pattern.find('*');
  if (star == std::string::npos) {
    return pattern == text;
  }
  // Prefix match up to the star.
  if (text.size() < star) return false;
  return std::memcmp(pattern.data(), text.data(), star) ==
         0;
}

// Parse "10.0.0.0/8" into (net, mask). Both in network
// byte order. Returns false on parse error.
bool ParseCidrV4(const std::string& cidr,
                 uint32_t* out_net,
                 uint32_t* out_mask) {
  auto slash = cidr.find('/');
  if (slash == std::string::npos) return false;
  std::string addr = cidr.substr(0, slash);
  int bits = 0;
  for (size_t i = slash + 1; i < cidr.size(); i++) {
    if (cidr[i] < '0' || cidr[i] > '9') return false;
    bits = bits * 10 + (cidr[i] - '0');
  }
  if (bits < 0 || bits > 32) return false;
  in_addr a{};
  if (inet_aton(addr.c_str(), &a) == 0) return false;
  uint32_t mask_host =
      bits == 0 ? 0 : (0xFFFFFFFFu << (32 - bits));
  *out_mask = htonl(mask_host);
  *out_net = a.s_addr & *out_mask;
  return true;
}

}  // namespace

bool HdPolicyAllows(const HdPeerRegistry* reg,
                    const Key& client_key,
                    uint32_t peer_ipv4_be,
                    const char** out_reason) {
  const auto& pol = reg->policy;
  if (pol.max_peers > 0 &&
      reg->peer_count >= pol.max_peers) {
    if (out_reason) *out_reason = "max_peers exceeded";
    return false;
  }
  if (!pol.allowed_keys.empty()) {
    std::string key_str = KeyToCkString(client_key);
    // Match against the "rk_..."/"ck_..." prefixed form and
    // also the raw 64-hex tail so admins can write either.
    std::string key_hex = key_str.substr(kKeyPrefixLen);
    bool matched = false;
    for (const auto& pat : pol.allowed_keys) {
      if (GlobMatch(pat, key_str) ||
          GlobMatch(pat, key_hex)) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      if (out_reason) {
        *out_reason = "key not in allowed_keys";
      }
      return false;
    }
  }
  if (!pol.require_ip_range.empty() && peer_ipv4_be != 0) {
    uint32_t net = 0, mask = 0;
    if (!ParseCidrV4(pol.require_ip_range, &net, &mask)) {
      if (out_reason) {
        *out_reason = "invalid require_ip_range";
      }
      return false;
    }
    if ((peer_ipv4_be & mask) != net) {
      if (out_reason) *out_reason = "peer ip out of range";
      return false;
    }
  }
  return true;
}

void HdPeersInit(HdPeerRegistry* reg,
                 const Key& relay_key,
                 HdEnrollMode mode) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  reg->relay_key = relay_key;
  reg->enroll_mode = mode;
  reg->peer_count = 0;
  for (int i = 0; i < kHdMaxPeers; ++i) {
    reg->peers[i] = HdPeer{};
  }
}

HdPeer* HdPeersLookup(HdPeerRegistry* reg,
                       const uint8_t* key) {
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (sodium_memcmp(p.key.data(), key, kKeySize) == 0) {
      return &p;
    }
  }
  return nullptr;
}

HdPeerPolicy* HdPeersLookupPolicy(HdPeerRegistry* reg,
                                   const uint8_t* key) {
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (sodium_memcmp(p.key.data(), key, kKeySize) == 0) {
      return &reg->policies[i];
    }
  }
  return nullptr;
}

bool HdPeersSetPolicy(HdPeerRegistry* reg,
                      const uint8_t* key,
                      const HdPeerPolicy& policy) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (sodium_memcmp(p.key.data(), key, kKeySize) == 0) {
      reg->policies[i] = policy;
      if (!reg->peer_policy_path.empty()) {
        HdPeerPolicySave(reg);
      }
      return true;
    }
  }
  return false;
}

bool HdPeersClearPolicy(HdPeerRegistry* reg,
                        const uint8_t* key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (sodium_memcmp(p.key.data(), key, kKeySize) == 0) {
      reg->policies[i] = HdPeerPolicy{};
      if (!reg->peer_policy_path.empty()) {
        HdPeerPolicySave(reg);
      }
      return true;
    }
  }
  return false;
}

HdPeer* HdPeersLookupById(HdPeerRegistry* reg,
                           uint16_t peer_id) {
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (p.peer_id == peer_id) return &p;
  }
  return nullptr;
}

HdPeer* HdPeersInsert(HdPeerRegistry* reg,
                       const Key& key,
                       int fd) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  // Check for duplicate.
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (sodium_memcmp(p.key.data(), key.data(),
                      kKeySize) == 0) {
      return &p;
    }
  }
  // Find an empty slot (occupied == 0 or tombstone == 2).
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied == 1) continue;
    p.key = key;
    p.fd = fd;
    p.peer_id = reg->next_peer_id++;
    if (reg->next_peer_id == 0) reg->next_peer_id = 1;
    p.state = (reg->enroll_mode == HdEnrollMode::kAutoApprove)
                  ? HdPeerState::kApproved
                  : HdPeerState::kPending;
    p.occupied = 1;
    p.rule_count = 0;
    p.enrolled_at = 0;
    for (int j = 0; j < kHdMaxForwardRules; ++j) {
      p.rules[j] = HdForwardRule{};
    }
    ++reg->peer_count;
    return &p;
  }
  return nullptr;  // Registry full.
}

bool HdPeersApprove(HdPeerRegistry* reg,
                    const uint8_t* key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, key);
  if (!p) return false;
  p->state = HdPeerState::kApproved;
  return true;
}

bool HdPeersDeny(HdPeerRegistry* reg,
                 const uint8_t* key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, key);
  if (!p) return false;
  p->state = HdPeerState::kDenied;
  return true;
}

void HdPeersRemove(HdPeerRegistry* reg,
                   const uint8_t* key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, key);
  if (!p) return;
  p->occupied = 2;  // Tombstone.
  --reg->peer_count;
}

bool HdPeersIsDenied(const HdPeerRegistry* reg,
                     const uint8_t* key) {
  for (const auto& k : reg->denylist) {
    if (sodium_memcmp(k.data(), key, kKeySize) == 0) {
      return true;
    }
  }
  return false;
}

void HdPeersRevoke(HdPeerRegistry* reg,
                   const uint8_t* key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  if (!HdPeersIsDenied(reg, key)) {
    Key k{};
    std::memcpy(k.data(), key, kKeySize);
    reg->denylist.push_back(k);
  }
  HdPeer* p = HdPeersLookup(reg, key);
  if (p) {
    p->occupied = 2;
    --reg->peer_count;
  }
  if (!reg->denylist_path.empty()) {
    HdDenylistSave(reg);
  }
}

bool HdDenylistLoad(HdPeerRegistry* reg) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  reg->denylist.clear();
  if (reg->denylist_path.empty()) return true;
  FILE* f = std::fopen(reg->denylist_path.c_str(), "rb");
  if (!f) {
    // Treat a missing file as an empty denylist.
    return true;
  }
  uint8_t buf[kKeySize];
  while (std::fread(buf, 1, kKeySize, f) ==
         static_cast<size_t>(kKeySize)) {
    Key k{};
    std::memcpy(k.data(), buf, kKeySize);
    reg->denylist.push_back(k);
  }
  std::fclose(f);
  return true;
}

namespace {

void WriteU16LE(FILE* f, uint16_t v) {
  uint8_t b[2] = {
      static_cast<uint8_t>(v),
      static_cast<uint8_t>(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

void WriteU32LE(FILE* f, uint32_t v) {
  uint8_t b[4] = {
      static_cast<uint8_t>(v),
      static_cast<uint8_t>(v >> 8),
      static_cast<uint8_t>(v >> 16),
      static_cast<uint8_t>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

bool ReadExact(FILE* f, uint8_t* buf, size_t n) {
  return std::fread(buf, 1, n, f) == n;
}

bool ReadU16LE(FILE* f, uint16_t* out) {
  uint8_t b[2];
  if (!ReadExact(f, b, 2)) return false;
  *out = static_cast<uint16_t>(b[0]) |
         (static_cast<uint16_t>(b[1]) << 8);
  return true;
}

bool ReadU32LE(FILE* f, uint32_t* out) {
  uint8_t b[4];
  if (!ReadExact(f, b, 4)) return false;
  *out = static_cast<uint32_t>(b[0]) |
         (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

}  // namespace

bool HdPeerPolicyLoad(HdPeerRegistry* reg) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  if (reg->peer_policy_path.empty()) return true;
  FILE* f =
      std::fopen(reg->peer_policy_path.c_str(), "rb");
  if (!f) return true;
  uint32_t count = 0;
  if (!ReadU32LE(f, &count)) {
    std::fclose(f);
    return true;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint8_t key[kKeySize];
    if (!ReadExact(f, key, kKeySize)) break;
    uint8_t hdr[3];
    if (!ReadExact(f, hdr, 3)) break;
    uint16_t tag_len = 0;
    if (!ReadU16LE(f, &tag_len)) break;
    std::string tag(tag_len, '\0');
    if (tag_len &&
        !ReadExact(f,
                   reinterpret_cast<uint8_t*>(tag.data()),
                   tag_len)) {
      break;
    }
    uint16_t reason_len = 0;
    if (!ReadU16LE(f, &reason_len)) break;
    std::string reason(reason_len, '\0');
    if (reason_len &&
        !ReadExact(
            f,
            reinterpret_cast<uint8_t*>(reason.data()),
            reason_len)) {
      break;
    }
    // Apply if the peer is currently registered.
    for (int j = 0; j < kHdMaxPeers; ++j) {
      HdPeer& p = reg->peers[j];
      if (p.occupied != 1) continue;
      if (sodium_memcmp(p.key.data(), key,
                        kKeySize) == 0) {
        HdPeerPolicy pol;
        pol.has_pin = hdr[0] != 0;
        pol.pinned_intent =
            static_cast<HdIntent>(hdr[1]);
        pol.override_client = hdr[2] != 0;
        pol.audit_tag = std::move(tag);
        pol.reason = std::move(reason);
        reg->policies[j] = std::move(pol);
        break;
      }
    }
  }
  std::fclose(f);
  return true;
}

bool HdPeerPolicySave(const HdPeerRegistry* reg) {
  if (reg->peer_policy_path.empty()) return false;
  std::string tmp = reg->peer_policy_path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  uint32_t count = 0;
  for (int i = 0; i < kHdMaxPeers; ++i) {
    const auto& p = reg->peers[i];
    const auto& pol = reg->policies[i];
    if (p.occupied != 1) continue;
    if (!pol.has_pin && !pol.override_client &&
        pol.audit_tag.empty() && pol.reason.empty()) {
      continue;
    }
    count++;
  }
  WriteU32LE(f, count);
  for (int i = 0; i < kHdMaxPeers; ++i) {
    const auto& p = reg->peers[i];
    const auto& pol = reg->policies[i];
    if (p.occupied != 1) continue;
    if (!pol.has_pin && !pol.override_client &&
        pol.audit_tag.empty() && pol.reason.empty()) {
      continue;
    }
    std::fwrite(p.key.data(), 1, kKeySize, f);
    uint8_t hdr[3] = {
        static_cast<uint8_t>(pol.has_pin ? 1 : 0),
        static_cast<uint8_t>(pol.pinned_intent),
        static_cast<uint8_t>(
            pol.override_client ? 1 : 0)};
    std::fwrite(hdr, 1, 3, f);
    WriteU16LE(
        f, static_cast<uint16_t>(pol.audit_tag.size()));
    std::fwrite(pol.audit_tag.data(), 1,
                pol.audit_tag.size(), f);
    WriteU16LE(
        f, static_cast<uint16_t>(pol.reason.size()));
    std::fwrite(pol.reason.data(), 1,
                pol.reason.size(), f);
  }
  std::fflush(f);
  fsync(fileno(f));
  std::fclose(f);
  if (std::rename(tmp.c_str(),
                  reg->peer_policy_path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

bool HdDenylistSave(const HdPeerRegistry* reg) {
  if (reg->denylist_path.empty()) return false;
  std::string tmp = reg->denylist_path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  for (const auto& k : reg->denylist) {
    if (std::fwrite(k.data(), 1, kKeySize, f) !=
        static_cast<size_t>(kKeySize)) {
      std::fclose(f);
      std::remove(tmp.c_str());
      return false;
    }
  }
  std::fflush(f);
  fsync(fileno(f));
  std::fclose(f);
  if (std::rename(tmp.c_str(),
                  reg->denylist_path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

bool HdPeersAddRule(HdPeerRegistry* reg,
                    const uint8_t* peer_key,
                    const Key& dst_key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, peer_key);
  if (!p) return false;
  if (p->rule_count >= kHdMaxForwardRules) return false;
  // Find an empty rule slot.
  for (int i = 0; i < kHdMaxForwardRules; ++i) {
    if (p->rules[i].occupied == 0) {
      p->rules[i].dst_key = dst_key;
      p->rules[i].occupied = 1;
      ++p->rule_count;
      return true;
    }
  }
  return false;
}

bool HdPeersRemoveRule(HdPeerRegistry* reg,
                       const uint8_t* peer_key,
                       const uint8_t* dst_key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, peer_key);
  if (!p) return false;
  for (int i = 0; i < kHdMaxForwardRules; ++i) {
    if (p->rules[i].occupied != 1) continue;
    if (sodium_memcmp(p->rules[i].dst_key.data(),
                      dst_key, kKeySize) == 0) {
      p->rules[i] = HdForwardRule{};
      --p->rule_count;
      return true;
    }
  }
  return false;
}

bool HdVerifyEnrollment(const HdPeerRegistry* reg,
                        const Key& client_key,
                        const uint8_t* hmac) {
  return crypto_auth_verify(
             hmac, client_key.data(), kKeySize,
             reg->relay_key.data()) == 0;
}

int HdPeersList(const HdPeerRegistry* reg,
                Key* out_keys,
                HdPeerState* out_states,
                int max_out) {
  int n = 0;
  for (int i = 0; i < kHdMaxPeers && n < max_out; ++i) {
    const HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    out_keys[n] = p.key;
    out_states[n] = p.state;
    ++n;
  }
  return n;
}

}  // namespace hyper_derp
