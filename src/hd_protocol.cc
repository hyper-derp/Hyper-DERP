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
