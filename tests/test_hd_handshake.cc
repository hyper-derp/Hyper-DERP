/// @file test_hd_handshake.cc
/// @brief Unit tests for HD enrollment handshake.

#include "hyper_derp/hd_handshake.h"

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <sodium.h>

#include "hyper_derp/hd_peers.h"
#include "hyper_derp/hd_protocol.h"

namespace hyper_derp {

class HdHandshakeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    randombytes_buf(relay_key_.data(), kKeySize);
    randombytes_buf(client_key_.data(), kKeySize);
    // Compute valid HMAC for client_key using relay_key.
    crypto_auth(valid_hmac_, client_key_.data(),
                kKeySize, relay_key_.data());
  }

  /// Create a socketpair, returning fds via out params.
  void MakeSocketPair(int* server_fd, int* client_fd) {
    int fds[2];
    ASSERT_EQ(
        socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    *server_fd = fds[0];
    *client_fd = fds[1];
  }

  /// Write a valid Enroll frame to fd.
  void WriteEnrollFrame(int fd, const Key& key,
                        const uint8_t* hmac) {
    uint8_t buf[kHdFrameHeaderSize + kKeySize +
                kHdHmacSize];
    int n = HdBuildEnroll(buf, key, hmac, kHdHmacSize);
    ASSERT_EQ(write(fd, buf, n), n);
  }

  /// Blocking read of exactly n bytes from fd.
  void ReadExact(int fd, uint8_t* buf, int n) {
    int total = 0;
    while (total < n) {
      int r = read(fd, buf + total, n - total);
      ASSERT_GT(r, 0) << "unexpected EOF or error";
      total += r;
    }
  }

  Key relay_key_{};
  Key client_key_{};
  uint8_t valid_hmac_[kHdHmacSize]{};
};

TEST_F(HdHandshakeTest, AutoApprove) {
  int srv, cli;
  MakeSocketPair(&srv, &cli);

  HdPeerRegistry reg;
  HdPeersInit(&reg, relay_key_,
              HdEnrollMode::kAutoApprove);

  // Client sends Enroll frame.
  WriteEnrollFrame(cli, client_key_, valid_hmac_);

  // Server performs handshake.
  HdEnrollResult result;
  auto r = HdPerformHandshake(srv, &reg, &result);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(result.auto_approved);
  EXPECT_EQ(result.client_key, client_key_);

  // Peer should be approved in registry.
  HdPeer* p = HdPeersLookup(&reg, client_key_.data());
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->state, HdPeerState::kApproved);

  // Client should receive Approved frame.
  uint8_t resp[kHdFrameHeaderSize + kKeySize];
  ReadExact(cli, resp,
            kHdFrameHeaderSize + kKeySize);
  EXPECT_EQ(HdReadFrameType(resp),
            HdFrameType::kApproved);
  EXPECT_EQ(HdReadPayloadLen(resp),
            static_cast<uint32_t>(kKeySize));
  EXPECT_EQ(memcmp(resp + kHdFrameHeaderSize,
                   client_key_.data(), kKeySize), 0);

  close(srv);
  close(cli);
}

TEST_F(HdHandshakeTest, ManualPending) {
  int srv, cli;
  MakeSocketPair(&srv, &cli);

  HdPeerRegistry reg;
  HdPeersInit(&reg, relay_key_, HdEnrollMode::kManual);

  WriteEnrollFrame(cli, client_key_, valid_hmac_);

  HdEnrollResult result;
  auto r = HdPerformHandshake(srv, &reg, &result);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_FALSE(result.auto_approved);
  EXPECT_EQ(result.client_key, client_key_);

  // Peer should be pending in registry.
  HdPeer* p = HdPeersLookup(&reg, client_key_.data());
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->state, HdPeerState::kPending);

  close(srv);
  close(cli);
}

TEST_F(HdHandshakeTest, InvalidHmac) {
  int srv, cli;
  MakeSocketPair(&srv, &cli);

  HdPeerRegistry reg;
  HdPeersInit(&reg, relay_key_, HdEnrollMode::kManual);

  // Send Enroll with bad HMAC.
  uint8_t bad_hmac[kHdHmacSize];
  memset(bad_hmac, 0x00, kHdHmacSize);
  WriteEnrollFrame(cli, client_key_, bad_hmac);

  HdEnrollResult result;
  auto r = HdPerformHandshake(srv, &reg, &result);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code,
            HdHandshakeError::InvalidHmac);

  // Client should receive Denied frame.
  uint8_t resp_hdr[kHdFrameHeaderSize];
  ReadExact(cli, resp_hdr, kHdFrameHeaderSize);
  EXPECT_EQ(HdReadFrameType(resp_hdr),
            HdFrameType::kDenied);

  close(srv);
  close(cli);
}

TEST_F(HdHandshakeTest, WrongFrameType) {
  int srv, cli;
  MakeSocketPair(&srv, &cli);

  HdPeerRegistry reg;
  HdPeersInit(&reg, relay_key_, HdEnrollMode::kManual);

  // Send a Ping frame instead of Enroll.
  uint8_t ping_data[kHdPingDataSize];
  memset(ping_data, 0xAA, kHdPingDataSize);
  uint8_t buf[kHdFrameHeaderSize + kHdPingDataSize];
  int n = HdBuildPing(buf, ping_data);
  ASSERT_EQ(write(cli, buf, n), n);

  HdEnrollResult result;
  auto r = HdPerformHandshake(srv, &reg, &result);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code,
            HdHandshakeError::UnexpectedFrame);

  close(srv);
  close(cli);
}

TEST_F(HdHandshakeTest, TruncatedPayload) {
  int srv, cli;
  MakeSocketPair(&srv, &cli);

  HdPeerRegistry reg;
  HdPeersInit(&reg, relay_key_, HdEnrollMode::kManual);

  // Write Enroll header with payload_len < 64.
  uint8_t hdr[kHdFrameHeaderSize];
  HdWriteFrameHeader(hdr, HdFrameType::kEnroll, 32);
  ASSERT_EQ(write(cli, hdr, kHdFrameHeaderSize),
            kHdFrameHeaderSize);

  HdEnrollResult result;
  auto r = HdPerformHandshake(srv, &reg, &result);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code,
            HdHandshakeError::BadPayloadLength);

  close(srv);
  close(cli);
}

TEST_F(HdHandshakeTest, SendApproved) {
  int srv, cli;
  MakeSocketPair(&srv, &cli);

  auto r = HdSendApproved(srv, client_key_);
  ASSERT_TRUE(r.has_value()) << r.error().message;

  // Read and verify the Approved frame.
  uint8_t resp[kHdFrameHeaderSize + kKeySize];
  ReadExact(cli, resp,
            kHdFrameHeaderSize + kKeySize);
  EXPECT_EQ(HdReadFrameType(resp),
            HdFrameType::kApproved);
  EXPECT_EQ(HdReadPayloadLen(resp),
            static_cast<uint32_t>(kKeySize));
  EXPECT_EQ(memcmp(resp + kHdFrameHeaderSize,
                   client_key_.data(), kKeySize), 0);

  close(srv);
  close(cli);
}

TEST_F(HdHandshakeTest, SendDenied) {
  int srv, cli;
  MakeSocketPair(&srv, &cli);

  const char* msg = "not allowed";
  auto r = HdSendDenied(srv, 0x02, msg);
  ASSERT_TRUE(r.has_value()) << r.error().message;

  // Read and verify the Denied frame.
  uint8_t resp_hdr[kHdFrameHeaderSize];
  ReadExact(cli, resp_hdr, kHdFrameHeaderSize);
  EXPECT_EQ(HdReadFrameType(resp_hdr),
            HdFrameType::kDenied);

  int msg_len = static_cast<int>(strlen(msg));
  uint32_t expected_payload = 1 + msg_len;
  EXPECT_EQ(HdReadPayloadLen(resp_hdr), expected_payload);

  // Read payload: 1 byte reason + message.
  auto payload =
      std::make_unique<uint8_t[]>(expected_payload);
  ReadExact(cli, payload.get(),
            static_cast<int>(expected_payload));
  EXPECT_EQ(payload[0], 0x02);
  EXPECT_EQ(memcmp(payload.get() + 1, msg, msg_len), 0);

  close(srv);
  close(cli);
}

}  // namespace hyper_derp
