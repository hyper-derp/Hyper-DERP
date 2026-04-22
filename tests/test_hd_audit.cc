/// @file test_hd_audit.cc
/// @brief Unit tests for the routing-policy audit ring.

#include "hyper_derp/hd_audit.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
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

TEST(HdAuditFlusherTest, WritesLdJsonToFile) {
  char path[] = "/tmp/hd_audit_XXXXXX";
  int tfd = mkstemp(path);
  ASSERT_GE(tfd, 0);
  close(tfd);
  std::string file = path;

  HdAuditRing ring;
  HdAuditInit(&ring);
  HdAuditFlusher flusher;
  HdAuditFlusherStart(&flusher, &ring, file, 0, 3);

  Key c{}, t{};
  c[0] = 0x11;
  t[0] = 0x22;
  HdClientView cv;
  HdDecision d;
  d.mode = HdConnMode::kRelayed;
  HdAuditRecordDecision(&ring, c, t, cv, d);

  // Let the flusher pick it up.
  for (int i = 0; i < 50; i++) {
    usleep(10000);
    uint64_t idx = ring.flush_idx.load();
    if (idx >= 1) break;
  }
  HdAuditFlusherStop(&flusher);

  FILE* f = std::fopen(file.c_str(), "r");
  ASSERT_NE(f, nullptr);
  char buf[1024] = {};
  std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  std::string s = buf;
  EXPECT_NE(s.find("\"decision\":\"relayed\""),
            std::string::npos);
  EXPECT_EQ(s.back(), '\n');
  unlink(file.c_str());
}

TEST(HdAuditFlusherTest, RotatesAtThreshold) {
  char path[] = "/tmp/hd_audit_rot_XXXXXX";
  int tfd = mkstemp(path);
  ASSERT_GE(tfd, 0);
  close(tfd);
  std::string file = path;
  ::unlink(file.c_str());

  HdAuditRing ring;
  HdAuditInit(&ring);
  HdAuditFlusher flusher;
  // Tiny cap forces rotation after one record.
  HdAuditFlusherStart(&flusher, &ring, file, 256, 3);

  Key c{}, t{};
  HdClientView cv;
  HdDecision d;
  d.mode = HdConnMode::kDirect;
  for (int i = 0; i < 5; i++) {
    c[0] = static_cast<uint8_t>(i);
    HdAuditRecordDecision(&ring, c, t, cv, d);
  }
  for (int i = 0; i < 100; i++) {
    usleep(10000);
    if (ring.flush_idx.load() >= 5) break;
  }
  HdAuditFlusherStop(&flusher);

  struct stat st{};
  EXPECT_EQ(::stat(file.c_str(), &st), 0);
  std::string rotated = file + ".1";
  EXPECT_EQ(::stat(rotated.c_str(), &st), 0);

  ::unlink(file.c_str());
  ::unlink(rotated.c_str());
  ::unlink((file + ".2").c_str());
}

}  // namespace
}  // namespace hyper_derp
