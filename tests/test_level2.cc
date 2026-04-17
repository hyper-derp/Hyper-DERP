/// @file test_level2.cc
/// @brief Unit tests for Level 2 upgrade/downgrade state
///   machine and ICE integration into the control plane.

#include "hyper_derp/control_plane.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/ice.h"
#include "hyper_derp/server.h"

#include <arpa/inet.h>

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

namespace {

Key MakeTestKey(uint8_t val) {
  Key k;
  std::memset(k.data(), val, kKeySize);
  return k;
}

}  // namespace

// -- ConnectionLevel enum tests ---------------------------

TEST(Level2Test, ConnectionLevelValues) {
  EXPECT_EQ(static_cast<uint8_t>(ConnectionLevel::kDerp),
            0);
  EXPECT_EQ(static_cast<uint8_t>(ConnectionLevel::kHd),
            1);
  EXPECT_EQ(
      static_cast<uint8_t>(ConnectionLevel::kDirect), 2);
}

// -- Level2Config defaults --------------------------------

TEST(Level2Test, Level2ConfigDefaults) {
  ServerConfig config;
  EXPECT_FALSE(config.level2.enabled);
  EXPECT_EQ(config.level2.stun_port, 3478);
  EXPECT_TRUE(config.level2.xdp_interface.empty());
  EXPECT_TRUE(config.level2.turn_realm.empty());
  EXPECT_EQ(config.level2.turn_max_allocations, 10000);
  EXPECT_EQ(config.level2.turn_default_lifetime, 600);
}

// -- ServerError Level2InitFailed -------------------------

TEST(Level2Test, ServerErrorLevel2InitFailed) {
  EXPECT_EQ(
      ServerErrorName(ServerError::Level2InitFailed),
      "Level2InitFailed");
}

// -- HdBuildPeerInfo frame builder ------------------------

TEST(Level2Test, BuildPeerInfoEmpty) {
  Key key = MakeTestKey(0xAA);
  uint8_t buf[256];
  int n = HdBuildPeerInfo(buf, key, nullptr, 0);
  EXPECT_EQ(n, kHdFrameHeaderSize + kKeySize);
  EXPECT_EQ(HdReadFrameType(buf),
            HdFrameType::kPeerInfo);
  EXPECT_EQ(HdReadPayloadLen(buf),
            static_cast<uint32_t>(kKeySize));
  EXPECT_EQ(std::memcmp(buf + kHdFrameHeaderSize,
                         key.data(), kKeySize), 0);
}

TEST(Level2Test, BuildPeerInfoWithCandidates) {
  Key key = MakeTestKey(0xBB);
  // Create a session with one local candidate and
  // serialize it.
  IceSession session{};
  session.nominated_pair = -1;
  IceAddLocalCandidate(&session,
                       IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  uint8_t cand_buf[256];
  int cand_len = IceSerializeCandidates(
      &session, cand_buf, sizeof(cand_buf));
  ASSERT_GT(cand_len, 0);

  uint8_t frame[512];
  int n = HdBuildPeerInfo(frame, key,
                          cand_buf, cand_len);
  int expected = kHdFrameHeaderSize + kKeySize + cand_len;
  EXPECT_EQ(n, expected);
  EXPECT_EQ(HdReadFrameType(frame),
            HdFrameType::kPeerInfo);
  EXPECT_EQ(HdReadPayloadLen(frame),
            static_cast<uint32_t>(kKeySize + cand_len));

  // Verify key is in the payload.
  EXPECT_EQ(
      std::memcmp(frame + kHdFrameHeaderSize,
                  key.data(), kKeySize), 0);
  // Verify candidate data follows the key.
  EXPECT_EQ(
      std::memcmp(frame + kHdFrameHeaderSize + kKeySize,
                  cand_buf, cand_len), 0);
}

// -- ICE session start with relay candidate ---------------

TEST(Level2Test, IceSessionWithRelaySrflx) {
  IceAgent agent;
  IceInit(&agent, htonl(0x0A000001), 3478);

  Key peer = MakeTestKey(0xCC);
  IceSession* s = IceStartSession(&agent, peer);
  ASSERT_NE(s, nullptr);

  IceAddLocalCandidate(
      s, IceCandidateType::kServerReflexive,
      htonl(0x0A000001), htons(3478));
  EXPECT_EQ(s->local_count, 1);
  EXPECT_EQ(s->local_candidates[0].type,
            IceCandidateType::kServerReflexive);

  IceCloseSession(&agent, peer);
}

// -- PeerInfo roundtrip through serialize/parse -----------

TEST(Level2Test, PeerInfoRoundtrip) {
  // Simulate: server serializes candidates into a
  // PeerInfo frame, then parses them back out.
  Key peer_key = MakeTestKey(0xDD);

  IceSession src{};
  src.nominated_pair = -1;
  IceAddLocalCandidate(&src, IceCandidateType::kHost,
                       htonl(0xC0A80101), htons(5000));
  IceAddLocalCandidate(
      &src, IceCandidateType::kServerReflexive,
      htonl(0x0A000001), htons(3478));

  // Serialize candidates.
  uint8_t cand_buf[256];
  int cand_len = IceSerializeCandidates(
      &src, cand_buf, sizeof(cand_buf));
  ASSERT_GT(cand_len, 0);

  // Build PeerInfo frame.
  uint8_t frame[512];
  int flen = HdBuildPeerInfo(frame, peer_key,
                             cand_buf, cand_len);
  ASSERT_GT(flen, 0);

  // Parse back: extract payload (skip HD header).
  const uint8_t* payload = frame + kHdFrameHeaderSize;
  int payload_len = flen - kHdFrameHeaderSize;

  // First 32 bytes = peer key.
  EXPECT_EQ(std::memcmp(payload, peer_key.data(),
                         kKeySize), 0);

  // Remaining bytes = candidate data.
  IceSession dst{};
  dst.nominated_pair = -1;
  int parsed = IceParseCandidates(
      &dst, payload + kKeySize,
      payload_len - kKeySize);
  EXPECT_EQ(parsed, 2);
  EXPECT_EQ(dst.remote_count, 2);
  EXPECT_EQ(dst.remote_candidates[0].type,
            IceCandidateType::kHost);
  EXPECT_EQ(dst.remote_candidates[1].type,
            IceCandidateType::kServerReflexive);
}

// -- ICE state machine full cycle -------------------------

TEST(Level2Test, IceStateMachineFullCycle) {
  IceAgent agent;
  IceInit(&agent, htonl(0x0A000001), 3478);

  Key peer_a = MakeTestKey(0x01);
  Key peer_b = MakeTestKey(0x02);

  // Start session for peer_b (from peer_a's perspective).
  IceSession* s = IceStartSession(&agent, peer_b);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->state, IceState::kGathering);

  // Add local candidate (relay's server-reflexive).
  IceAddLocalCandidate(
      s, IceCandidateType::kServerReflexive,
      htonl(0x0A000001), htons(3478));

  // Simulate receiving remote candidates from peer_b.
  IceAddRemoteCandidate(
      s, IceCandidateType::kServerReflexive,
      htonl(0x0A000002), htons(3478));

  // Form pairs and transition to checking.
  IceFormPairs(s);
  EXPECT_EQ(s->state, IceState::kChecking);
  EXPECT_EQ(s->pair_count, 1);

  // Get next check.
  IceCandidatePair* pair = IceNextCheck(s);
  ASSERT_NE(pair, nullptr);
  EXPECT_EQ(pair->in_progress, 1);

  // Simulate successful STUN response.
  uint8_t txn_id[12];
  std::memcpy(txn_id, pair->transaction_id, 12);
  bool nominated = IceProcessResponse(
      s, txn_id, htonl(0x0A000001), htons(3478));
  EXPECT_TRUE(nominated);
  EXPECT_EQ(s->state, IceState::kConnected);

  // Get nominated pair.
  const IceCandidatePair* nom = IceGetNominated(s);
  ASSERT_NE(nom, nullptr);
  EXPECT_EQ(nom->nominated, 1);

  // Close session (downgrade back to Level 1).
  IceCloseSession(&agent, peer_b);
  EXPECT_EQ(agent.session_count, 0);
  IceSession* found =
      IceFindSession(&agent, peer_b.data());
  EXPECT_EQ(found, nullptr);
}

// -- CpHandleHdPeerInfo with Level 2 disabled -------------

TEST(Level2Test, CpHandleHdPeerInfoDisabled) {
  // When Level 2 is not enabled, PeerInfo should be
  // silently ignored (no crash).
  ControlPlane cp{};
  cp.level2_enabled = false;
  cp.ice_agent = nullptr;

  uint8_t payload[64];
  std::memset(payload, 0, sizeof(payload));
  // Should not crash.
  CpHandleHdPeerInfo(&cp, 5, payload, sizeof(payload));
}

// -- CpHandleHdPeerInfo with session -----------------------

TEST(Level2Test, CpHandleHdPeerInfoWithSession) {
  IceAgent agent;
  IceInit(&agent, htonl(0x0A000001), 3478);

  Key peer_key = MakeTestKey(0xEE);
  IceSession* s = IceStartSession(&agent, peer_key);
  ASSERT_NE(s, nullptr);

  // Add local candidate so IceFormPairs can run.
  IceAddLocalCandidate(
      s, IceCandidateType::kServerReflexive,
      htonl(0x0A000001), htons(3478));

  ControlPlane cp{};
  cp.level2_enabled = true;
  cp.ice_agent = &agent;

  // Build a PeerInfo payload: [32B key][candidate_data].
  uint8_t payload[256];
  std::memcpy(payload, peer_key.data(), kKeySize);
  // Single host candidate: [1B count][1B type][4B ip]
  //   [2B port]
  payload[kKeySize] = 1;  // count = 1
  payload[kKeySize + 1] = static_cast<uint8_t>(
      IceCandidateType::kHost);
  uint32_t ip = htonl(0xC0A80102);
  std::memcpy(payload + kKeySize + 2, &ip, 4);
  uint16_t port = htons(6000);
  std::memcpy(payload + kKeySize + 6, &port, 2);
  int payload_len = kKeySize + 8;

  CpHandleHdPeerInfo(&cp, 5, payload, payload_len);

  // Session should now have 1 remote candidate and be
  // in checking state (since local_count > 0).
  EXPECT_EQ(s->remote_count, 1);
  EXPECT_EQ(s->state, IceState::kChecking);
  EXPECT_EQ(s->pair_count, 1);

  IceCloseSession(&agent, peer_key);
}

// -- CpHandleHdPeerInfo with short payload ----------------

TEST(Level2Test, CpHandleHdPeerInfoShort) {
  IceAgent agent;
  IceInit(&agent, htonl(0x0A000001), 3478);

  ControlPlane cp{};
  cp.level2_enabled = true;
  cp.ice_agent = &agent;

  // Payload shorter than kKeySize should be rejected.
  uint8_t payload[16];
  std::memset(payload, 0, sizeof(payload));
  CpHandleHdPeerInfo(&cp, 5, payload, sizeof(payload));
  // No crash, no session changes.
}

// -- CpHandleHdPeerInfo no matching session ---------------

TEST(Level2Test, CpHandleHdPeerInfoNoSession) {
  IceAgent agent;
  IceInit(&agent, htonl(0x0A000001), 3478);

  ControlPlane cp{};
  cp.level2_enabled = true;
  cp.ice_agent = &agent;

  // Valid payload but no matching session.
  Key peer_key = MakeTestKey(0xFF);
  uint8_t payload[kKeySize + 8];
  std::memcpy(payload, peer_key.data(), kKeySize);
  payload[kKeySize] = 1;
  payload[kKeySize + 1] = 0;
  uint32_t ip = htonl(0xC0A80103);
  std::memcpy(payload + kKeySize + 2, &ip, 4);
  uint16_t port = htons(7000);
  std::memcpy(payload + kKeySize + 6, &port, 2);

  CpHandleHdPeerInfo(&cp, 5, payload, sizeof(payload));
  // No crash. No sessions affected.
  EXPECT_EQ(agent.session_count, 0);
}

}  // namespace hyper_derp
