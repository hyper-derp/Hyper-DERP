/// @file test_types.cc
/// @brief Unit tests for types.h tuning constants and
///   adaptive backpressure thresholds.

#include "hyper_derp/types.h"

#include <gtest/gtest.h>

namespace hyper_derp {

// -- Adaptive backpressure threshold tests --

TEST(TypesTest, PressureHighFloorAtOnePeer) {
  // Even with 0 or 1 peer, threshold is at least
  // kPressurePerPeer.
  EXPECT_EQ(SendPressureHigh(0), kPressurePerPeer);
  EXPECT_EQ(SendPressureHigh(1), kPressurePerPeer);
}

TEST(TypesTest, PressureHighScalesLinearly) {
  EXPECT_EQ(SendPressureHigh(5),
            5 * kPressurePerPeer);
  EXPECT_EQ(SendPressureHigh(10),
            10 * kPressurePerPeer);
  // 25 * 2048 = 51200 > kSendPressureMax, so clamped.
  EXPECT_EQ(SendPressureHigh(15),
            15 * kPressurePerPeer);
}

TEST(TypesTest, PressureHighClampsAtMax) {
  // At 64+ peers (64 * 512 = 32768 = kSendPressureMax),
  // the threshold should be clamped.
  int peers_at_max = kSendPressureMax / kPressurePerPeer;
  EXPECT_EQ(SendPressureHigh(peers_at_max),
            kSendPressureMax);
  EXPECT_EQ(SendPressureHigh(peers_at_max + 1),
            kSendPressureMax);
  EXPECT_EQ(SendPressureHigh(500), kSendPressureMax);
  EXPECT_EQ(SendPressureHigh(1000), kSendPressureMax);
}

TEST(TypesTest, PressureLowIsFractionOfHigh) {
  for (int peers : {1, 5, 10, 25, 50, 100, 500}) {
    int high = SendPressureHigh(peers);
    int low = SendPressureLow(peers);
    EXPECT_EQ(low, high / PressureResumeDiv(peers))
        << "at peers=" << peers;
    EXPECT_LT(low, high) << "at peers=" << peers;
  }
}

TEST(TypesTest, PressureResumeDivScalesWithPeers) {
  EXPECT_EQ(PressureResumeDiv(1), 8);
  EXPECT_EQ(PressureResumeDiv(10), 8);
  EXPECT_EQ(PressureResumeDiv(12), 8);
  EXPECT_EQ(PressureResumeDiv(13), 6);
  EXPECT_EQ(PressureResumeDiv(24), 6);
  EXPECT_EQ(PressureResumeDiv(25), 4);
  EXPECT_EQ(PressureResumeDiv(100), 4);
}

TEST(TypesTest, PressureHighBelowSlabSize) {
  // Threshold must never exceed slab size (invariant
  // documented in types.h).
  for (int peers = 0; peers <= 2000; peers++) {
    EXPECT_LE(SendPressureHigh(peers), kSlabSize)
        << "at peers=" << peers;
  }
}

TEST(TypesTest, FivePeersGetsTightThreshold) {
  // At 5 peers, threshold = 5 * 2048 = 10240.
  // Below total queue max = 5 * 8192 = 40960.
  int high = SendPressureHigh(5);
  EXPECT_EQ(high, 5 * kPressurePerPeer);
  EXPECT_LT(high, 5 * kMaxSendQueueDepth);
}

// -- Constant sanity checks --

TEST(TypesTest, ConstantsPowerOfTwo) {
  auto is_pow2 = [](int v) { return v > 0 && (v & (v - 1)) == 0; };
  EXPECT_TRUE(is_pow2(kHtCapacity));
  EXPECT_TRUE(is_pow2(kCmdRingSize));
  EXPECT_TRUE(is_pow2(kXferSpscSize));
  EXPECT_TRUE(is_pow2(kUringQueueDepth));
  EXPECT_TRUE(is_pow2(kRecvDeferSize));
  EXPECT_TRUE(is_pow2(kPbufCount));
}

TEST(TypesTest, PressureMaxBelowSlabSize) {
  EXPECT_LT(kSendPressureMax, kSlabSize);
}

TEST(TypesTest, MaxSendsInflightFitsInRing) {
  // At kMaxWorkers * kMaxSendsInflight per peer, with a
  // reasonable peer count, SQEs should fit in the ring.
  // 250 peers * 16 inflight = 4000 < 4096.
  EXPECT_LE(250 * kMaxSendsInflight, kUringQueueDepth);
}

// -- Frame pool constant checks --

TEST(TypesTest, FramePoolCoversWireGuardMTU) {
  // Typical forwarded frame: 5B header + 32B key + 1400B.
  int typical = kFrameHeaderSize + kKeySize + 1400;
  EXPECT_LE(typical, kFramePoolBufSize);
}

TEST(TypesTest, FramePoolBufSizeIsPowerOfTwo) {
  EXPECT_GT(kFramePoolBufSize, 0);
  EXPECT_EQ(kFramePoolBufSize & (kFramePoolBufSize - 1), 0);
}

TEST(TypesTest, FramePoolCountIsPowerOfTwo) {
  EXPECT_GT(kFramePoolCount, 0);
  EXPECT_EQ(kFramePoolCount & (kFramePoolCount - 1), 0);
}

TEST(TypesTest, PeerProtocolDefaults) {
  EXPECT_EQ(static_cast<uint8_t>(PeerProtocol::kDerp), 0);
  EXPECT_EQ(static_cast<uint8_t>(PeerProtocol::kHd), 1);
}

}  // namespace hyper_derp
