/// @file test_handshake.cc
/// @brief Unit tests for DERP handshake (key generation,
///   frame building, ClientInfo parsing).

#include "hyper_derp/handshake.h"

#include <cstring>
#include <sodium.h>

#include <gtest/gtest.h>

namespace hyper_derp {

class HandshakeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(sodium_init() >= 0, true);
    ASSERT_EQ(GenerateServerKeys(&server_keys_), 0);
  }

  ServerKeys server_keys_;
};

TEST_F(HandshakeTest, GenerateServerKeysProducesKeys) {
  // Keys should be non-zero.
  uint8_t zero[kKeySize] = {};
  EXPECT_NE(
      memcmp(server_keys_.public_key, zero, kKeySize), 0);
  EXPECT_NE(
      memcmp(server_keys_.private_key, zero, kKeySize), 0);
  // Public and private should differ.
  EXPECT_NE(
      memcmp(server_keys_.public_key,
             server_keys_.private_key, kKeySize),
      0);
}

TEST_F(HandshakeTest, BuildServerKeyFrame) {
  uint8_t buf[kFrameHeaderSize + kServerKeyPayloadSize];
  int n = BuildServerKeyFrame(buf, &server_keys_);

  EXPECT_EQ(n,
            kFrameHeaderSize + kServerKeyPayloadSize);
  EXPECT_EQ(ReadFrameType(buf), FrameType::kServerKey);
  EXPECT_EQ(ReadPayloadLen(buf),
            static_cast<uint32_t>(
                kServerKeyPayloadSize));

  // Check magic.
  EXPECT_EQ(
      memcmp(buf + kFrameHeaderSize, kMagic, kMagicSize),
      0);

  // Check public key.
  EXPECT_EQ(
      memcmp(buf + kFrameHeaderSize + kMagicSize,
             server_keys_.public_key, kKeySize),
      0);
}

TEST_F(HandshakeTest, BuildAndParseClientInfo) {
  // Simulate what a Tailscale client does: build a
  // ClientInfo payload and verify we can parse it.

  // Generate client key pair.
  uint8_t client_pub[kKeySize];
  uint8_t client_priv[kKeySize];
  crypto_box_keypair(client_pub, client_priv);

  // Build ClientInfo payload:
  // [32B client pub][24B nonce][ciphertext]
  const char* json =
      "{\"version\":2,\"CanAckPings\":true}";
  int json_len = strlen(json);

  uint8_t nonce[kNonceSize];
  randombytes_buf(nonce, kNonceSize);

  int ciphertext_len = json_len + kBoxOverhead;
  int payload_len = kKeySize + kNonceSize + ciphertext_len;
  auto* payload = new uint8_t[payload_len];

  // Client public key.
  memcpy(payload, client_pub, kKeySize);

  // Nonce.
  memcpy(payload + kKeySize, nonce, kNonceSize);

  // NaCl box: encrypt with client_priv + server_pub.
  ASSERT_EQ(
      crypto_box_easy(
          payload + kKeySize + kNonceSize,
          reinterpret_cast<const uint8_t*>(json),
          json_len, nonce,
          server_keys_.public_key, client_priv),
      0);

  // Parse it.
  ClientInfo info;
  int rc = ParseClientInfo(payload, payload_len,
                           &server_keys_, &info);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(
      memcmp(info.public_key, client_pub, kKeySize), 0);
  EXPECT_EQ(info.version, 2);
  EXPECT_TRUE(info.can_ack_pings);

  delete[] payload;
}

TEST_F(HandshakeTest, ParseClientInfoBadCrypto) {
  // Garbled ciphertext should fail.
  int payload_len =
      kKeySize + kNonceSize + kBoxOverhead + 10;
  auto* payload = new uint8_t[payload_len];
  randombytes_buf(payload, payload_len);

  ClientInfo info;
  int rc = ParseClientInfo(payload, payload_len,
                           &server_keys_, &info);
  EXPECT_EQ(rc, -1);

  delete[] payload;
}

TEST_F(HandshakeTest, ParseClientInfoTooShort) {
  uint8_t payload[kKeySize];
  memset(payload, 0, kKeySize);

  ClientInfo info;
  int rc = ParseClientInfo(payload, kKeySize,
                           &server_keys_, &info);
  EXPECT_EQ(rc, -1);
}

TEST_F(HandshakeTest, BuildServerInfoFrame) {
  uint8_t client_pub[kKeySize];
  uint8_t client_priv[kKeySize];
  crypto_box_keypair(client_pub, client_priv);

  uint8_t buf[256];
  int n = BuildServerInfoFrame(
      buf, sizeof(buf), &server_keys_, client_pub);
  ASSERT_GT(n, 0);

  EXPECT_EQ(ReadFrameType(buf), FrameType::kServerInfo);
  uint32_t payload_len = ReadPayloadLen(buf);
  EXPECT_GE(payload_len,
            static_cast<uint32_t>(
                kNonceSize + kBoxOverhead));

  // Decrypt as the client would.
  const uint8_t* sealed = buf + kFrameHeaderSize;
  const uint8_t* nonce = sealed;
  const uint8_t* ciphertext = sealed + kNonceSize;
  int ciphertext_len =
      static_cast<int>(payload_len) - kNonceSize;

  uint8_t plaintext[256];
  ASSERT_EQ(
      crypto_box_open_easy(
          plaintext, ciphertext, ciphertext_len,
          nonce, server_keys_.public_key, client_priv),
      0);

  int pt_len = ciphertext_len - kBoxOverhead;
  plaintext[pt_len] = '\0';
  EXPECT_TRUE(
      strstr(reinterpret_cast<char*>(plaintext),
             "\"version\""));
}

TEST_F(HandshakeTest, KeyToHexRoundtrip) {
  char hex[kKeySize * 2 + 1];
  KeyToHex(server_keys_.public_key, hex);

  EXPECT_EQ(strlen(hex),
            static_cast<size_t>(kKeySize * 2));

  // All hex chars.
  for (int i = 0; hex[i]; i++) {
    char c = hex[i];
    bool ok = (c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f');
    EXPECT_TRUE(ok) << "non-hex char at index " << i;
  }
}

}  // namespace hyper_derp
