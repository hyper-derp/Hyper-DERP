/// @file test_stun.cc
/// @brief Unit tests for STUN message codec.

#include "hyper_derp/stun.h"

#include <arpa/inet.h>

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

// -- Helpers ----------------------------------------------------------------

namespace {

/// Build a minimal valid STUN header in buf.
/// Returns kStunHeaderSize.
int WriteMinimalHeader(uint8_t* buf, uint16_t type,
                       uint16_t payload_len) {
  buf[0] = static_cast<uint8_t>(type >> 8);
  buf[1] = static_cast<uint8_t>(type);
  buf[2] = static_cast<uint8_t>(payload_len >> 8);
  buf[3] = static_cast<uint8_t>(payload_len);
  // Magic cookie.
  buf[4] = 0x21;
  buf[5] = 0x12;
  buf[6] = 0xA4;
  buf[7] = 0x42;
  // Transaction ID (12 bytes of 0xAA).
  std::memset(buf + 8, 0xAA, kStunTransactionIdSize);
  return kStunHeaderSize;
}

}  // namespace

// -- StunIsStun tests -------------------------------------------------------

TEST(StunTest, IsStunValid) {
  uint8_t buf[kStunHeaderSize];
  WriteMinimalHeader(buf, kStunBindingRequest, 0);
  EXPECT_TRUE(StunIsStun(buf, kStunHeaderSize));
}

TEST(StunTest, IsStunTooShort) {
  uint8_t buf[10] = {};
  EXPECT_FALSE(StunIsStun(buf, 10));
}

TEST(StunTest, IsStunBadCookie) {
  uint8_t buf[kStunHeaderSize];
  WriteMinimalHeader(buf, kStunBindingRequest, 0);
  // Corrupt the magic cookie.
  buf[4] = 0xFF;
  EXPECT_FALSE(StunIsStun(buf, kStunHeaderSize));
}

// -- StunParse tests --------------------------------------------------------

TEST(StunTest, ParseBindingRequest) {
  uint8_t buf[kStunHeaderSize];
  WriteMinimalHeader(buf, kStunBindingRequest, 0);

  StunMessage msg;
  EXPECT_TRUE(StunParse(buf, kStunHeaderSize, &msg));
  EXPECT_EQ(msg.type, kStunBindingRequest);
  // Verify transaction ID.
  uint8_t expected_id[kStunTransactionIdSize];
  std::memset(expected_id, 0xAA, kStunTransactionIdSize);
  EXPECT_EQ(std::memcmp(msg.transaction_id, expected_id,
                         kStunTransactionIdSize),
            0);
  EXPECT_FALSE(msg.has_xor_mapped);
  EXPECT_FALSE(msg.has_username);
  EXPECT_FALSE(msg.has_integrity);
  EXPECT_FALSE(msg.has_fingerprint);
  EXPECT_FALSE(msg.has_error);
}

TEST(StunTest, ParseBindingResponse) {
  // Build a response with XOR-MAPPED-ADDRESS attribute.
  // Header (20) + attr header (4) + attr value (8) = 32.
  uint8_t buf[32];
  WriteMinimalHeader(buf, kStunBindingResponse, 12);

  int off = kStunHeaderSize;
  // Attr type: XOR-MAPPED-ADDRESS.
  buf[off++] = 0x00;
  buf[off++] = 0x20;
  // Attr length: 8.
  buf[off++] = 0x00;
  buf[off++] = 0x08;
  // Reserved.
  buf[off++] = 0x00;
  // Family: IPv4.
  buf[off++] = 0x01;
  // XOR'd port (0x2112 XOR 0x1234 = 0x3306) big-endian.
  buf[off++] = 0x33;
  buf[off++] = 0x06;
  // XOR'd address (0x2112A442 XOR 0xC0A80164) big-endian.
  // = 0xE1BAA526
  buf[off++] = 0xE1;
  buf[off++] = 0xBA;
  buf[off++] = 0xA5;
  buf[off++] = 0x26;

  StunMessage msg;
  EXPECT_TRUE(StunParse(buf, sizeof(buf), &msg));
  EXPECT_EQ(msg.type, kStunBindingResponse);
  EXPECT_TRUE(msg.has_xor_mapped);
  EXPECT_EQ(msg.xor_mapped_port, 0x3306);
  EXPECT_EQ(msg.xor_mapped_ip, 0xE1BAA526u);
}

// -- Build + roundtrip tests ------------------------------------------------

TEST(StunTest, BuildBindingRequest) {
  uint8_t txn_id[kStunTransactionIdSize];
  std::memset(txn_id, 0x42, kStunTransactionIdSize);

  uint8_t buf[64];
  int n = StunBuildBindingRequest(buf, sizeof(buf), txn_id);
  EXPECT_EQ(n, kStunHeaderSize);

  // Re-parse.
  StunMessage msg;
  EXPECT_TRUE(StunParse(buf, n, &msg));
  EXPECT_EQ(msg.type, kStunBindingRequest);
  EXPECT_EQ(std::memcmp(msg.transaction_id, txn_id,
                         kStunTransactionIdSize),
            0);
}

TEST(StunTest, BuildBindingResponse) {
  uint8_t txn_id[kStunTransactionIdSize];
  std::memset(txn_id, 0x55, kStunTransactionIdSize);

  // 192.168.1.100 = 0xC0A80164 in network byte order.
  uint32_t client_ip = htonl(0xC0A80164);
  // Port 4660 = 0x1234 in network byte order.
  uint16_t client_port = htons(0x1234);

  uint8_t buf[64];
  int n = StunBuildBindingResponse(
      buf, sizeof(buf), txn_id, client_ip, client_port);
  EXPECT_EQ(n, 32);

  // Re-parse.
  StunMessage msg;
  EXPECT_TRUE(StunParse(buf, n, &msg));
  EXPECT_EQ(msg.type, kStunBindingResponse);
  EXPECT_TRUE(msg.has_xor_mapped);

  // Decode XOR address and verify.
  uint16_t decoded_port;
  uint32_t decoded_addr;
  StunDecodeXorAddress(
      htons(msg.xor_mapped_port),
      htonl(msg.xor_mapped_ip),
      &decoded_port, &decoded_addr);
  EXPECT_EQ(decoded_port, client_port);
  EXPECT_EQ(decoded_addr, client_ip);
}

TEST(StunTest, BuildErrorResponse) {
  uint8_t txn_id[kStunTransactionIdSize];
  std::memset(txn_id, 0x77, kStunTransactionIdSize);

  const char* reason = "Bad Request";
  uint8_t buf[128];
  int n = StunBuildErrorResponse(
      buf, sizeof(buf), txn_id, 400, reason);
  EXPECT_GT(n, kStunHeaderSize);

  // Re-parse.
  StunMessage msg;
  EXPECT_TRUE(StunParse(buf, n, &msg));
  EXPECT_EQ(msg.type, kStunBindingError);
  EXPECT_TRUE(msg.has_error);
  EXPECT_EQ(msg.error_code, 400);
  EXPECT_EQ(std::string(msg.error_reason,
                         msg.error_reason_len),
            "Bad Request");
}

// -- XOR address decode -----------------------------------------------------

TEST(StunTest, XorAddressDecode) {
  // Port 0x1234 XOR'd: 0x1234 ^ 0x2112 = 0x3326.
  uint16_t xor_port = htons(0x3326);
  // IP 192.168.1.100 (0xC0A80164) XOR'd with cookie:
  // 0xC0A80164 ^ 0x2112A442 = 0xE1BAA526.
  uint32_t xor_addr = htonl(0xE1BAA526);

  uint16_t decoded_port;
  uint32_t decoded_addr;
  StunDecodeXorAddress(xor_port, xor_addr,
                       &decoded_port, &decoded_addr);

  EXPECT_EQ(ntohs(decoded_port), 0x1234);
  EXPECT_EQ(ntohl(decoded_addr), 0xC0A80164u);
}

// -- Attribute padding ------------------------------------------------------

TEST(StunTest, AttributePadding) {
  // Build a message with a USERNAME attribute that has an
  // odd-length value (5 bytes = "alice"), followed by a
  // FINGERPRINT attribute. The parser must skip padding
  // bytes to find the second attribute.
  //
  // Layout:
  //   Header: 20 bytes
  //   USERNAME attr: 4 (hdr) + 5 (value) + 3 (pad) = 12
  //   FINGERPRINT attr: 4 (hdr) + 4 (value) = 8
  //   Total payload: 20
  uint8_t buf[40];
  WriteMinimalHeader(buf, kStunBindingRequest, 20);

  int off = kStunHeaderSize;
  // USERNAME attribute.
  buf[off++] = 0x00;
  buf[off++] = 0x06;
  // Length: 5.
  buf[off++] = 0x00;
  buf[off++] = 0x05;
  // Value: "alice".
  buf[off++] = 'a';
  buf[off++] = 'l';
  buf[off++] = 'i';
  buf[off++] = 'c';
  buf[off++] = 'e';
  // 3 padding bytes.
  buf[off++] = 0x00;
  buf[off++] = 0x00;
  buf[off++] = 0x00;
  // FINGERPRINT attribute.
  buf[off++] = 0x80;
  buf[off++] = 0x28;
  // Length: 4.
  buf[off++] = 0x00;
  buf[off++] = 0x04;
  // Value: 0xDEADBEEF.
  buf[off++] = 0xDE;
  buf[off++] = 0xAD;
  buf[off++] = 0xBE;
  buf[off++] = 0xEF;

  StunMessage msg;
  EXPECT_TRUE(StunParse(buf, off, &msg));
  EXPECT_TRUE(msg.has_username);
  EXPECT_EQ(msg.username_len, 5);
  EXPECT_EQ(std::string(msg.username, msg.username_len),
            "alice");
  EXPECT_TRUE(msg.has_fingerprint);
  EXPECT_EQ(msg.fingerprint, 0xDEADBEEFu);
}

}  // namespace hyper_derp
