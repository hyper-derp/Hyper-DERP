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

}  // namespace hyper_derp
