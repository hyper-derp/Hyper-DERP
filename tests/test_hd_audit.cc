/// @file test_hd_audit.cc
/// @brief Unit tests for the routing-policy audit ring.

#include "hyper_derp/hd_audit.h"

#include <cstring>

#include <gtest/gtest.h>

#include "hyper_derp/hd_resolver.h"

namespace hyper_derp {
namespace {

TEST(HdAuditTest, RecordAndSnapshot) {
  HdAuditRing ring;
  HdAuditInit(&ring);

  Key c{};
  c[0] = 0x11;
  Key t{};
  t[0] = 0x22;
  HdClientView cv;
  cv.intent = HdIntent::kRequireRelay;
  HdDecision d;
  d.mode = HdConnMode::kRelayed;
  d.deny_reason = HdDenyReason::kNone;

  HdAuditRecordDecision(&ring, c, t, cv, d);

  HdAuditRecord out[4];
  int n = HdAuditSnapshot(&ring, out, 4);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(out[0].mode, HdConnMode::kRelayed);
  EXPECT_EQ(out[0].client_intent,
            HdIntent::kRequireRelay);
  EXPECT_EQ(out[0].client_key, c);
  EXPECT_EQ(out[0].target_key, t);
}

TEST(HdAuditTest, NewestFirst) {
  HdAuditRing ring;
  HdAuditInit(&ring);
  Key c{}, t{};
  HdClientView cv;
  HdDecision d;
  for (int i = 0; i < 5; i++) {
    c[0] = static_cast<uint8_t>(i);
    HdAuditRecordDecision(&ring, c, t, cv, d);
  }
  HdAuditRecord out[10];
  int n = HdAuditSnapshot(&ring, out, 10);
  ASSERT_EQ(n, 5);
  EXPECT_EQ(out[0].client_key[0], 4);
  EXPECT_EQ(out[4].client_key[0], 0);
}

TEST(HdAuditTest, JsonSchemaMatches) {
  HdAuditRecord rec{};
  rec.ts_ns = 0;  // epoch
  rec.client_key[0] = 0xAA;
  rec.target_key[0] = 0xBB;
  rec.client_intent = HdIntent::kPreferDirect;
  rec.mode = HdConnMode::kRelayed;
  rec.deny_reason = HdDenyReason::kPeerOverride;
  rec.allow_upgrade = true;
  rec.allow_downgrade = false;
  for (int i = 0; i < 5; i++) {
    rec.layer_allowed[i] = kModeAny;
  }
  rec.layer_allowed[static_cast<int>(HdLayer::kPeer)] =
      kModeRelayed;

  std::string j = HdAuditToJson(rec);
  EXPECT_NE(j.find("\"decision\":\"relayed\""),
            std::string::npos);
  EXPECT_NE(j.find("\"intent\":\"prefer_direct\""),
            std::string::npos);
  EXPECT_NE(j.find("\"reason\":\"peer_override\""),
            std::string::npos);
  EXPECT_NE(j.find("\"allow_upgrade\":true"),
            std::string::npos);
  EXPECT_NE(j.find("\"allow_downgrade\":false"),
            std::string::npos);
  EXPECT_NE(j.find("\"chain\":["), std::string::npos);
  EXPECT_NE(j.find("\"layer\":\"peer\""),
            std::string::npos);
  EXPECT_NE(j.find("\"layer\":\"client\""),
            std::string::npos);
  EXPECT_NE(j.find("ck_aa"), std::string::npos);
  EXPECT_NE(j.find("ck_bb"), std::string::npos);
}

TEST(HdAuditTest, JsonNoReasonWhenPermitted) {
  HdAuditRecord rec{};
  rec.mode = HdConnMode::kDirect;
  rec.deny_reason = HdDenyReason::kNone;
  std::string j = HdAuditToJson(rec);
  EXPECT_EQ(j.find("\"reason\""), std::string::npos);
}

}  // namespace
}  // namespace hyper_derp
