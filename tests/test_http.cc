/// @file test_http.cc
/// @brief Unit tests for HTTP request parser and response
///   builders.

#include "hyper_derp/http.h"

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

TEST(HttpTest, ParseDerpUpgradeRequest) {
  const char* raw =
      "GET /derp HTTP/1.1\r\n"
      "Host: relay.example.com\r\n"
      "Upgrade: DERP\r\n"
      "Connection: Upgrade\r\n"
      "\r\n";
  int len = strlen(raw);
  HttpRequest req;
  auto result = ParseHttpRequest(
      reinterpret_cast<const uint8_t*>(raw), len, &req);
  ASSERT_TRUE(result.has_value());
  ASSERT_GT(*result, 0);
  EXPECT_EQ(req.method, "GET");
  EXPECT_EQ(req.path, "/derp");
  EXPECT_TRUE(req.has_upgrade);
  EXPECT_EQ(req.upgrade, "DERP");
  EXPECT_FALSE(req.fast_start);
}

TEST(HttpTest, ParseProbeRequest) {
  const char* raw =
      "GET /derp/probe HTTP/1.1\r\n"
      "Host: relay.example.com\r\n"
      "\r\n";
  int len = strlen(raw);
  HttpRequest req;
  auto result = ParseHttpRequest(
      reinterpret_cast<const uint8_t*>(raw), len, &req);
  ASSERT_TRUE(result.has_value());
  ASSERT_GT(*result, 0);
  EXPECT_EQ(req.path, "/derp/probe");
  EXPECT_FALSE(req.has_upgrade);
}

TEST(HttpTest, ParseGenerate204Request) {
  const char* raw =
      "GET /generate_204 HTTP/1.1\r\n"
      "Host: relay.example.com\r\n"
      "\r\n";
  int len = strlen(raw);
  HttpRequest req;
  auto result = ParseHttpRequest(
      reinterpret_cast<const uint8_t*>(raw), len, &req);
  ASSERT_TRUE(result.has_value());
  ASSERT_GT(*result, 0);
  EXPECT_EQ(req.path, "/generate_204");
}

TEST(HttpTest, ParseFastStart) {
  const char* raw =
      "GET /derp HTTP/1.1\r\n"
      "Host: relay.example.com\r\n"
      "Upgrade: DERP\r\n"
      "Connection: Upgrade\r\n"
      "Derp-Fast-Start: 1\r\n"
      "\r\n";
  int len = strlen(raw);
  HttpRequest req;
  auto result = ParseHttpRequest(
      reinterpret_cast<const uint8_t*>(raw), len, &req);
  ASSERT_TRUE(result.has_value());
  ASSERT_GT(*result, 0);
  EXPECT_TRUE(req.fast_start);
}

TEST(HttpTest, ParseWebSocketUpgrade) {
  const char* raw =
      "GET /derp HTTP/1.1\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "\r\n";
  int len = strlen(raw);
  HttpRequest req;
  auto result = ParseHttpRequest(
      reinterpret_cast<const uint8_t*>(raw), len, &req);
  ASSERT_TRUE(result.has_value());
  ASSERT_GT(*result, 0);
  EXPECT_EQ(req.upgrade, "websocket");
  EXPECT_TRUE(req.has_upgrade);
}

TEST(HttpTest, IncompleteRequest) {
  const char* raw = "GET /derp HTTP/1.1\r\n";
  int len = strlen(raw);
  HttpRequest req;
  auto result = ParseHttpRequest(
      reinterpret_cast<const uint8_t*>(raw), len, &req);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code,
            HttpParseError::Incomplete);
}

TEST(HttpTest, UpgradeResponse) {
  uint8_t buf[512];
  const char* hex =
      "abcdef0123456789abcdef0123456789"
      "abcdef0123456789abcdef0123456789";
  int n = WriteUpgradeResponse(buf, sizeof(buf), hex);
  ASSERT_GT(n, 0);
  const char* resp = reinterpret_cast<const char*>(buf);
  EXPECT_TRUE(strstr(resp, "101 Switching Protocols"));
  EXPECT_TRUE(strstr(resp, "Upgrade: DERP"));
  EXPECT_TRUE(strstr(resp, "Derp-Version:"));
  EXPECT_TRUE(strstr(resp, hex));
}

TEST(HttpTest, ProbeResponse) {
  uint8_t buf[256];
  int n = WriteProbeResponse(buf, sizeof(buf));
  ASSERT_GT(n, 0);
  EXPECT_TRUE(strstr(reinterpret_cast<char*>(buf),
                     "200 OK"));
  EXPECT_TRUE(strstr(reinterpret_cast<char*>(buf),
                     "Access-Control-Allow-Origin: *"));
}

TEST(HttpTest, NoContentResponse) {
  uint8_t buf[512];
  int n = WriteNoContentResponse(buf, sizeof(buf),
                                 nullptr);
  ASSERT_GT(n, 0);
  EXPECT_TRUE(strstr(reinterpret_cast<char*>(buf),
                     "204 No Content"));
}

TEST(HttpTest, NoContentResponseWithChallenge) {
  uint8_t buf[512];
  int n = WriteNoContentResponse(buf, sizeof(buf),
                                 "test-123");
  ASSERT_GT(n, 0);
  EXPECT_TRUE(strstr(reinterpret_cast<char*>(buf),
                     "X-Tailscale-Response: "
                     "response test-123"));
}

TEST(HttpTest, NoContentResponseBadChallenge) {
  uint8_t buf[512];
  // Angle brackets are not valid challenge chars.
  int n = WriteNoContentResponse(buf, sizeof(buf),
                                 "bad<chars>");
  ASSERT_GT(n, 0);
  EXPECT_FALSE(strstr(reinterpret_cast<char*>(buf),
                      "X-Tailscale-Response"));
}

TEST(HttpTest, ErrorResponse) {
  uint8_t buf[256];
  int n = WriteErrorResponse(buf, sizeof(buf), 426,
                             "upgrade required");
  ASSERT_GT(n, 0);
  EXPECT_TRUE(strstr(reinterpret_cast<char*>(buf),
                     "426 Upgrade Required"));
  EXPECT_TRUE(strstr(reinterpret_cast<char*>(buf),
                     "upgrade required"));
}

TEST(HttpTest, CaseInsensitiveHeaders) {
  const char* raw =
      "GET /derp HTTP/1.1\r\n"
      "HOST: relay.example.com\r\n"
      "UPGRADE: derp\r\n"
      "CONNECTION: upgrade\r\n"
      "\r\n";
  int len = strlen(raw);
  HttpRequest req;
  auto result = ParseHttpRequest(
      reinterpret_cast<const uint8_t*>(raw), len, &req);
  ASSERT_TRUE(result.has_value());
  ASSERT_GT(*result, 0);
  EXPECT_TRUE(req.has_upgrade);
}

}  // namespace hyper_derp
