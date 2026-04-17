/// @file test_hd_protocol.cc
/// @brief Unit tests for HD wire protocol codec.

#include "hyper_derp/hd_protocol.h"

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

TEST(HdProtocolTest, HeaderRoundtrip) {
  uint8_t hdr[kHdFrameHeaderSize];
  HdWriteFrameHeader(hdr, HdFrameType::kData, 1234);
  EXPECT_EQ(HdReadFrameType(hdr), HdFrameType::kData);
  EXPECT_EQ(HdReadPayloadLen(hdr), 1234u);
}

TEST(HdProtocolTest, ZeroPayload) {
  uint8_t hdr[kHdFrameHeaderSize];
  HdWriteFrameHeader(hdr, HdFrameType::kPing, 0);
  EXPECT_EQ(HdReadFrameType(hdr), HdFrameType::kPing);
  EXPECT_EQ(HdReadPayloadLen(hdr), 0u);
}

TEST(HdProtocolTest, MaxPayload) {
  uint8_t hdr[kHdFrameHeaderSize];
  uint32_t max_len = kHdMaxFramePayload;
  HdWriteFrameHeader(hdr, HdFrameType::kData, max_len);
  EXPECT_EQ(HdReadPayloadLen(hdr), max_len);
  EXPECT_TRUE(HdIsValidPayloadLen(max_len));
  EXPECT_FALSE(HdIsValidPayloadLen(max_len + 1));
}

TEST(HdProtocolTest, BigEndianOrder) {
  uint8_t hdr[kHdFrameHeaderSize];
  HdWriteFrameHeader(hdr, HdFrameType::kData, 0x010203);
  EXPECT_EQ(hdr[1], 0x01);
  EXPECT_EQ(hdr[2], 0x02);
  EXPECT_EQ(hdr[3], 0x03);
}

TEST(HdProtocolTest, AllFrameTypesDistinct) {
  uint8_t types[] = {
      static_cast<uint8_t>(HdFrameType::kData),
      static_cast<uint8_t>(HdFrameType::kPing),
      static_cast<uint8_t>(HdFrameType::kPong),
      static_cast<uint8_t>(HdFrameType::kEnroll),
      static_cast<uint8_t>(HdFrameType::kApproved),
      static_cast<uint8_t>(HdFrameType::kDenied),
      static_cast<uint8_t>(HdFrameType::kPeerInfo),
      static_cast<uint8_t>(HdFrameType::kPeerGone),
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

TEST(HdProtocolTest, BuildPing) {
  uint8_t ping_data[kHdPingDataSize] = {
      1, 2, 3, 4, 5, 6, 7, 8};

  uint8_t buf[kHdFrameHeaderSize + kHdPingDataSize];
  int n = HdBuildPing(buf, ping_data);

  EXPECT_EQ(n, kHdFrameHeaderSize + kHdPingDataSize);
  EXPECT_EQ(HdReadFrameType(buf), HdFrameType::kPing);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(kHdPingDataSize));
  EXPECT_EQ(memcmp(buf + kHdFrameHeaderSize, ping_data,
                   kHdPingDataSize),
            0);
}

TEST(HdProtocolTest, BuildPong) {
  uint8_t ping_data[kHdPingDataSize] = {
      8, 7, 6, 5, 4, 3, 2, 1};

  uint8_t buf[kHdFrameHeaderSize + kHdPingDataSize];
  int n = HdBuildPong(buf, ping_data);

  EXPECT_EQ(n, kHdFrameHeaderSize + kHdPingDataSize);
  EXPECT_EQ(HdReadFrameType(buf), HdFrameType::kPong);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(kHdPingDataSize));
  EXPECT_EQ(memcmp(buf + kHdFrameHeaderSize, ping_data,
                   kHdPingDataSize),
            0);
}

TEST(HdProtocolTest, BuildEnroll) {
  Key key;
  memset(key.data(), 0xAA, kKeySize);
  uint8_t hmac[] = {0x01, 0x02, 0x03, 0x04};
  int hmac_len = 4;

  uint8_t buf[kHdFrameHeaderSize + kKeySize + 4];
  int n = HdBuildEnroll(buf, key, hmac, hmac_len);

  EXPECT_EQ(n, kHdFrameHeaderSize + kKeySize + hmac_len);
  EXPECT_EQ(HdReadFrameType(buf), HdFrameType::kEnroll);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(kKeySize + hmac_len));
  EXPECT_EQ(
      memcmp(buf + kHdFrameHeaderSize, key.data(),
             kKeySize),
      0);
  EXPECT_EQ(
      memcmp(buf + kHdFrameHeaderSize + kKeySize, hmac,
             hmac_len),
      0);
}

TEST(HdProtocolTest, BuildApproved) {
  Key key;
  memset(key.data(), 0xBB, kKeySize);

  uint8_t buf[kHdFrameHeaderSize + kKeySize];
  int n = HdBuildApproved(buf, key);

  EXPECT_EQ(n, kHdFrameHeaderSize + kKeySize);
  EXPECT_EQ(HdReadFrameType(buf), HdFrameType::kApproved);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(kKeySize));
  EXPECT_EQ(
      memcmp(buf + kHdFrameHeaderSize, key.data(),
             kKeySize),
      0);
}

TEST(HdProtocolTest, BuildDenied) {
  const char* msg = "not authorized";
  int msg_len = strlen(msg);
  uint8_t reason = 0x03;

  uint8_t buf[kHdFrameHeaderSize + 1 + 64];
  int n = HdBuildDenied(buf, reason, msg, msg_len);

  EXPECT_EQ(n, kHdFrameHeaderSize + 1 + msg_len);
  EXPECT_EQ(HdReadFrameType(buf), HdFrameType::kDenied);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(1 + msg_len));
  EXPECT_EQ(buf[kHdFrameHeaderSize], reason);
  EXPECT_EQ(
      memcmp(buf + kHdFrameHeaderSize + 1, msg, msg_len),
      0);
}

TEST(HdProtocolTest, BuildPeerGone) {
  Key key;
  memset(key.data(), 0xCC, kKeySize);
  uint8_t reason = 0x01;

  uint8_t buf[kHdFrameHeaderSize + kKeySize + 1];
  int n = HdBuildPeerGone(buf, key, reason);

  EXPECT_EQ(n, kHdFrameHeaderSize + kKeySize + 1);
  EXPECT_EQ(HdReadFrameType(buf), HdFrameType::kPeerGone);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(kKeySize + 1));
  EXPECT_EQ(
      memcmp(buf + kHdFrameHeaderSize, key.data(),
             kKeySize),
      0);
  EXPECT_EQ(buf[kHdFrameHeaderSize + kKeySize], reason);
}

}  // namespace hyper_derp
