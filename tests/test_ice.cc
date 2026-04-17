/// @file test_ice.cc
/// @brief Unit tests for ICE agent.

#include "hyper_derp/ice.h"

#include <arpa/inet.h>
#include <sodium.h>

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

// -- Helpers ------------------------------------------------

namespace {

/// Create a test Key filled with a single byte value.
Key MakeTestKey(uint8_t val) {
  Key k;
  std::memset(k.data(), val, kKeySize);
  return k;
}

}  // namespace

// -- IceCalcPriority tests ----------------------------------

TEST(IceTest, CalcPriorityHostHighest) {
  uint32_t host = IceCalcPriority(
      IceCandidateType::kHost, 65535, 1);
  uint32_t srflx = IceCalcPriority(
      IceCandidateType::kServerReflexive, 65535, 1);
  uint32_t relay = IceCalcPriority(
      IceCandidateType::kRelay, 65535, 1);
  EXPECT_GT(host, srflx);
  EXPECT_GT(srflx, relay);
}

TEST(IceTest, CalcPriorityValues) {
  // host: 126 * 2^24 + 65535 * 2^8 + (256 - 1)
  //     = 2113929216 + 16776960 + 255 = 2130706431
  uint32_t host = IceCalcPriority(
      IceCandidateType::kHost, 65535, 1);
  EXPECT_EQ(host, 2130706431u);
  // srflx: 100 * 2^24 + 65535 * 2^8 + 255
  //      = 1677721600 + 16776960 + 255 = 1694498815
  uint32_t srflx = IceCalcPriority(
      IceCandidateType::kServerReflexive, 65535, 1);
  EXPECT_EQ(srflx, 1694498815u);
  // relay: 0 * 2^24 + 65535 * 2^8 + 255 = 16777215
  uint32_t relay = IceCalcPriority(
      IceCandidateType::kRelay, 65535, 1);
  EXPECT_EQ(relay, 16777215u);
}

// -- IceCalcPairPriority tests ------------------------------

TEST(IceTest, CalcPairPriority) {
  uint32_t g = 2130706431u;  // host priority
  uint32_t d = 1694498815u;  // srflx priority
  uint64_t pp = IceCalcPairPriority(g, d);
  // min(g,d) = d, max(g,d) = g
  // pp = d * 2^32 + g * 2 + 1
  uint64_t expected =
      (static_cast<uint64_t>(d) << 32) |
      (static_cast<uint64_t>(g) << 1) | 1;
  EXPECT_EQ(pp, expected);
}

TEST(IceTest, CalcPairPriorityEqual) {
  uint32_t p = 2130706431u;
  uint64_t pp = IceCalcPairPriority(p, p);
  // G == D so: min=p, max=p, G>D = 0
  uint64_t expected =
      (static_cast<uint64_t>(p) << 32) |
      (static_cast<uint64_t>(p) << 1);
  EXPECT_EQ(pp, expected);
}

// -- IceAddCandidate tests ----------------------------------

TEST(IceTest, AddLocalCandidate) {
  IceSession s{};
  s.nominated_pair = -1;
  EXPECT_TRUE(IceAddLocalCandidate(
      &s, IceCandidateType::kHost,
      htonl(0xC0A80101), htons(5000)));
  EXPECT_EQ(s.local_count, 1);
  EXPECT_EQ(s.local_candidates[0].ip,
            htonl(0xC0A80101));
  EXPECT_EQ(s.local_candidates[0].port, htons(5000));
  EXPECT_GT(s.local_candidates[0].priority, 0u);
}

TEST(IceTest, AddRemoteCandidate) {
  IceSession s{};
  s.nominated_pair = -1;
  EXPECT_TRUE(IceAddRemoteCandidate(
      &s, IceCandidateType::kServerReflexive,
      htonl(0x0A000001), htons(6000)));
  EXPECT_EQ(s.remote_count, 1);
  EXPECT_EQ(s.remote_candidates[0].type,
            IceCandidateType::kServerReflexive);
}

TEST(IceTest, AddCandidateOverflow) {
  IceSession s{};
  s.nominated_pair = -1;
  for (int i = 0; i < kIceMaxCandidates; ++i) {
    EXPECT_TRUE(IceAddLocalCandidate(
        &s, IceCandidateType::kHost,
        htonl(0xC0A80100 + i), htons(5000 + i)));
  }
  EXPECT_FALSE(IceAddLocalCandidate(
      &s, IceCandidateType::kHost,
      htonl(0xC0A80200), htons(9999)));
  EXPECT_EQ(s.local_count, kIceMaxCandidates);
}

// -- IceFormPairs tests -------------------------------------

TEST(IceTest, FormPairs) {
  IceSession s{};
  s.nominated_pair = -1;
  // 2 local: host + srflx
  IceAddLocalCandidate(&s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddLocalCandidate(
      &s, IceCandidateType::kServerReflexive,
      htonl(0x0A000001), htons(5001));
  // 2 remote: host + relay
  IceAddRemoteCandidate(&s, IceCandidateType::kHost,
                        htonl(0xC0A80102), htons(6000));
  IceAddRemoteCandidate(&s, IceCandidateType::kRelay,
                        htonl(0x0A000002), htons(6001));
  IceFormPairs(&s);
  EXPECT_EQ(s.pair_count, 4);
  EXPECT_EQ(s.state, IceState::kChecking);
  // Verify sorted descending.
  for (int i = 1; i < s.pair_count; ++i) {
    EXPECT_GE(s.pairs[i - 1].priority,
              s.pairs[i].priority);
  }
  // Highest pair should be host-host.
  EXPECT_EQ(s.pairs[0].local.type,
            IceCandidateType::kHost);
  EXPECT_EQ(s.pairs[0].remote.type,
            IceCandidateType::kHost);
}

// -- IceNextCheck tests -------------------------------------

TEST(IceTest, NextCheck) {
  IceSession s{};
  s.nominated_pair = -1;
  IceAddLocalCandidate(&s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddRemoteCandidate(&s, IceCandidateType::kHost,
                        htonl(0xC0A80102), htons(6000));
  IceFormPairs(&s);
  ASSERT_EQ(s.pair_count, 1);

  IceCandidatePair* p = IceNextCheck(&s);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->in_progress, 1);

  // Calling again returns nullptr (only one pair, and
  // it's in_progress).
  IceCandidatePair* p2 = IceNextCheck(&s);
  EXPECT_EQ(p2, nullptr);
}

TEST(IceTest, NextCheckReturnsHighestPriority) {
  IceSession s{};
  s.nominated_pair = -1;
  IceAddLocalCandidate(&s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddLocalCandidate(
      &s, IceCandidateType::kServerReflexive,
      htonl(0x0A000001), htons(5001));
  IceAddRemoteCandidate(&s, IceCandidateType::kHost,
                        htonl(0xC0A80102), htons(6000));
  IceFormPairs(&s);
  ASSERT_EQ(s.pair_count, 2);

  IceCandidatePair* p = IceNextCheck(&s);
  ASSERT_NE(p, nullptr);
  // First check should be highest priority (host-host).
  EXPECT_EQ(p->local.type, IceCandidateType::kHost);
}

// -- IceProcessResponse tests -------------------------------

TEST(IceTest, ProcessResponse) {
  IceSession s{};
  s.nominated_pair = -1;
  IceAddLocalCandidate(&s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddRemoteCandidate(&s, IceCandidateType::kHost,
                        htonl(0xC0A80102), htons(6000));
  IceFormPairs(&s);

  IceCandidatePair* p = IceNextCheck(&s);
  ASSERT_NE(p, nullptr);

  // Save the transaction ID.
  uint8_t txn_id[12];
  std::memcpy(txn_id, p->transaction_id, 12);

  bool nominated = IceProcessResponse(
      &s, txn_id, htonl(0xC0A80101), htons(5000));
  EXPECT_TRUE(nominated);
  EXPECT_EQ(p->succeeded, 1);
  EXPECT_EQ(p->in_progress, 0);
  EXPECT_EQ(p->nominated, 1);
  EXPECT_EQ(s.state, IceState::kConnected);
  EXPECT_EQ(s.nominated_pair, 0);
}

TEST(IceTest, ProcessResponseWrongTxn) {
  IceSession s{};
  s.nominated_pair = -1;
  IceAddLocalCandidate(&s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddRemoteCandidate(&s, IceCandidateType::kHost,
                        htonl(0xC0A80102), htons(6000));
  IceFormPairs(&s);

  IceCandidatePair* p = IceNextCheck(&s);
  ASSERT_NE(p, nullptr);

  // Use a wrong transaction ID.
  uint8_t wrong_id[12];
  std::memset(wrong_id, 0xFF, 12);

  bool nominated = IceProcessResponse(
      &s, wrong_id, htonl(0xC0A80101), htons(5000));
  EXPECT_FALSE(nominated);
  // Pair should still be in_progress.
  EXPECT_EQ(p->in_progress, 1);
  EXPECT_EQ(p->succeeded, 0);
}

// -- IceCheckFailed tests -----------------------------------

TEST(IceTest, CheckFailed) {
  IceSession s{};
  s.nominated_pair = -1;
  IceAddLocalCandidate(&s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddRemoteCandidate(&s, IceCandidateType::kHost,
                        htonl(0xC0A80102), htons(6000));
  IceAddRemoteCandidate(
      &s, IceCandidateType::kServerReflexive,
      htonl(0x0A000002), htons(6001));
  IceFormPairs(&s);
  ASSERT_EQ(s.pair_count, 2);

  // Check the first pair (highest priority) and fail it.
  IceCandidatePair* p1 = IceNextCheck(&s);
  ASSERT_NE(p1, nullptr);
  uint8_t txn1[12];
  std::memcpy(txn1, p1->transaction_id, 12);
  IceCheckFailed(&s, txn1);
  EXPECT_EQ(p1->failed, 1);
  EXPECT_EQ(p1->in_progress, 0);

  // Next check should return the second pair.
  IceCandidatePair* p2 = IceNextCheck(&s);
  ASSERT_NE(p2, nullptr);
  EXPECT_NE(p2, p1);
}

TEST(IceTest, AllChecksFailed) {
  IceSession s{};
  s.nominated_pair = -1;
  IceAddLocalCandidate(&s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddRemoteCandidate(&s, IceCandidateType::kHost,
                        htonl(0xC0A80102), htons(6000));
  IceFormPairs(&s);

  IceCandidatePair* p = IceNextCheck(&s);
  ASSERT_NE(p, nullptr);
  uint8_t txn[12];
  std::memcpy(txn, p->transaction_id, 12);
  IceCheckFailed(&s, txn);
  EXPECT_EQ(s.state, IceState::kFailed);
}

// -- Serialize/Parse roundtrip test -------------------------

TEST(IceTest, SerializeRoundtrip) {
  IceSession src{};
  src.nominated_pair = -1;
  IceAddLocalCandidate(&src, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddLocalCandidate(
      &src, IceCandidateType::kServerReflexive,
      htonl(0x0A000001), htons(5001));
  IceAddLocalCandidate(&src, IceCandidateType::kRelay,
                       htonl(0x0A000002), htons(5002));

  uint8_t buf[256];
  int n = IceSerializeCandidates(&src, buf, sizeof(buf));
  EXPECT_EQ(n, 1 + 3 * 7);

  // Parse into a fresh session as remote candidates.
  IceSession dst{};
  dst.nominated_pair = -1;
  int parsed = IceParseCandidates(&dst, buf, n);
  EXPECT_EQ(parsed, 3);
  EXPECT_EQ(dst.remote_count, 3);

  // Verify all candidates match.
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(dst.remote_candidates[i].type,
              src.local_candidates[i].type);
    EXPECT_EQ(dst.remote_candidates[i].ip,
              src.local_candidates[i].ip);
    EXPECT_EQ(dst.remote_candidates[i].port,
              src.local_candidates[i].port);
  }
}

TEST(IceTest, SerializeBufferTooSmall) {
  IceSession s{};
  s.nominated_pair = -1;
  IceAddLocalCandidate(&s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  uint8_t buf[4];  // Too small for 1 + 7 = 8 bytes.
  int n = IceSerializeCandidates(&s, buf, sizeof(buf));
  EXPECT_EQ(n, 0);
}

TEST(IceTest, ParseTruncated) {
  // Single candidate needs 1 + 7 = 8 bytes.
  uint8_t buf[4] = {1, 0, 0, 0};
  IceSession s{};
  s.nominated_pair = -1;
  int parsed = IceParseCandidates(&s, buf, sizeof(buf));
  EXPECT_EQ(parsed, -1);
}

// -- Session lifecycle test ---------------------------------

TEST(IceTest, SessionLifecycle) {
  IceAgent agent;
  IceInit(&agent, htonl(0x0A000001), 3478);

  Key peer_key = MakeTestKey(0x42);
  IceSession* s = IceStartSession(&agent, peer_key);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->state, IceState::kGathering);
  EXPECT_EQ(agent.session_count, 1);

  // Find the session.
  IceSession* found =
      IceFindSession(&agent, peer_key.data());
  EXPECT_EQ(found, s);

  // Gather local candidate.
  IceAddLocalCandidate(s, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));

  // Receive remote candidate.
  IceAddRemoteCandidate(s, IceCandidateType::kHost,
                        htonl(0xC0A80102), htons(6000));

  // Form pairs and check.
  IceFormPairs(s);
  EXPECT_EQ(s->state, IceState::kChecking);

  IceCandidatePair* p = IceNextCheck(s);
  ASSERT_NE(p, nullptr);

  // Simulate success.
  uint8_t txn_id[12];
  std::memcpy(txn_id, p->transaction_id, 12);
  bool nominated = IceProcessResponse(
      s, txn_id, htonl(0xC0A80101), htons(5000));
  EXPECT_TRUE(nominated);
  EXPECT_EQ(s->state, IceState::kConnected);

  // Get nominated pair.
  const IceCandidatePair* nom = IceGetNominated(s);
  ASSERT_NE(nom, nullptr);
  EXPECT_EQ(nom->nominated, 1);

  // Close session.
  IceCloseSession(&agent, peer_key);
  EXPECT_EQ(agent.session_count, 0);

  // Find should return nullptr.
  found = IceFindSession(&agent, peer_key.data());
  EXPECT_EQ(found, nullptr);
}

TEST(IceTest, SessionTableFull) {
  IceAgent agent;
  IceInit(&agent, htonl(0x0A000001), 3478);
  for (int i = 0; i < kIceMaxSessions; ++i) {
    Key k = MakeTestKey(static_cast<uint8_t>(i));
    EXPECT_NE(IceStartSession(&agent, k), nullptr);
  }
  // Table full.
  Key overflow_key = MakeTestKey(0xFF);
  EXPECT_EQ(IceStartSession(&agent, overflow_key),
            nullptr);
}

}  // namespace hyper_derp
