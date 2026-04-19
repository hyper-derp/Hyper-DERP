/// @file test_mesh_routing.cc
/// @brief Unit tests for MeshData and FleetData frame
///   routing.

#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"

#include <cstring>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

namespace hyper_derp {

TEST(MeshRoutingTest, MeshDataFrameFormat) {
  uint8_t buf[kHdFrameHeaderSize + kHdMeshDstSize];
  int n = HdBuildMeshDataHeader(buf, 42, 100);

  EXPECT_EQ(n, kHdFrameHeaderSize + kHdMeshDstSize);

  // Type byte is MeshData (0x04).
  EXPECT_EQ(HdReadFrameType(buf), HdFrameType::kMeshData);

  // Payload length = 2B dst + 100B data = 102.
  EXPECT_EQ(HdReadPayloadLen(buf), 102u);

  // Read back destination peer ID.
  uint16_t dst = HdReadMeshDst(buf + kHdFrameHeaderSize);
  EXPECT_EQ(dst, 42u);
}

TEST(MeshRoutingTest, MeshDataRoundtrip) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  // Build a complete MeshData frame: header + payload.
  const uint8_t payload[] = "hello mesh";
  int payload_len = sizeof(payload) - 1;
  int hdr_len = kHdFrameHeaderSize + kHdMeshDstSize;
  int total = hdr_len + payload_len;

  uint8_t frame[64];
  HdBuildMeshDataHeader(frame, 7, payload_len);
  std::memcpy(frame + hdr_len, payload, payload_len);

  // Write on one end.
  ASSERT_EQ(write(sv[0], frame, total), total);

  // Read header on the other end.
  uint8_t hdr[kHdFrameHeaderSize];
  ASSERT_EQ(read(sv[1], hdr, kHdFrameHeaderSize),
            kHdFrameHeaderSize);

  EXPECT_EQ(HdReadFrameType(hdr), HdFrameType::kMeshData);
  uint32_t plen = HdReadPayloadLen(hdr);
  EXPECT_EQ(plen,
            static_cast<uint32_t>(
                kHdMeshDstSize + payload_len));

  // Read payload (dst + data).
  uint8_t recv_buf[64];
  ASSERT_EQ(read(sv[1], recv_buf, plen),
            static_cast<ssize_t>(plen));

  EXPECT_EQ(HdReadMeshDst(recv_buf), 7u);
  EXPECT_EQ(std::memcmp(recv_buf + kHdMeshDstSize,
                        payload, payload_len),
            0);

  close(sv[0]);
  close(sv[1]);
}

TEST(MeshRoutingTest, DataVsMeshDataOverhead) {
  // Data header for 1400B payload.
  uint8_t data_hdr[kHdFrameHeaderSize];
  int data_n = HdBuildDataHeader(data_hdr, 1400);
  EXPECT_EQ(data_n, kHdFrameHeaderSize);

  // MeshData header for 1400B payload.
  uint8_t mesh_hdr[kHdFrameHeaderSize + kHdMeshDstSize];
  int mesh_n = HdBuildMeshDataHeader(mesh_hdr, 1, 1400);
  EXPECT_EQ(mesh_n, kHdFrameHeaderSize + kHdMeshDstSize);

  // MeshData is exactly 2 bytes more header overhead.
  EXPECT_EQ(mesh_n - data_n, kHdMeshDstSize);
  EXPECT_EQ(mesh_n - data_n, 2);
}

TEST(MeshRoutingTest, FleetDataFrameFormat) {
  uint8_t buf[kHdFrameHeaderSize + kHdFleetDstSize];
  int n = HdBuildFleetDataHeader(buf, 5, 42, 100);

  EXPECT_EQ(n, kHdFrameHeaderSize + kHdFleetDstSize);

  // Type byte is FleetData (0x05).
  EXPECT_EQ(HdReadFrameType(buf),
            HdFrameType::kFleetData);

  // Payload length = 4B dst + 100B data = 104.
  EXPECT_EQ(HdReadPayloadLen(buf), 104u);

  // Read back relay and peer IDs.
  const uint8_t* p = buf + kHdFrameHeaderSize;
  EXPECT_EQ(HdReadFleetRelay(p), 5u);
  EXPECT_EQ(HdReadFleetPeer(p), 42u);
}

TEST(MeshRoutingTest, HdClientSendMeshData) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  // Set up a minimal HdClient using the write end.
  HdClient client{};
  client.fd = sv[0];
  client.connected = true;
  client.approved = true;
  client.peer_id = 99;

  // Send MeshData to peer 42 with a 10-byte payload.
  const uint8_t payload[] = {0, 1, 2, 3, 4,
                             5, 6, 7, 8, 9};
  auto result = HdClientSendMeshData(
      &client, 42, payload, sizeof(payload));
  ASSERT_TRUE(result.has_value()) << result.error().message;

  // Read it back on the other end.
  int hdr_len = kHdFrameHeaderSize + kHdMeshDstSize;
  int total = hdr_len + static_cast<int>(sizeof(payload));
  uint8_t recv_buf[64];
  int got = 0;
  while (got < total) {
    ssize_t r = read(sv[1], recv_buf + got, total - got);
    ASSERT_GT(r, 0);
    got += static_cast<int>(r);
  }

  EXPECT_EQ(HdReadFrameType(recv_buf),
            HdFrameType::kMeshData);
  uint32_t plen = HdReadPayloadLen(recv_buf);
  EXPECT_EQ(plen,
            static_cast<uint32_t>(
                kHdMeshDstSize + sizeof(payload)));

  uint16_t dst =
      HdReadMeshDst(recv_buf + kHdFrameHeaderSize);
  EXPECT_EQ(dst, 42u);

  EXPECT_EQ(std::memcmp(recv_buf + hdr_len,
                        payload, sizeof(payload)),
            0);

  // Clean up without HdClientClose (we own the fds).
  client.fd = -1;
  close(sv[0]);
  close(sv[1]);
}

TEST(MeshRoutingTest, MeshDataMaxPayload) {
  // MeshData frame at maximum payload size should succeed
  // validation.
  int max_data = kHdMaxFramePayload - kHdMeshDstSize;
  uint8_t buf[kHdFrameHeaderSize + kHdMeshDstSize];
  int n = HdBuildMeshDataHeader(buf, 1, max_data);

  EXPECT_EQ(n, kHdFrameHeaderSize + kHdMeshDstSize);
  uint32_t plen = HdReadPayloadLen(buf);
  EXPECT_EQ(plen,
            static_cast<uint32_t>(kHdMaxFramePayload));
  EXPECT_TRUE(HdIsValidPayloadLen(plen));
}

TEST(MeshRoutingTest, MeshDataZeroPayload) {
  uint8_t buf[kHdFrameHeaderSize + kHdMeshDstSize];
  int n = HdBuildMeshDataHeader(buf, 100, 0);

  EXPECT_EQ(n, kHdFrameHeaderSize + kHdMeshDstSize);
  // Payload length is just the 2B destination.
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(kHdMeshDstSize));
  EXPECT_EQ(HdReadMeshDst(buf + kHdFrameHeaderSize),
            100u);
}

TEST(MeshRoutingTest, MeshDataHighPeerIds) {
  // Verify big-endian encoding of high peer IDs.
  uint8_t buf[kHdFrameHeaderSize + kHdMeshDstSize];
  HdBuildMeshDataHeader(buf, 0xFFFF, 0);
  EXPECT_EQ(HdReadMeshDst(buf + kHdFrameHeaderSize),
            0xFFFFu);

  HdBuildMeshDataHeader(buf, 0x0100, 0);
  EXPECT_EQ(HdReadMeshDst(buf + kHdFrameHeaderSize),
            0x0100u);

  HdBuildMeshDataHeader(buf, 0x00FF, 0);
  EXPECT_EQ(HdReadMeshDst(buf + kHdFrameHeaderSize),
            0x00FFu);
}

TEST(MeshRoutingTest, SendMeshDataOverflow) {
  HdClient client{};
  client.fd = -1;

  // Payload that exceeds max frame size should fail.
  int too_big = kHdMaxFramePayload + 1;
  auto result = HdClientSendMeshData(
      &client, 1, nullptr, too_big);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code,
            HdClientError::BufferOverflow);
}

}  // namespace hyper_derp
