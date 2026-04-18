/// @file test_ctl_channel.cc
/// @brief Tests for the ZMQ IPC control channel.

#include <gtest/gtest.h>
#include <sodium.h>

#include <zmq.hpp>
#include <string>

#include "hyper_derp/ctl_channel.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/types.h"

namespace hyper_derp {
namespace {

/// Minimal Ctx with no actual workers for unit testing.
static Ctx MakeTestCtx() {
  Ctx ctx{};
  ctx.num_workers = 0;
  return ctx;
}

static std::string ZmqReq(const char* endpoint,
                          const std::string& req) {
  zmq::context_t ctx(1);
  zmq::socket_t sock(ctx, zmq::socket_type::dealer);
  sock.set(zmq::sockopt::rcvtimeo, 2000);
  sock.set(zmq::sockopt::linger, 0);
  sock.connect(endpoint);

  sock.send(zmq::message_t(), zmq::send_flags::sndmore);
  sock.send(zmq::buffer(req), zmq::send_flags::none);

  zmq::message_t delim, resp;
  auto r1 = sock.recv(delim, zmq::recv_flags::none);
  if (!r1) return "";
  auto r2 = sock.recv(resp, zmq::recv_flags::none);
  if (!r2) return "";

  return std::string(
      static_cast<char*>(resp.data()), resp.size());
}

class CtlChannelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = MakeTestCtx();
    // Use a unique IPC path to avoid conflicts.
    snprintf(ipc_, sizeof(ipc_),
             "ipc:///tmp/hdtest-%d.sock", getpid());
    ch_ = CtlChannelStart(ipc_, &ctx_, nullptr);
    ASSERT_NE(ch_, nullptr);
    usleep(50000);  // Let ROUTER bind.
  }

  void TearDown() override {
    CtlChannelStop(ch_);
    // Clean up socket file.
    std::string path = std::string(ipc_ + 6);
    unlink(path.c_str());
  }

  Ctx ctx_{};
  CtlChannel* ch_ = nullptr;
  char ipc_[128];
};

TEST_F(CtlChannelTest, Status) {
  auto resp = ZmqReq(ipc_, "{\"cmd\":\"status\"}");
  ASSERT_FALSE(resp.empty());
  EXPECT_NE(resp.find("\"status\""), std::string::npos);
  EXPECT_NE(resp.find("\"ok\""), std::string::npos);
  EXPECT_NE(resp.find("\"workers\""), std::string::npos);
}

TEST_F(CtlChannelTest, PeersEmpty) {
  auto resp = ZmqReq(ipc_, "{\"cmd\":\"peers\"}");
  ASSERT_FALSE(resp.empty());
  EXPECT_NE(resp.find("\"count\":0"), std::string::npos);
}

TEST_F(CtlChannelTest, WorkersEmpty) {
  auto resp = ZmqReq(ipc_, "{\"cmd\":\"workers\"}");
  ASSERT_FALSE(resp.empty());
  EXPECT_NE(resp.find("\"workers\":[]"), std::string::npos);
}

TEST_F(CtlChannelTest, UnknownCmd) {
  auto resp = ZmqReq(ipc_, "{\"cmd\":\"bogus\"}");
  EXPECT_NE(resp.find("\"error\""), std::string::npos);
}

TEST_F(CtlChannelTest, MissingCmd) {
  auto resp = ZmqReq(ipc_, "{\"foo\":\"bar\"}");
  EXPECT_NE(resp.find("\"error\""), std::string::npos);
}

class CtlChannelHdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    sodium_init();
    ctx_ = MakeTestCtx();
    Key relay_key{};
    randombytes_buf(relay_key.data(), kKeySize);
    HdPeersInit(&peers_, relay_key,
                HdEnrollMode::kAutoApprove);
    snprintf(ipc_, sizeof(ipc_),
             "ipc:///tmp/hdtest-hd-%d.sock", getpid());
    ch_ = CtlChannelStart(ipc_, &ctx_, &peers_);
    ASSERT_NE(ch_, nullptr);
    usleep(50000);
  }

  void TearDown() override {
    CtlChannelStop(ch_);
    std::string path = std::string(ipc_ + 6);
    unlink(path.c_str());
  }

  Ctx ctx_{};
  HdPeerRegistry peers_{};
  CtlChannel* ch_ = nullptr;
  char ipc_[128];
};

TEST_F(CtlChannelHdTest, StatusShowsHd) {
  auto resp = ZmqReq(ipc_, "{\"cmd\":\"status\"}");
  EXPECT_NE(resp.find("\"hd_enabled\":true"),
            std::string::npos);
}

TEST_F(CtlChannelHdTest, PeersWithEnrolledPeer) {
  // Insert a peer directly.
  Key k{};
  randombytes_buf(k.data(), kKeySize);
  {
    std::lock_guard lock(peers_.mutex);
    HdPeersInsert(&peers_, k, 42);
  }

  auto resp = ZmqReq(ipc_, "{\"cmd\":\"peers\"}");
  EXPECT_NE(resp.find("\"count\":1"), std::string::npos);
  EXPECT_NE(resp.find("\"approved\""), std::string::npos);
}

TEST_F(CtlChannelHdTest, MultipleRequests) {
  // Verify the channel handles multiple sequential requests.
  for (int i = 0; i < 5; i++) {
    auto resp = ZmqReq(ipc_, "{\"cmd\":\"status\"}");
    ASSERT_FALSE(resp.empty()) << "request " << i;
    EXPECT_NE(resp.find("\"ok\""), std::string::npos);
  }
}

}  // namespace
}  // namespace hyper_derp
