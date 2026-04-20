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
      static_cast<uint8_t>(HdFrameType::kMeshData),
      static_cast<uint8_t>(HdFrameType::kFleetData),
      static_cast<uint8_t>(HdFrameType::kEnroll),
      static_cast<uint8_t>(HdFrameType::kApproved),
      static_cast<uint8_t>(HdFrameType::kDenied),
      static_cast<uint8_t>(HdFrameType::kPeerInfo),
      static_cast<uint8_t>(HdFrameType::kPeerGone),
      static_cast<uint8_t>(HdFrameType::kRedirect),
      static_cast<uint8_t>(HdFrameType::kRouteAnnounce),
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

TEST(HdProtocolTest, BuildMeshDataHeader) {
  uint8_t buf[kHdFrameHeaderSize + kHdMeshDstSize];
  int payload = 1400;
  int n = HdBuildMeshDataHeader(buf, 0x1234, payload);

  EXPECT_EQ(n, kHdFrameHeaderSize + kHdMeshDstSize);
  EXPECT_EQ(HdReadFrameType(buf), HdFrameType::kMeshData);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(
                kHdMeshDstSize + payload));
  // Verify big-endian dst bytes.
  EXPECT_EQ(buf[kHdFrameHeaderSize], 0x12);
  EXPECT_EQ(buf[kHdFrameHeaderSize + 1], 0x34);
}

TEST(HdProtocolTest, ReadMeshDstRoundtrip) {
  uint8_t buf[kHdFrameHeaderSize + kHdMeshDstSize];
  HdBuildMeshDataHeader(buf, 0xABCD, 100);
  uint16_t id = HdReadMeshDst(buf + kHdFrameHeaderSize);
  EXPECT_EQ(id, 0xABCD);
}

TEST(HdProtocolTest, BuildFleetDataHeader) {
  uint8_t buf[kHdFrameHeaderSize + kHdFleetDstSize];
  int payload = 1400;
  int n = HdBuildFleetDataHeader(
      buf, 0x0102, 0x0304, payload);

  EXPECT_EQ(n, kHdFrameHeaderSize + kHdFleetDstSize);
  EXPECT_EQ(HdReadFrameType(buf),
            HdFrameType::kFleetData);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(
                kHdFleetDstSize + payload));
  // Verify relay ID bytes.
  EXPECT_EQ(buf[kHdFrameHeaderSize], 0x01);
  EXPECT_EQ(buf[kHdFrameHeaderSize + 1], 0x02);
  // Verify peer ID bytes.
  EXPECT_EQ(buf[kHdFrameHeaderSize + 2], 0x03);
  EXPECT_EQ(buf[kHdFrameHeaderSize + 3], 0x04);
}

TEST(HdProtocolTest, ReadFleetRelayAndPeerRoundtrip) {
  uint8_t buf[kHdFrameHeaderSize + kHdFleetDstSize];
  HdBuildFleetDataHeader(buf, 0xFEDC, 0xBA98, 200);

  const uint8_t* payload = buf + kHdFrameHeaderSize;
  EXPECT_EQ(HdReadFleetRelay(payload), 0xFEDC);
  EXPECT_EQ(HdReadFleetPeer(payload), 0xBA98);
}

TEST(HdProtocolTest, BuildRedirect) {
  const char* url = "hd://relay-eu-2.example.com:443";
  int url_len = strlen(url);

  uint8_t buf[kHdFrameHeaderSize + 1 + 256];
  int n = HdBuildRedirect(buf,
                          HdRedirectReason::kRebalancing,
                          url, url_len);

  EXPECT_EQ(n, kHdFrameHeaderSize + 1 + url_len);
  EXPECT_EQ(HdReadFrameType(buf),
            HdFrameType::kRedirect);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(1 + url_len));
  EXPECT_EQ(buf[kHdFrameHeaderSize],
            static_cast<uint8_t>(
                HdRedirectReason::kRebalancing));
  EXPECT_EQ(
      memcmp(buf + kHdFrameHeaderSize + 1, url, url_len),
      0);
}

TEST(HdProtocolTest, RedirectUrlTooLong) {
  char url[kHdRedirectMaxUrl + 1];
  memset(url, 'x', sizeof(url));

  uint8_t buf[kHdFrameHeaderSize + 1 + kHdRedirectMaxUrl +
              1];
  int n = HdBuildRedirect(buf,
                          HdRedirectReason::kDraining,
                          url, sizeof(url));

  EXPECT_EQ(n, -1);
}

TEST(HdProtocolTest, ParseRedirectRoundtrip) {
  const char* url = "hd://target.example:3340";
  int url_len = strlen(url);

  uint8_t buf[kHdFrameHeaderSize + 1 + 256];
  HdBuildRedirect(buf,
                  HdRedirectReason::kGeoCorrection,
                  url, url_len);

  char out_url[kHdRedirectMaxUrl + 1];
  HdRedirectReason out_reason;
  int parsed_len = HdParseRedirect(
      buf + kHdFrameHeaderSize,
      HdReadPayloadLen(buf),
      &out_reason,
      out_url,
      sizeof(out_url));

  EXPECT_EQ(parsed_len, url_len);
  EXPECT_EQ(out_reason,
            HdRedirectReason::kGeoCorrection);
  EXPECT_STREQ(out_url, url);
}

TEST(HdProtocolTest, ParseRedirectRejectsEmpty) {
  HdRedirectReason r;
  char out_url[16];
  EXPECT_EQ(HdParseRedirect(nullptr, 0, &r, out_url,
                            sizeof(out_url)),
            -1);
}

TEST(HdProtocolTest, MeshDataVsDataOverhead) {
  // MeshData header = 4B frame header + 2B dst = 6B.
  // Data header = 4B frame header = 4B.
  uint8_t mesh_buf[kHdFrameHeaderSize + kHdMeshDstSize];
  int mesh_n = HdBuildMeshDataHeader(mesh_buf, 1, 100);

  uint8_t data_buf[kHdFrameHeaderSize];
  int data_n = HdBuildDataHeader(data_buf, 100);

  EXPECT_EQ(mesh_n, 6);
  EXPECT_EQ(data_n, 4);
  EXPECT_EQ(mesh_n - data_n, 2);
}

}  // namespace hyper_derp
