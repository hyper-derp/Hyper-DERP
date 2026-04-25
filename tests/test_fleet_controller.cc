/// @file test_fleet_controller.cc
/// @brief Unit tests for FleetControllerApplyBundle.

#include "hyper_derp/fleet_controller.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <sodium.h>

namespace hyper_derp {
namespace {

// Sign `payload` with the given Ed25519 keypair and
// package into the bundle.json wire format.
std::string MakeBundle(const std::string& payload,
                       const uint8_t* sk,
                       const std::string& outer_fleet,
                       int64_t outer_version,
                       bool corrupt_sig = false) {
  unsigned char sig[crypto_sign_ed25519_BYTES];
  crypto_sign_ed25519_detached(
      sig, nullptr,
      reinterpret_cast<const unsigned char*>(
          payload.data()),
      payload.size(), sk);
  if (corrupt_sig) sig[0] ^= 0xFF;

  auto b64 = [](const unsigned char* data, int n) {
    int cap =
        sodium_base64_ENCODED_LEN(
            n, sodium_base64_VARIANT_ORIGINAL);
    std::string out(cap, '\0');
    sodium_bin2base64(
        out.data(), cap, data, n,
        sodium_base64_VARIANT_ORIGINAL);
    out.resize(std::strlen(out.c_str()));
    return out;
  };
  std::string sig_b64 = b64(sig, sizeof(sig));
  std::string payload_b64 = b64(
      reinterpret_cast<const unsigned char*>(
          payload.data()),
      static_cast<int>(payload.size()));

  std::string out =
      "{\"version\":" + std::to_string(outer_version) +
      ",\"fleet_id\":\"" + outer_fleet +
      "\",\"signed_body_b64\":\"" + payload_b64 +
      "\",\"signature_b64\":\"" + sig_b64 + "\"}";
  return out;
}

class FleetControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    crypto_sign_ed25519_keypair(pk_, sk_);

    // Minimal registry.
    HdPeersInit(&reg_, Key{}, HdEnrollMode::kManual);
    reg_.relay_id = 7;

    char b64pk[sodium_base64_ENCODED_LEN(
        crypto_sign_ed25519_PUBLICKEYBYTES,
        sodium_base64_VARIANT_ORIGINAL)];
    sodium_bin2base64(
        b64pk, sizeof(b64pk), pk_,
        crypto_sign_ed25519_PUBLICKEYBYTES,
        sodium_base64_VARIANT_ORIGINAL);

    config_.signing_pubkey_b64 = b64pk;
    config_.fleet_id = "company-a";
    fc_.config = config_;
    fc_.hd_peers = &reg_;
  }

  // cpplint's runtime/arrays rule wants the size token to
  // start with k+CamelCase; libsodium's all-lowercase
  // macros trip it.
  static constexpr size_t kSignPubBytes =
      crypto_sign_ed25519_PUBLICKEYBYTES;
  static constexpr size_t kSignSecBytes =
      crypto_sign_ed25519_SECRETKEYBYTES;

  HdPeerRegistry reg_;
  FleetControllerConfig config_;
  FleetController fc_;
  uint8_t pk_[kSignPubBytes];
  uint8_t sk_[kSignSecBytes];
};

TEST_F(FleetControllerTest, AcceptsValidBundle) {
  std::string inner =
      "{\"fleet_id\":\"company-a\","
      "\"policy\":{\"allow_direct\":false},"
      "\"revocations\":{},"
      "\"version\":1}";
  std::string bundle = MakeBundle(inner, sk_,
                                   "company-a", 1);
  auto st = FleetControllerApplyBundle(
      &fc_,
      reinterpret_cast<const uint8_t*>(bundle.data()),
      static_cast<int>(bundle.size()));
  EXPECT_EQ(st, FleetApplyStatus::kOk);
  EXPECT_EQ(fc_.applied_version.load(), 1);
  EXPECT_FALSE(reg_.fleet_policy.allow_direct);
}

TEST_F(FleetControllerTest, RejectsBadSignature) {
  std::string inner =
      "{\"fleet_id\":\"company-a\",\"version\":1,"
      "\"policy\":{},\"revocations\":{}}";
  std::string bundle = MakeBundle(inner, sk_,
                                   "company-a", 1,
                                   /*corrupt=*/true);
  auto st = FleetControllerApplyBundle(
      &fc_,
      reinterpret_cast<const uint8_t*>(bundle.data()),
      static_cast<int>(bundle.size()));
  EXPECT_EQ(st, FleetApplyStatus::kBadSignature);
  EXPECT_EQ(fc_.applied_version.load(), 0);
}

TEST_F(FleetControllerTest, RejectsStaleVersion) {
  std::string inner =
      "{\"fleet_id\":\"company-a\",\"version\":5,"
      "\"policy\":{},\"revocations\":{}}";
  std::string b1 = MakeBundle(inner, sk_,
                              "company-a", 5);
  EXPECT_EQ(FleetControllerApplyBundle(
                &fc_,
                reinterpret_cast<const uint8_t*>(
                    b1.data()),
                static_cast<int>(b1.size())),
            FleetApplyStatus::kOk);
  std::string older =
      "{\"fleet_id\":\"company-a\",\"version\":3,"
      "\"policy\":{},\"revocations\":{}}";
  std::string b2 = MakeBundle(older, sk_,
                              "company-a", 3);
  EXPECT_EQ(FleetControllerApplyBundle(
                &fc_,
                reinterpret_cast<const uint8_t*>(
                    b2.data()),
                static_cast<int>(b2.size())),
            FleetApplyStatus::kStaleVersion);
  EXPECT_EQ(fc_.applied_version.load(), 5);
}

TEST_F(FleetControllerTest, RejectsFleetMismatch) {
  std::string inner =
      "{\"fleet_id\":\"other\",\"version\":1,"
      "\"policy\":{},\"revocations\":{}}";
  std::string bundle = MakeBundle(inner, sk_,
                                   "other", 1);
  auto st = FleetControllerApplyBundle(
      &fc_,
      reinterpret_cast<const uint8_t*>(bundle.data()),
      static_cast<int>(bundle.size()));
  EXPECT_EQ(st, FleetApplyStatus::kFleetMismatch);
}

TEST_F(FleetControllerTest, PeerRevocationListPopulated) {
  std::string inner =
      "{\"fleet_id\":\"company-a\",\"version\":1,"
      "\"policy\":{},"
      "\"revocations\":{\"peers\":["
      "{\"peer_fingerprint\":\"ck_abc123\"},"
      "{\"peer_fingerprint\":\"ck_def456\"}]}}";
  std::string bundle = MakeBundle(inner, sk_,
                                   "company-a", 1);
  auto st = FleetControllerApplyBundle(
      &fc_,
      reinterpret_cast<const uint8_t*>(bundle.data()),
      static_cast<int>(bundle.size()));
  EXPECT_EQ(st, FleetApplyStatus::kOk);

  std::vector<std::string> revoked;
  FleetControllerGetRevokedPeers(&fc_, &revoked);
  ASSERT_EQ(revoked.size(), 2u);
  EXPECT_EQ(revoked[0], "ck_abc123");
  EXPECT_EQ(revoked[1], "ck_def456");
}

TEST_F(FleetControllerTest, SelfRevocationTriggers) {
  // Relay id 7 revoked in the bundle.
  std::string inner =
      "{\"fleet_id\":\"company-a\",\"version\":1,"
      "\"policy\":{},"
      "\"revocations\":{\"relays\":["
      "{\"relay_id\":7}]}}";
  std::string bundle = MakeBundle(inner, sk_,
                                   "company-a", 1);
  auto st = FleetControllerApplyBundle(
      &fc_,
      reinterpret_cast<const uint8_t*>(bundle.data()),
      static_cast<int>(bundle.size()));
  EXPECT_EQ(st, FleetApplyStatus::kRevoked);
  EXPECT_TRUE(fc_.self_revoked.load());
}

}  // namespace
}  // namespace hyper_derp
