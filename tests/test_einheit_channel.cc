/// @file test_einheit_channel.cc
/// @brief Integration test for the einheit control channel.
///
/// Starts a relay with einheit enabled, opens a ZMQ REQ client,
/// speaks the einheit envelope protocol directly (the HD adapter
/// + framework transport are tested separately), and verifies
/// the shape of the responses for representative commands.

#include "harness.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <sodium.h>
#include <zmq.hpp>

#include "hyper_derp/einheit_protocol.h"
#include "hyper_derp/hd_client.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {
namespace {

class EinheitChannelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    randombytes_buf(relay_key_.data(), kKeySize);
    port_ = test::FindFreePort();
    ASSERT_NE(port_, 0);
    char ipc_path[128];
    std::snprintf(
        ipc_path, sizeof(ipc_path),
        "ipc:///tmp/einheit-hd-%d-%d.ctl", getpid(),
        port_);
    ctl_endpoint_ = ipc_path;
    relay_pid_ =
        test::StartHdRelay(port_, 1, relay_key_, 0, 0, "",
                           0, "", "", ctl_endpoint_.c_str());
    ASSERT_GT(relay_pid_, 0);
    ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0);
    // Channel bind is in a second thread; short settle.
    usleep(300000);
  }

  void TearDown() override {
    if (relay_pid_ > 0) test::StopRelay(relay_pid_);
  }

  auto SendRequest(const einheit::Request& req)
      -> einheit::Response {
    zmq::context_t ctx{1};
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.set(zmq::sockopt::linger, 0);
    sock.set(zmq::sockopt::rcvtimeo, 3000);
    sock.connect(ctl_endpoint_);

    auto encoded = einheit::EncodeRequest(req);
    EXPECT_TRUE(encoded.has_value());
    zmq::message_t msg(encoded->data(), encoded->size());
    sock.send(msg, zmq::send_flags::none);

    zmq::message_t reply;
    auto got = sock.recv(reply, zmq::recv_flags::none);
    EXPECT_TRUE(got.has_value());
    std::span<const std::uint8_t> view(
        static_cast<const std::uint8_t*>(reply.data()),
        reply.size());
    auto decoded = einheit::DecodeResponse(view);
    EXPECT_TRUE(decoded.has_value())
        << (decoded ? "" : decoded.error().message);
    return decoded.value_or(einheit::Response{});
  }

  Key relay_key_{};
  uint16_t port_ = 0;
  std::string ctl_endpoint_;
  pid_t relay_pid_ = -1;
};

TEST_F(EinheitChannelTest, ShowStatusAnswers) {
  einheit::Request req;
  req.id = "t1";
  req.command = "show_status";
  auto resp = SendRequest(req);
  EXPECT_EQ(resp.id, "t1");
  EXPECT_EQ(resp.status, einheit::ResponseStatus::kOk)
      << (resp.error ? resp.error->message : "");
  const std::string body(resp.data.begin(),
                          resp.data.end());
  EXPECT_NE(body.find("status=ok"), std::string::npos);
  EXPECT_NE(body.find("workers="), std::string::npos);
  EXPECT_NE(body.find("hd_enabled="), std::string::npos);
}

TEST_F(EinheitChannelTest, ShowPeersWithOnePeer) {
  // Enroll one peer via the existing HD client path.
  uint8_t pub[kKeySize], priv[kKeySize];
  crypto_box_keypair(pub, priv);
  HdClient c;
  HdClientInitWithKeys(&c, pub, priv, relay_key_);
  ASSERT_TRUE(
      test::ConnectHdClient(&c, "127.0.0.1", port_)
          .has_value());
  ASSERT_TRUE(HdClientUpgrade(&c).has_value());
  ASSERT_TRUE(HdClientEnroll(&c).has_value());
  usleep(100000);

  einheit::Request req;
  req.id = "t2";
  req.command = "show_peers";
  auto resp = SendRequest(req);
  EXPECT_EQ(resp.status, einheit::ResponseStatus::kOk);
  const std::string body(resp.data.begin(),
                          resp.data.end());
  EXPECT_NE(body.find("peer.0.key=ck_"),
            std::string::npos);
  EXPECT_NE(body.find("peer.0.state="),
            std::string::npos);
  EXPECT_NE(body.find("peer.count=1"),
            std::string::npos);
  HdClientClose(&c);
}

TEST_F(EinheitChannelTest, UnknownCommandErrors) {
  einheit::Request req;
  req.id = "t3";
  req.command = "this_is_not_a_handler";
  auto resp = SendRequest(req);
  EXPECT_EQ(resp.status, einheit::ResponseStatus::kError);
  ASSERT_TRUE(resp.error.has_value());
  EXPECT_EQ(resp.error->code, "unknown_command");
}

TEST_F(EinheitChannelTest, RelayInitReturnsKey) {
  einheit::Request req;
  req.id = "t4";
  req.command = "relay_init";
  auto resp = SendRequest(req);
  EXPECT_EQ(resp.status, einheit::ResponseStatus::kOk);
  const std::string body(resp.data.begin(),
                          resp.data.end());
  EXPECT_NE(body.find("relay_key="), std::string::npos);
  EXPECT_NE(body.find("relay_key_str=rk_"),
            std::string::npos);
}

TEST_F(EinheitChannelTest, PubSocketStreamsMetrics) {
  // Re-launch a relay with the PUB endpoint enabled; the
  // default SetUp() skips it because of the ipc-path
  // collision concern on shared test runners.
  test::StopRelay(relay_pid_);
  relay_pid_ = -1;

  char ipc_ctl[128], ipc_pub[128];
  std::snprintf(ipc_ctl, sizeof(ipc_ctl),
                "ipc:///tmp/einheit-hd-%d-pub.ctl",
                getpid());
  std::snprintf(ipc_pub, sizeof(ipc_pub),
                "ipc:///tmp/einheit-hd-%d-pub.pub",
                getpid());
  ctl_endpoint_ = ipc_ctl;

  port_ = test::FindFreePort();
  ASSERT_NE(port_, 0);
  relay_pid_ = test::StartHdRelay(
      port_, 1, relay_key_, 0, 0, "", 0, "", "",
      ipc_ctl, ipc_pub);
  ASSERT_GT(relay_pid_, 0);
  ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0);
  // Channel + PUB bind happen in the child; wait for it.
  usleep(600000);

  zmq::context_t ctx{1};
  zmq::socket_t sub(ctx, zmq::socket_type::sub);
  sub.set(zmq::sockopt::rcvtimeo, 3000);
  sub.set(zmq::sockopt::subscribe, "state.");
  sub.connect(ipc_pub);

  // Give SUB a moment to complete its handshake.
  usleep(300000);

  zmq::message_t topic;
  auto got = sub.recv(topic, zmq::recv_flags::none);
  ASSERT_TRUE(got.has_value())
      << "expected a metrics event within 3s";
  std::string topic_str(
      static_cast<const char*>(topic.data()),
      topic.size());
  EXPECT_TRUE(topic_str.starts_with("state.metrics."));

  zmq::message_t body;
  got = sub.recv(body, zmq::recv_flags::none);
  ASSERT_TRUE(got.has_value());

  std::span<const std::uint8_t> view(
      static_cast<const std::uint8_t*>(body.data()),
      body.size());
  auto decoded = einheit::DecodeEventBody(topic_str, view);
  ASSERT_TRUE(decoded.has_value())
      << decoded.error().message;
  EXPECT_EQ(decoded->topic, topic_str);
  EXPECT_FALSE(decoded->timestamp.empty());
}

TEST_F(EinheitChannelTest, PeerApproveRejectsBadKey) {
  einheit::Request req;
  req.id = "t5";
  req.command = "peer_approve";
  req.args = {"not-a-key"};
  auto resp = SendRequest(req);
  EXPECT_EQ(resp.status, einheit::ResponseStatus::kError);
  ASSERT_TRUE(resp.error.has_value());
  EXPECT_EQ(resp.error->code, "invalid_key");
}

}  // namespace
}  // namespace hyper_derp
