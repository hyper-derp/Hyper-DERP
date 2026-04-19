/// @file test_hd_bridge.cc
/// @brief Unit tests for DERP-HD protocol bridge.

#include "hyper_derp/hd_bridge.h"

#include <cstring>

#include <gtest/gtest.h>

#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

TEST(HdBridgeTest, DerpToHdStripsKey) {
  // DERP SendPacket payload: [32B key][hello].
  uint8_t payload[kKeySize + 5];
  memset(payload, 0xAB, kKeySize);
  memcpy(payload + kKeySize, "hello", 5);

  uint8_t out[256];
  int len = BridgeDerpToHd(payload, sizeof(payload),
                           out, sizeof(out));
  ASSERT_GT(len, 0);
  EXPECT_EQ(len, kHdFrameHeaderSize + 5);
  EXPECT_EQ(HdReadFrameType(out), HdFrameType::kData);
  EXPECT_EQ(HdReadPayloadLen(out), 5u);
  EXPECT_EQ(memcmp(out + kHdFrameHeaderSize, "hello", 5),
            0);
}

TEST(HdBridgeTest, DerpToHdEmptyData) {
  // Payload is just 32 bytes (key only, no data).
  uint8_t payload[kKeySize];
  memset(payload, 0xCC, kKeySize);

  uint8_t out[256];
  int len = BridgeDerpToHd(payload, kKeySize,
                           out, sizeof(out));
  ASSERT_GT(len, 0);
  EXPECT_EQ(len, kHdFrameHeaderSize);
  EXPECT_EQ(HdReadFrameType(out), HdFrameType::kData);
  EXPECT_EQ(HdReadPayloadLen(out), 0u);
}

TEST(HdBridgeTest, DerpToHdTooShort) {
  uint8_t payload[16];
  memset(payload, 0, sizeof(payload));

  uint8_t out[256];
  int len = BridgeDerpToHd(payload, sizeof(payload),
                           out, sizeof(out));
  EXPECT_EQ(len, -1);
}

TEST(HdBridgeTest, DerpToHdBufferTooSmall) {
  uint8_t payload[kKeySize + 10];
  memset(payload, 0, sizeof(payload));

  // Output buffer too small for header + data.
  uint8_t out[4];
  int len = BridgeDerpToHd(payload, sizeof(payload),
                           out, sizeof(out));
  EXPECT_EQ(len, -1);
}

TEST(HdBridgeTest, HdToDerpPrependsKey) {
  const uint8_t data[] = "hello";
  int data_len = 5;
  Key src_key;
  memset(src_key.data(), 0x42, kKeySize);

  uint8_t out[256];
  int len = BridgeHdToDerp(data, data_len, src_key,
                           out, sizeof(out));
  ASSERT_GT(len, 0);
  EXPECT_EQ(len, kFrameHeaderSize + kKeySize + data_len);
  EXPECT_EQ(ReadFrameType(out), FrameType::kRecvPacket);
  EXPECT_EQ(ReadPayloadLen(out),
            static_cast<uint32_t>(kKeySize + data_len));
  EXPECT_EQ(
      memcmp(out + kFrameHeaderSize, src_key.data(),
             kKeySize),
      0);
  EXPECT_EQ(
      memcmp(out + kFrameHeaderSize + kKeySize, "hello", 5),
      0);
}

TEST(HdBridgeTest, HdToDerpEmptyData) {
  Key src_key;
  memset(src_key.data(), 0x99, kKeySize);

  uint8_t out[256];
  int len = BridgeHdToDerp(nullptr, 0, src_key,
                           out, sizeof(out));
  ASSERT_GT(len, 0);
  EXPECT_EQ(len, kFrameHeaderSize + kKeySize);
  EXPECT_EQ(ReadFrameType(out), FrameType::kRecvPacket);
  EXPECT_EQ(ReadPayloadLen(out),
            static_cast<uint32_t>(kKeySize));
  EXPECT_EQ(
      memcmp(out + kFrameHeaderSize, src_key.data(),
             kKeySize),
      0);
}

TEST(HdBridgeTest, HdToDerpBufferTooSmall) {
  const uint8_t data[] = "test";
  Key src_key{};

  uint8_t out[8];
  int len = BridgeHdToDerp(data, 4, src_key,
                           out, sizeof(out));
  EXPECT_EQ(len, -1);
}

TEST(HdBridgeTest, RoundtripDerpToHdToDerp) {
  // Original DERP SendPacket payload.
  const char msg[] = "roundtrip payload";
  int msg_len = static_cast<int>(strlen(msg));
  uint8_t derp_payload[kKeySize + 64];
  memset(derp_payload, 0xDD, kKeySize);  // dst_key
  memcpy(derp_payload + kKeySize, msg, msg_len);
  int derp_payload_len = kKeySize + msg_len;

  // Step 1: DERP → HD.
  uint8_t hd_frame[256];
  int hd_len = BridgeDerpToHd(derp_payload,
                               derp_payload_len,
                               hd_frame, sizeof(hd_frame));
  ASSERT_GT(hd_len, 0);
  ASSERT_EQ(hd_len, kHdFrameHeaderSize + msg_len);

  // Step 2: HD → DERP with a new src_key.
  Key src_key;
  memset(src_key.data(), 0xEE, kKeySize);
  const uint8_t* hd_data = hd_frame + kHdFrameHeaderSize;
  int hd_data_len = hd_len - kHdFrameHeaderSize;

  uint8_t derp_frame[256];
  int derp_len = BridgeHdToDerp(hd_data, hd_data_len,
                                src_key,
                                derp_frame,
                                sizeof(derp_frame));
  ASSERT_GT(derp_len, 0);

  // Verify DERP RecvPacket structure.
  EXPECT_EQ(ReadFrameType(derp_frame),
            FrameType::kRecvPacket);
  int recv_payload_len =
      static_cast<int>(ReadPayloadLen(derp_frame));
  EXPECT_EQ(recv_payload_len, kKeySize + msg_len);

  // Verify src_key is present.
  EXPECT_EQ(
      memcmp(derp_frame + kFrameHeaderSize,
             src_key.data(), kKeySize),
      0);

  // Verify original data is preserved.
  EXPECT_EQ(
      memcmp(derp_frame + kFrameHeaderSize + kKeySize,
             msg, msg_len),
      0);
}

}  // namespace hyper_derp
