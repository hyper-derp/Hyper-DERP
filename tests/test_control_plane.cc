/// @file test_control_plane.cc
/// @brief Unit tests for the DERP control plane.

#include "hyper_derp/control_plane.h"

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

// Helper: fill a key with a single byte value.
static Key MakeKey(uint8_t val) {
  Key key;
  memset(key.data(), val, kKeySize);
  return key;
}

class ControlPlaneTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = {};
    ctx_.num_workers = 0;
    CpInit(&cp_, &ctx_);
  }

  void TearDown() override {
    CpDestroy(&cp_);
  }

  ControlPlane cp_;
  Ctx ctx_;
};

TEST_F(ControlPlaneTest, PeerConnectAndLookup) {
  Key key = MakeKey(0xAA);

  CpOnPeerConnect(&cp_, key, 10);
  EXPECT_EQ(cp_.peer_count, 1);

  // fd_map should point to the entry.
  ASSERT_NE(cp_.fd_map[10], nullptr);
  EXPECT_EQ(cp_.fd_map[10]->key, key);
  EXPECT_EQ(cp_.fd_map[10]->fd, 10);
}

TEST_F(ControlPlaneTest, PeerGoneRemovesEntry) {
  Key key = MakeKey(0xBB);

  CpOnPeerConnect(&cp_, key, 20);
  EXPECT_EQ(cp_.peer_count, 1);

  CpOnPeerGone(&cp_, key, 20,
               PeerGoneReason::kDisconnected);
  EXPECT_EQ(cp_.peer_count, 0);
  EXPECT_EQ(cp_.fd_map[20], nullptr);
}

TEST_F(ControlPlaneTest, PeerReconnectUpdates) {
  Key key = MakeKey(0xCC);

  CpOnPeerConnect(&cp_, key, 30);
  EXPECT_EQ(cp_.peer_count, 1);
  EXPECT_EQ(cp_.fd_map[30]->fd, 30);

  // Reconnect with a new fd.
  CpOnPeerConnect(&cp_, key, 31);
  EXPECT_EQ(cp_.peer_count, 1);
  EXPECT_EQ(cp_.fd_map[30], nullptr);
  ASSERT_NE(cp_.fd_map[31], nullptr);
  EXPECT_EQ(cp_.fd_map[31]->fd, 31);
}

TEST_F(ControlPlaneTest, MultiplePeers) {
  for (int i = 0; i < 10; i++) {
    Key key = MakeKey(static_cast<uint8_t>(i + 1));
    CpOnPeerConnect(&cp_, key, 100 + i);
  }
  EXPECT_EQ(cp_.peer_count, 10);

  // Remove a few.
  for (int i = 0; i < 3; i++) {
    Key key = MakeKey(static_cast<uint8_t>(i + 1));
    CpOnPeerGone(&cp_, key, 100 + i,
                 PeerGoneReason::kDisconnected);
  }
  EXPECT_EQ(cp_.peer_count, 7);
}

TEST_F(ControlPlaneTest, WatcherGoneOnDisconnect) {
  Key key = MakeKey(0xDD);
  CpOnPeerConnect(&cp_, key, 40);

  // Simulate WatchConns from this peer.
  CpProcessFrame(&cp_, 40, FrameType::kWatchConns,
                  nullptr, 0);
  EXPECT_EQ(cp_.watcher_count, 1);

  // Peer disconnects — watcher should be removed.
  CpOnPeerGone(&cp_, key, 40,
               PeerGoneReason::kDisconnected);
  EXPECT_EQ(cp_.watcher_count, 0);
}

TEST_F(ControlPlaneTest, DuplicateWatchConnIgnored) {
  Key key = MakeKey(0xEE);
  CpOnPeerConnect(&cp_, key, 50);

  CpProcessFrame(&cp_, 50, FrameType::kWatchConns,
                  nullptr, 0);
  CpProcessFrame(&cp_, 50, FrameType::kWatchConns,
                  nullptr, 0);
  EXPECT_EQ(cp_.watcher_count, 1);
}

TEST_F(ControlPlaneTest, NotePreferred) {
  Key key = MakeKey(0xFF);
  CpOnPeerConnect(&cp_, key, 60);

  uint8_t payload = 1;
  CpProcessFrame(&cp_, 60, FrameType::kNotePreferred,
                  &payload, 1);
  EXPECT_EQ(cp_.fd_map[60]->preferred, 1);

  payload = 0;
  CpProcessFrame(&cp_, 60, FrameType::kNotePreferred,
                  &payload, 1);
  EXPECT_EQ(cp_.fd_map[60]->preferred, 0);
}

TEST_F(ControlPlaneTest, PipeMessageParsing) {
  // Verify the pipe header constants match the format:
  // [4B fd][1B type][4B len]
  EXPECT_EQ(kPipeMsgHeader, 9);
}

}  // namespace hyper_derp
