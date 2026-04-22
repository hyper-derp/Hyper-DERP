/// @file hd_protocol.cc
/// @brief HD frame building functions.

#include "hyper_derp/hd_protocol.h"

#include <cstring>

namespace hyper_derp {

int HdBuildDataHeader(uint8_t* buf, int payload_len) {
  HdWriteFrameHeader(buf, HdFrameType::kData,
                     static_cast<uint32_t>(payload_len));
  return kHdFrameHeaderSize;
}

int HdBuildPing(uint8_t* buf, const uint8_t* data) {
  HdWriteFrameHeader(buf, HdFrameType::kPing,
                     static_cast<uint32_t>(kHdPingDataSize));
  std::memcpy(buf + kHdFrameHeaderSize,
              data, kHdPingDataSize);
  return kHdFrameHeaderSize + kHdPingDataSize;
}

int HdBuildPong(uint8_t* buf, const uint8_t* data) {
  HdWriteFrameHeader(buf, HdFrameType::kPong,
                     static_cast<uint32_t>(kHdPingDataSize));
  std::memcpy(buf + kHdFrameHeaderSize,
              data, kHdPingDataSize);
  return kHdFrameHeaderSize + kHdPingDataSize;
}

int HdBuildEnroll(uint8_t* buf,
                  const Key& client_key,
                  const uint8_t* hmac,
                  int hmac_len) {
  int payload_len = kKeySize + hmac_len;
  HdWriteFrameHeader(buf, HdFrameType::kEnroll,
                     static_cast<uint32_t>(payload_len));
  std::memcpy(buf + kHdFrameHeaderSize,
              client_key.data(), kKeySize);
  std::memcpy(buf + kHdFrameHeaderSize + kKeySize,
              hmac, hmac_len);
  return kHdFrameHeaderSize + payload_len;
}

int HdBuildApproved(uint8_t* buf,
                    const Key& client_key) {
  HdWriteFrameHeader(buf, HdFrameType::kApproved,
                     static_cast<uint32_t>(kKeySize));
  std::memcpy(buf + kHdFrameHeaderSize,
              client_key.data(), kKeySize);
  return kHdFrameHeaderSize + kKeySize;
}

int HdBuildDenied(uint8_t* buf,
                  uint8_t reason,
                  const char* msg,
                  int msg_len) {
  int payload_len = 1 + msg_len;
  HdWriteFrameHeader(buf, HdFrameType::kDenied,
                     static_cast<uint32_t>(payload_len));
  buf[kHdFrameHeaderSize] = reason;
  std::memcpy(buf + kHdFrameHeaderSize + 1,
              msg, msg_len);
  return kHdFrameHeaderSize + payload_len;
}

int HdBuildPeerGone(uint8_t* buf,
                    const Key& peer_key,
                    uint8_t reason) {
  int payload_len = kKeySize + 1;
  HdWriteFrameHeader(buf, HdFrameType::kPeerGone,
                     static_cast<uint32_t>(payload_len));
  std::memcpy(buf + kHdFrameHeaderSize,
              peer_key.data(), kKeySize);
  buf[kHdFrameHeaderSize + kKeySize] = reason;
  return kHdFrameHeaderSize + payload_len;
}

int HdBuildPeerInfo(uint8_t* buf,
                    const Key& peer_key,
                    const uint8_t* candidate_data,
                    int candidate_len) {
  int payload_len = kKeySize + candidate_len;
  HdWriteFrameHeader(buf, HdFrameType::kPeerInfo,
                     static_cast<uint32_t>(payload_len));
  std::memcpy(buf + kHdFrameHeaderSize,
              peer_key.data(), kKeySize);
  if (candidate_len > 0) {
    std::memcpy(buf + kHdFrameHeaderSize + kKeySize,
                candidate_data, candidate_len);
  }
  return kHdFrameHeaderSize + payload_len;
}

int HdBuildMeshDataHeader(uint8_t* buf,
                          uint16_t dst_peer_id,
                          int payload_len) {
  // Total payload = 2B dst + actual payload.
  HdWriteFrameHeader(buf, HdFrameType::kMeshData,
      static_cast<uint32_t>(kHdMeshDstSize + payload_len));
  buf[kHdFrameHeaderSize] =
      static_cast<uint8_t>(dst_peer_id >> 8);
  buf[kHdFrameHeaderSize + 1] =
      static_cast<uint8_t>(dst_peer_id);
  return kHdFrameHeaderSize + kHdMeshDstSize;
}

int HdBuildFleetDataHeader(uint8_t* buf,
                           uint16_t dst_relay_id,
                           uint16_t dst_peer_id,
                           int payload_len) {
  HdWriteFrameHeader(buf, HdFrameType::kFleetData,
      static_cast<uint32_t>(
          kHdFleetDstSize + payload_len));
  buf[kHdFrameHeaderSize] =
      static_cast<uint8_t>(dst_relay_id >> 8);
  buf[kHdFrameHeaderSize + 1] =
      static_cast<uint8_t>(dst_relay_id);
  buf[kHdFrameHeaderSize + 2] =
      static_cast<uint8_t>(dst_peer_id >> 8);
  buf[kHdFrameHeaderSize + 3] =
      static_cast<uint8_t>(dst_peer_id);
  return kHdFrameHeaderSize + kHdFleetDstSize;
}

int HdBuildRouteAnnounce(uint8_t* buf, int buf_size,
                         const uint16_t* relay_ids,
                         const uint8_t* hops,
                         int count) {
  int payload_len = count * kHdRouteEntrySize;
  int total = kHdFrameHeaderSize + payload_len;
  if (total > buf_size) return -1;
  HdWriteFrameHeader(buf, HdFrameType::kRouteAnnounce,
                     static_cast<uint32_t>(payload_len));
  uint8_t* p = buf + kHdFrameHeaderSize;
  for (int i = 0; i < count; i++) {
    p[0] = static_cast<uint8_t>(relay_ids[i] >> 8);
    p[1] = static_cast<uint8_t>(relay_ids[i]);
    p[2] = hops[i];
    p += kHdRouteEntrySize;
  }
  return total;
}

int HdParseRouteAnnounce(const uint8_t* payload,
                         int payload_len,
                         uint16_t* out_ids,
                         uint8_t* out_hops,
                         int max_out) {
  int entry_count = payload_len / kHdRouteEntrySize;
  if (entry_count > max_out) entry_count = max_out;
  const uint8_t* p = payload;
  for (int i = 0; i < entry_count; i++) {
    out_ids[i] = static_cast<uint16_t>(
        (p[0] << 8) | p[1]);
    out_hops[i] = p[2];
    p += kHdRouteEntrySize;
  }
  return entry_count;
}

namespace {

inline void WriteU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

inline uint16_t ReadU16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline void WriteU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = static_cast<uint8_t>(v >> (56 - i * 8));
  }
}

inline uint64_t ReadU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

}  // namespace

// -- Routing policy frames ---------------------------------------------------

int HdBuildOpenConnection(uint8_t* buf,
                          uint16_t target_peer_id,
                          uint16_t target_relay_id,
                          HdIntent intent,
                          uint8_t flags,
                          uint64_t correlation_id) {
  HdWriteFrameHeader(buf, HdFrameType::kOpenConnection,
                     kHdOpenConnSize);
  uint8_t* p = buf + kHdFrameHeaderSize;
  WriteU16(p, target_peer_id);
  WriteU16(p + 2, target_relay_id);
  p[4] = static_cast<uint8_t>(intent);
  p[5] = flags;
  WriteU64(p + 6, correlation_id);
  return kHdFrameHeaderSize + kHdOpenConnSize;
}

bool HdParseOpenConnection(const uint8_t* payload,
                           int payload_len,
                           HdOpenConnection* out) {
  if (payload_len != kHdOpenConnSize) return false;
  out->target_peer_id = ReadU16(payload);
  out->target_relay_id = ReadU16(payload + 2);
  out->intent = static_cast<HdIntent>(payload[4]);
  out->flags = payload[5];
  out->correlation_id = ReadU64(payload + 6);
  return true;
}

int HdBuildOpenConnectionResult(uint8_t* buf,
                                int buf_size,
                                uint64_t correlation_id,
                                HdConnMode mode,
                                HdDenyReason deny_reason,
                                uint8_t sub_reason,
                                const uint16_t* relay_path,
                                int relay_path_len,
                                const char* endpoint_hint,
                                int endpoint_hint_len) {
  if (relay_path_len < 0 ||
      relay_path_len > kHdMaxRelayPath) {
    return -1;
  }
  if (endpoint_hint_len < 0 ||
      endpoint_hint_len > kHdMaxEndpointHint) {
    return -1;
  }
  int payload_len = kHdOpenResultMinSize +
                    relay_path_len * 2 +
                    endpoint_hint_len;
  int total = kHdFrameHeaderSize + payload_len;
  if (total > buf_size) return -1;
  HdWriteFrameHeader(buf,
                     HdFrameType::kOpenConnectionResult,
                     static_cast<uint32_t>(payload_len));
  uint8_t* p = buf + kHdFrameHeaderSize;
  WriteU64(p, correlation_id);
  p[8] = static_cast<uint8_t>(mode);
  WriteU16(p + 9, static_cast<uint16_t>(deny_reason));
  p[11] = sub_reason;
  WriteU16(p + 12,
           static_cast<uint16_t>(relay_path_len));
  uint8_t* q = p + 14;
  for (int i = 0; i < relay_path_len; i++) {
    WriteU16(q, relay_path[i]);
    q += 2;
  }
  WriteU16(q, static_cast<uint16_t>(endpoint_hint_len));
  q += 2;
  if (endpoint_hint_len > 0) {
    std::memcpy(q, endpoint_hint, endpoint_hint_len);
  }
  return total;
}

bool HdParseOpenConnectionResult(
    const uint8_t* payload,
    int payload_len,
    HdOpenConnectionResult* out) {
  if (payload_len < kHdOpenResultMinSize) return false;
  out->correlation_id = ReadU64(payload);
  out->mode = static_cast<HdConnMode>(payload[8]);
  out->deny_reason = static_cast<HdDenyReason>(
      ReadU16(payload + 9));
  out->sub_reason = payload[11];
  int path_len = ReadU16(payload + 12);
  if (path_len > kHdMaxRelayPath) return false;
  // Minimum covers everything up through hint_len.
  int fixed = kHdOpenResultMinSize + path_len * 2;
  if (payload_len < fixed) return false;
  for (int i = 0; i < path_len; i++) {
    out->relay_path[i] = ReadU16(payload + 14 + i * 2);
  }
  out->relay_path_len = path_len;
  const uint8_t* hint_len_off =
      payload + 14 + path_len * 2;
  int hint_len = ReadU16(hint_len_off);
  if (hint_len > kHdMaxEndpointHint) return false;
  if (payload_len != fixed + hint_len) return false;
  if (hint_len > 0) {
    std::memcpy(out->endpoint_hint, hint_len_off + 2,
                hint_len);
  }
  out->endpoint_hint[hint_len] = '\0';
  out->endpoint_hint_len = hint_len;
  return true;
}

int HdBuildIncomingConnection(uint8_t* buf,
                              const Key& initiator_key,
                              uint16_t initiator_peer_id,
                              HdIntent intent,
                              uint8_t flags,
                              uint64_t correlation_id) {
  HdWriteFrameHeader(buf, HdFrameType::kIncomingConnection,
                     kHdIncomingConnSize);
  uint8_t* p = buf + kHdFrameHeaderSize;
  std::memcpy(p, initiator_key.data(), kKeySize);
  WriteU16(p + 32, initiator_peer_id);
  p[34] = static_cast<uint8_t>(intent);
  p[35] = flags;
  WriteU64(p + 36, correlation_id);
  return kHdFrameHeaderSize + kHdIncomingConnSize;
}

bool HdParseIncomingConnection(const uint8_t* payload,
                               int payload_len,
                               HdIncomingConnection* out) {
  if (payload_len != kHdIncomingConnSize) return false;
  std::memcpy(out->initiator_key.data(), payload,
              kKeySize);
  out->initiator_peer_id = ReadU16(payload + 32);
  out->intent = static_cast<HdIntent>(payload[34]);
  out->flags = payload[35];
  out->correlation_id = ReadU64(payload + 36);
  return true;
}

int HdBuildIncomingConnResponse(uint8_t* buf,
                                uint64_t correlation_id,
                                HdIntent intent,
                                uint8_t flags,
                                uint8_t accept) {
  HdWriteFrameHeader(buf,
                     HdFrameType::kIncomingConnResponse,
                     kHdIncomingRespSize);
  uint8_t* p = buf + kHdFrameHeaderSize;
  WriteU64(p, correlation_id);
  p[8] = static_cast<uint8_t>(intent);
  p[9] = flags;
  p[10] = accept;
  return kHdFrameHeaderSize + kHdIncomingRespSize;
}

bool HdParseIncomingConnResponse(
    const uint8_t* payload,
    int payload_len,
    HdIncomingConnResponse* out) {
  if (payload_len != kHdIncomingRespSize) return false;
  out->correlation_id = ReadU64(payload);
  out->intent = static_cast<HdIntent>(payload[8]);
  out->flags = payload[9];
  out->accept = payload[10];
  return true;
}

int HdBuildIncomingConnResult(uint8_t* buf,
                              uint64_t correlation_id,
                              HdConnMode mode,
                              HdDenyReason deny_reason,
                              uint8_t sub_reason) {
  HdWriteFrameHeader(buf,
                     HdFrameType::kIncomingConnResult,
                     kHdIncomingResultSize);
  uint8_t* p = buf + kHdFrameHeaderSize;
  WriteU64(p, correlation_id);
  p[8] = static_cast<uint8_t>(mode);
  WriteU16(p + 9, static_cast<uint16_t>(deny_reason));
  p[11] = sub_reason;
  return kHdFrameHeaderSize + kHdIncomingResultSize;
}

bool HdParseIncomingConnResult(const uint8_t* payload,
                               int payload_len,
                               HdIncomingConnResult* out) {
  if (payload_len != kHdIncomingResultSize) return false;
  out->correlation_id = ReadU64(payload);
  out->mode = static_cast<HdConnMode>(payload[8]);
  out->deny_reason = static_cast<HdDenyReason>(
      ReadU16(payload + 9));
  out->sub_reason = payload[11];
  return true;
}

int HdBuildRedirect(uint8_t* buf,
                    HdRedirectReason reason,
                    const char* target_url,
                    int url_len) {
  if (url_len < 0 || url_len > kHdRedirectMaxUrl) {
    return -1;
  }
  int payload_len = 1 + url_len;
  HdWriteFrameHeader(buf, HdFrameType::kRedirect,
                     static_cast<uint32_t>(payload_len));
  buf[kHdFrameHeaderSize] = static_cast<uint8_t>(reason);
  if (url_len > 0) {
    std::memcpy(buf + kHdFrameHeaderSize + 1,
                target_url, url_len);
  }
  return kHdFrameHeaderSize + payload_len;
}

int HdParseRedirect(const uint8_t* payload,
                    int payload_len,
                    HdRedirectReason* out_reason,
                    char* out_url,
                    int out_url_size) {
  if (payload_len < 1) return -1;
  int url_len = payload_len - 1;
  if (url_len > kHdRedirectMaxUrl) return -1;
  if (out_url_size < url_len + 1) return -1;
  *out_reason = static_cast<HdRedirectReason>(payload[0]);
  if (url_len > 0) {
    std::memcpy(out_url, payload + 1, url_len);
  }
  out_url[url_len] = '\0';
  return url_len;
}

int HdBuildRelayEnroll(uint8_t* buf,
                       const Key& client_key,
                       const uint8_t* hmac,
                       int hmac_len,
                       uint16_t relay_id) {
  int payload_len =
      kKeySize + hmac_len + kHdRelayExtSize;
  HdWriteFrameHeader(buf, HdFrameType::kEnroll,
                     static_cast<uint32_t>(payload_len));
  int off = kHdFrameHeaderSize;
  std::memcpy(buf + off, client_key.data(), kKeySize);
  off += kKeySize;
  std::memcpy(buf + off, hmac, hmac_len);
  off += hmac_len;
  buf[off] = static_cast<uint8_t>(relay_id >> 8);
  buf[off + 1] = static_cast<uint8_t>(relay_id);
  off += 2;
  std::memcpy(buf + off, kHdRelayMagic, 5);
  off += 5;
  return kHdFrameHeaderSize + payload_len;
}

}  // namespace hyper_derp
