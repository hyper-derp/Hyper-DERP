// Fuzz target for protocol frame parsing functions.
// Tests ReadFrameType, ReadPayloadLen, IsValidPayloadLen,
// and BuildRecvPacket with arbitrary inputs.
#include "hyper_derp/protocol.h"

#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data,
                                       size_t size) {
  if (size < hyper_derp::kFrameHeaderSize) return 0;

  auto type = hyper_derp::ReadFrameType(data);
  auto len = hyper_derp::ReadPayloadLen(data);
  (void)type;
  (void)hyper_derp::IsValidPayloadLen(len);

  // Also exercise BuildRecvPacket with fuzzed data.
  if (size >= hyper_derp::kKeySize + 1) {
    auto key = hyper_derp::ToKey(data);
    const uint8_t* payload = data + hyper_derp::kKeySize;
    int payload_len =
        static_cast<int>(size - hyper_derp::kKeySize);
    if (payload_len > 0 &&
        payload_len <= hyper_derp::kMaxFramePayload) {
      uint8_t buf[hyper_derp::kMaxFrameSize];
      hyper_derp::BuildRecvPacket(
          buf, key, payload, payload_len);
    }
  }
  return 0;
}
