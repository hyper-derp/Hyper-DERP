/// @file hd_bridge.cc
/// @brief DERP-to-HD and HD-to-DERP frame format bridge.

#include "hyper_derp/hd_bridge.h"

#include <cstring>

#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

int BridgeDerpToHd(const uint8_t* send_payload,
                   int send_len,
                   uint8_t* out, int out_size) {
  int data_len = send_len - kKeySize;
  if (data_len < 0) return -1;
  int frame_len = kHdFrameHeaderSize + data_len;
  if (frame_len > out_size) return -1;
  HdWriteFrameHeader(out, HdFrameType::kData,
                     static_cast<uint32_t>(data_len));
  if (data_len > 0) {
    memcpy(out + kHdFrameHeaderSize,
           send_payload + kKeySize, data_len);
  }
  return frame_len;
}

int BridgeHdToDerp(const uint8_t* hd_payload,
                   int hd_len,
                   const Key& src_key,
                   uint8_t* out, int out_size) {
  int payload_len = kKeySize + hd_len;
  int frame_len = kFrameHeaderSize + payload_len;
  if (frame_len > out_size) return -1;
  WriteFrameHeader(out, FrameType::kRecvPacket,
                   static_cast<uint32_t>(payload_len));
  memcpy(out + kFrameHeaderSize, src_key.data(), kKeySize);
  if (hd_len > 0) {
    memcpy(out + kFrameHeaderSize + kKeySize,
           hd_payload, hd_len);
  }
  return frame_len;
}

}  // namespace hyper_derp
