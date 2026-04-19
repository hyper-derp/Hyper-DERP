/// @file test_wg_config.cc
/// @brief Tests for WG daemon YAML config loader.

#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>

#include "hd/wg/wg_config.h"

namespace hyper_derp {
namespace {

class WgConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    snprintf(path_, sizeof(path_),
             "/tmp/hdwg-test-%d.yaml", getpid());
  }

  void TearDown() override {
    unlink(path_);
  }

  void WriteFile(const char* content) {
    FILE* f = fopen(path_, "w");
    ASSERT_NE(f, nullptr);
    fputs(content, f);
    fclose(f);
  }

  char path_[128];
};

TEST_F(WgConfigTest, FullConfig) {
  WriteFile(
      "relay:\n"
      "  host: \"10.50.0.2\"\n"
      "  port: 3341\n"
      "  key: \"aabb\"\n"
      "wireguard:\n"
      "  private_key: \"ccdd\"\n"
      "  interface: wg1\n"
      "  listen_port: 51999\n"
      "tunnel:\n"
      "  cidr: \"10.99.0.1/24\"\n"
      "proxy:\n"
      "  port: 51888\n"
      "stun:\n"
      "  server: \"stun.example.com:3478\"\n"
      "  timeout_ms: 3000\n"
      "keepalive: 15\n"
      "log_level: debug\n");

  WgDaemonConfig cfg;
  ASSERT_TRUE(WgLoadConfig(path_, &cfg));

  EXPECT_EQ(cfg.relay_host, "10.50.0.2");
  EXPECT_EQ(cfg.relay_port, 3341);
  EXPECT_EQ(cfg.relay_key_hex, "aabb");
  EXPECT_EQ(cfg.wg_private_key_hex, "ccdd");
  EXPECT_EQ(cfg.wg_interface, "wg1");
  EXPECT_EQ(cfg.wg_listen_port, 51999);
  EXPECT_EQ(cfg.tunnel_cidr, "10.99.0.1/24");
  EXPECT_EQ(cfg.proxy_port, 51888);
  EXPECT_EQ(cfg.stun_server, "stun.example.com:3478");
  EXPECT_EQ(cfg.stun_timeout_ms, 3000);
  EXPECT_EQ(cfg.keepalive_secs, 15);
  EXPECT_EQ(cfg.log_level, "debug");
}

TEST_F(WgConfigTest, DefaultValues) {
  WriteFile(
      "relay:\n"
      "  host: \"1.2.3.4\"\n"
      "  key: \"ff\"\n"
      "wireguard:\n"
      "  private_key: \"ee\"\n"
      "tunnel:\n"
      "  cidr: \"10.0.0.1/24\"\n");

  WgDaemonConfig cfg;
  ASSERT_TRUE(WgLoadConfig(path_, &cfg));

  EXPECT_EQ(cfg.relay_port, 3341);
  EXPECT_EQ(cfg.wg_interface, "wg0");
  EXPECT_EQ(cfg.wg_listen_port, 51820);
  EXPECT_EQ(cfg.proxy_port, 51821);
  EXPECT_EQ(cfg.keepalive_secs, 25);
  EXPECT_EQ(cfg.stun_server, "");
}

TEST_F(WgConfigTest, MissingFile) {
  WgDaemonConfig cfg;
  EXPECT_FALSE(WgLoadConfig("/tmp/nonexistent.yaml", &cfg));
}

TEST_F(WgConfigTest, EmptyFile) {
  WriteFile("");
  WgDaemonConfig cfg;
  // Empty YAML is valid but has no fields — defaults apply.
  // ryml may parse it or not, but shouldn't crash.
  WgLoadConfig(path_, &cfg);
  EXPECT_EQ(cfg.relay_host, "");
}

}  // namespace
}  // namespace hyper_derp
