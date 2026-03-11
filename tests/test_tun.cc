/// @file test_tun.cc
/// @brief Unit tests for TUN device and CIDR parsing.

#include "hyper_derp/tun.h"

#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>

#include <gtest/gtest.h>

namespace hyper_derp {

TEST(TunTest, ParseCidrValid) {
  uint32_t addr;
  int prefix;
  ASSERT_EQ(ParseCidr("10.99.0.1/24", &addr, &prefix), 0);
  EXPECT_EQ(prefix, 24);

  struct in_addr in;
  in.s_addr = addr;
  EXPECT_STREQ(inet_ntoa(in), "10.99.0.1");
}

TEST(TunTest, ParseCidrSlash32) {
  uint32_t addr;
  int prefix;
  ASSERT_EQ(ParseCidr("192.168.1.1/32", &addr, &prefix),
            0);
  EXPECT_EQ(prefix, 32);
}

TEST(TunTest, ParseCidrSlash0) {
  uint32_t addr;
  int prefix;
  ASSERT_EQ(ParseCidr("0.0.0.0/0", &addr, &prefix), 0);
  EXPECT_EQ(prefix, 0);
}

TEST(TunTest, ParseCidrNoSlash) {
  uint32_t addr;
  int prefix;
  EXPECT_EQ(ParseCidr("10.0.0.1", &addr, &prefix), -1);
}

TEST(TunTest, ParseCidrBadAddr) {
  uint32_t addr;
  int prefix;
  EXPECT_EQ(ParseCidr("not.an.ip/24", &addr, &prefix),
            -1);
}

TEST(TunTest, ParseCidrBadPrefix) {
  uint32_t addr;
  int prefix;
  EXPECT_EQ(ParseCidr("10.0.0.1/33", &addr, &prefix), -1);
  EXPECT_EQ(ParseCidr("10.0.0.1/-1", &addr, &prefix), -1);
  EXPECT_EQ(ParseCidr("10.0.0.1/abc", &addr, &prefix),
            -1);
}

TEST(TunTest, OpenRequiresPrivilege) {
  // Skip if running as root (test is for unprivileged).
  if (geteuid() == 0) {
    GTEST_SKIP() << "running as root, skipping "
                    "privilege test";
  }

  TunDevice tun;
  // Should fail without CAP_NET_ADMIN.
  int rc = TunOpen(&tun, "test_derp%d");
  if (rc == 0) {
    // Unexpected success; clean up.
    TunClose(&tun);
    GTEST_SKIP() << "TUN open succeeded without root "
                    "(maybe CAP_NET_ADMIN set)";
  }
  EXPECT_EQ(rc, -1);
}

}  // namespace hyper_derp
