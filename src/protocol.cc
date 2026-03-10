/// @file protocol.cc
/// @brief DERP frame building functions.

#include "hyper_derp/protocol.h"

#include <cstring>

namespace hyper_derp {

int BuildRecvPacket(uint8_t* buf,
                    const uint8_t* src_key,
                    const uint8_t* data,
                    int data_len) {
  int payload_len = kKeySize + data_len;
  WriteFrameHeader(buf, FrameType::kRecvPacket,
                   static_cast<uint32_t>(payload_len));
  memcpy(buf + kFrameHeaderSize, src_key, kKeySize);
  memcpy(buf + kFrameHeaderSize + kKeySize,
         data, data_len);
  return kFrameHeaderSize + payload_len;
}

int BuildPeerGone(uint8_t* buf,
                  const uint8_t* peer_key,
                  PeerGoneReason reason) {
  int payload_len = kKeySize + 1;
  WriteFrameHeader(buf, FrameType::kPeerGone,
                   static_cast<uint32_t>(payload_len));
  memcpy(buf + kFrameHeaderSize, peer_key, kKeySize);
  buf[kFrameHeaderSize + kKeySize] =
      static_cast<uint8_t>(reason);
  return kFrameHeaderSize + payload_len;
}

int BuildPeerPresent(uint8_t* buf,
                     const uint8_t* peer_key) {
  WriteFrameHeader(buf, FrameType::kPeerPresent,
                   static_cast<uint32_t>(kKeySize));
  memcpy(buf + kFrameHeaderSize, peer_key, kKeySize);
  return kFrameHeaderSize + kKeySize;
}

int BuildKeepAlive(uint8_t* buf) {
  WriteFrameHeader(buf, FrameType::kKeepAlive, 0);
  return kFrameHeaderSize;
}

int BuildPong(uint8_t* buf, const uint8_t* ping_data) {
  WriteFrameHeader(buf, FrameType::kPong,
                   static_cast<uint32_t>(kPingDataSize));
  memcpy(buf + kFrameHeaderSize, ping_data, kPingDataSize);
  return kFrameHeaderSize + kPingDataSize;
}

int BuildHealth(uint8_t* buf,
                const uint8_t* msg, int msg_len) {
  WriteFrameHeader(buf, FrameType::kHealth,
                   static_cast<uint32_t>(msg_len));
  memcpy(buf + kFrameHeaderSize, msg, msg_len);
  return kFrameHeaderSize + msg_len;
}

int BuildServerKey(uint8_t* buf,
                   const uint8_t* server_key) {
  WriteFrameHeader(buf, FrameType::kServerKey,
                   static_cast<uint32_t>(kKeySize));
  memcpy(buf + kFrameHeaderSize, server_key, kKeySize);
  return kFrameHeaderSize + kKeySize;
}

int BuildRestarting(uint8_t* buf,
                    uint32_t reconnect_in_ms,
                    uint32_t try_for_ms) {
  WriteFrameHeader(buf, FrameType::kRestarting, 8);
  buf[kFrameHeaderSize + 0] =
      static_cast<uint8_t>(reconnect_in_ms >> 24);
  buf[kFrameHeaderSize + 1] =
      static_cast<uint8_t>(reconnect_in_ms >> 16);
  buf[kFrameHeaderSize + 2] =
      static_cast<uint8_t>(reconnect_in_ms >> 8);
  buf[kFrameHeaderSize + 3] =
      static_cast<uint8_t>(reconnect_in_ms);
  buf[kFrameHeaderSize + 4] =
      static_cast<uint8_t>(try_for_ms >> 24);
  buf[kFrameHeaderSize + 5] =
      static_cast<uint8_t>(try_for_ms >> 16);
  buf[kFrameHeaderSize + 6] =
      static_cast<uint8_t>(try_for_ms >> 8);
  buf[kFrameHeaderSize + 7] =
      static_cast<uint8_t>(try_for_ms);
  return kFrameHeaderSize + 8;
}

}  // namespace hyper_derp
