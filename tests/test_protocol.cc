/// @file test_protocol.cc
/// @brief Unit tests for DERP wire protocol codec.

#include "hyper_derp/protocol.h"

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

TEST(ProtocolTest, FrameHeaderRoundtrip) {
  uint8_t hdr[kFrameHeaderSize];
  WriteFrameHeader(hdr, FrameType::kSendPacket, 1234);
  EXPECT_EQ(ReadFrameType(hdr), FrameType::kSendPacket);
  EXPECT_EQ(ReadPayloadLen(hdr), 1234u);
}

TEST(ProtocolTest, FrameHeaderZeroLength) {
  uint8_t hdr[kFrameHeaderSize];
  WriteFrameHeader(hdr, FrameType::kKeepAlive, 0);
  EXPECT_EQ(ReadFrameType(hdr), FrameType::kKeepAlive);
  EXPECT_EQ(ReadPayloadLen(hdr), 0u);
}

TEST(ProtocolTest, FrameHeaderMaxLength) {
  uint8_t hdr[kFrameHeaderSize];
  uint32_t max_len = kMaxFramePayload;
  WriteFrameHeader(hdr, FrameType::kRecvPacket, max_len);
  EXPECT_EQ(ReadPayloadLen(hdr), max_len);
  EXPECT_TRUE(IsValidPayloadLen(max_len));
  EXPECT_FALSE(IsValidPayloadLen(max_len + 1));
}

TEST(ProtocolTest, FrameHeaderBigEndian) {
  uint8_t hdr[kFrameHeaderSize];
  WriteFrameHeader(hdr, FrameType::kSendPacket,
                   0x01020304);
  EXPECT_EQ(hdr[1], 0x01);
  EXPECT_EQ(hdr[2], 0x02);
  EXPECT_EQ(hdr[3], 0x03);
  EXPECT_EQ(hdr[4], 0x04);
}

TEST(ProtocolTest, BuildRecvPacket) {
  Key src_key;
  memset(src_key.data(), 0xAA, kKeySize);
  uint8_t data[] = {0x01, 0x02, 0x03};
  int data_len = 3;

  uint8_t buf[kFrameHeaderSize + kKeySize + 3];
  int n = BuildRecvPacket(buf, src_key, data, data_len);

  EXPECT_EQ(n, kFrameHeaderSize + kKeySize + data_len);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kRecvPacket);
  EXPECT_EQ(ReadPayloadLen(buf),
            static_cast<uint32_t>(kKeySize + data_len));
  EXPECT_EQ(
      memcmp(buf + kFrameHeaderSize, src_key.data(),
             kKeySize),
      0);
  EXPECT_EQ(
      memcmp(buf + kFrameHeaderSize + kKeySize, data,
             data_len),
      0);
}

TEST(ProtocolTest, BuildPeerGone) {
  Key key;
  memset(key.data(), 0xBB, kKeySize);

  uint8_t buf[kFrameHeaderSize + kKeySize + 1];
  int n = BuildPeerGone(buf, key,
                        PeerGoneReason::kNotHere);

  EXPECT_EQ(n, kFrameHeaderSize + kKeySize + 1);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kPeerGone);
  EXPECT_EQ(ReadPayloadLen(buf),
            static_cast<uint32_t>(kKeySize + 1));
  EXPECT_EQ(buf[kFrameHeaderSize + kKeySize],
            static_cast<uint8_t>(
                PeerGoneReason::kNotHere));
}

TEST(ProtocolTest, BuildPeerPresent) {
  Key key;
  memset(key.data(), 0xCC, kKeySize);

  uint8_t buf[kFrameHeaderSize + kKeySize];
  int n = BuildPeerPresent(buf, key);

  EXPECT_EQ(n, kFrameHeaderSize + kKeySize);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kPeerPresent);
  EXPECT_EQ(ReadPayloadLen(buf),
            static_cast<uint32_t>(kKeySize));
}

TEST(ProtocolTest, BuildKeepAlive) {
  uint8_t buf[kFrameHeaderSize];
  int n = BuildKeepAlive(buf);

  EXPECT_EQ(n, kFrameHeaderSize);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kKeepAlive);
  EXPECT_EQ(ReadPayloadLen(buf), 0u);
}

TEST(ProtocolTest, BuildPong) {
  uint8_t ping_data[kPingDataSize] = {
      1, 2, 3, 4, 5, 6, 7, 8};

  uint8_t buf[kFrameHeaderSize + kPingDataSize];
  int n = BuildPong(buf, ping_data);

  EXPECT_EQ(n, kFrameHeaderSize + kPingDataSize);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kPong);
  EXPECT_EQ(ReadPayloadLen(buf),
            static_cast<uint32_t>(kPingDataSize));
  EXPECT_EQ(memcmp(buf + kFrameHeaderSize, ping_data,
                   kPingDataSize),
            0);
}

TEST(ProtocolTest, BuildServerKey) {
  Key key;
  memset(key.data(), 0xDD, kKeySize);

  uint8_t buf[kFrameHeaderSize + kKeySize];
  int n = BuildServerKey(buf, key);

  EXPECT_EQ(n, kFrameHeaderSize + kKeySize);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kServerKey);
  EXPECT_EQ(ReadPayloadLen(buf),
            static_cast<uint32_t>(kKeySize));
  EXPECT_EQ(
      memcmp(buf + kFrameHeaderSize, key.data(),
             kKeySize),
      0);
}

TEST(ProtocolTest, BuildHealth) {
  const char* msg = "all good";
  int msg_len = strlen(msg);

  uint8_t buf[kFrameHeaderSize + 64];
  int n = BuildHealth(
      buf, reinterpret_cast<const uint8_t*>(msg),
      msg_len);

  EXPECT_EQ(n, kFrameHeaderSize + msg_len);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kHealth);
  EXPECT_EQ(ReadPayloadLen(buf),
            static_cast<uint32_t>(msg_len));
  EXPECT_EQ(
      memcmp(buf + kFrameHeaderSize, msg, msg_len), 0);
}

TEST(ProtocolTest, BuildRestarting) {
  uint8_t buf[kFrameHeaderSize + 8];
  int n = BuildRestarting(buf, 5000, 30000);

  EXPECT_EQ(n, kFrameHeaderSize + 8);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kRestarting);
  EXPECT_EQ(ReadPayloadLen(buf), 8u);

  // Verify big-endian encoding of 5000 and 30000.
  uint32_t reconnect =
      (static_cast<uint32_t>(buf[5]) << 24) |
      (static_cast<uint32_t>(buf[6]) << 16) |
      (static_cast<uint32_t>(buf[7]) << 8) |
      static_cast<uint32_t>(buf[8]);
  uint32_t try_for =
      (static_cast<uint32_t>(buf[9]) << 24) |
      (static_cast<uint32_t>(buf[10]) << 16) |
      (static_cast<uint32_t>(buf[11]) << 8) |
      static_cast<uint32_t>(buf[12]);
  EXPECT_EQ(reconnect, 5000u);
  EXPECT_EQ(try_for, 30000u);
}

TEST(ProtocolTest, SendPacketAccessors) {
  uint8_t payload[kKeySize + 4];
  memset(payload, 0xEE, kKeySize);
  payload[kKeySize] = 0x01;
  payload[kKeySize + 1] = 0x02;
  payload[kKeySize + 2] = 0x03;
  payload[kKeySize + 3] = 0x04;

  EXPECT_EQ(SendPacketDstKey(payload), payload);
  EXPECT_EQ(SendPacketData(payload),
            payload + kKeySize);
  EXPECT_EQ(SendPacketDataLen(kKeySize + 4), 4);
}

TEST(ProtocolTest, AllFrameTypesHaveDistinctValues) {
  // Verify no accidental collisions.
  uint8_t types[] = {
      static_cast<uint8_t>(FrameType::kServerKey),
      static_cast<uint8_t>(FrameType::kClientInfo),
      static_cast<uint8_t>(FrameType::kSendPacket),
      static_cast<uint8_t>(FrameType::kRecvPacket),
      static_cast<uint8_t>(FrameType::kKeepAlive),
      static_cast<uint8_t>(FrameType::kNotePreferred),
      static_cast<uint8_t>(FrameType::kPeerGone),
      static_cast<uint8_t>(FrameType::kPeerPresent),
      static_cast<uint8_t>(FrameType::kWatchConns),
      static_cast<uint8_t>(FrameType::kClosePeer),
      static_cast<uint8_t>(FrameType::kPing),
      static_cast<uint8_t>(FrameType::kPong),
      static_cast<uint8_t>(FrameType::kHealth),
      static_cast<uint8_t>(FrameType::kRestarting),
      static_cast<uint8_t>(FrameType::kForwardPacket),
  };
  int count = sizeof(types) / sizeof(types[0]);
  for (int i = 0; i < count; i++) {
    for (int j = i + 1; j < count; j++) {
      EXPECT_NE(types[i], types[j])
          << "Frame type collision at index " << i
          << " and " << j;
    }
  }
}

}  // namespace hyper_derp
