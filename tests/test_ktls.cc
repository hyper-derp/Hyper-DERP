/// @file test_ktls.cc
/// @brief kTLS unit tests: probe, context init, and
///   full TLS 1.3 handshake with auto kTLS verification.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "hyper_derp/ktls.h"

namespace hyper_derp {
namespace {

class KtlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/ktls_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    ASSERT_NE(dir, nullptr);
    tmpdir_ = dir;

    cert_path_ = tmpdir_ + "/cert.pem";
    key_path_ = tmpdir_ + "/key.pem";

    std::string cmd =
        "openssl req -x509 -newkey ec "
        "-pkeyopt ec_paramgen_curve:prime256v1 "
        "-keyout " + key_path_ +
        " -out " + cert_path_ +
        " -days 1 -nodes -subj '/CN=localhost' "
        "2>/dev/null";
    ASSERT_EQ(system(cmd.c_str()), 0)
        << "Failed to generate test certs";
  }

  void TearDown() override {
    if (!tmpdir_.empty()) {
      std::filesystem::remove_all(tmpdir_);
    }
  }

  std::string tmpdir_;
  std::string cert_path_;
  std::string key_path_;
};

TEST_F(KtlsTest, Probe) {
  auto result = ProbeKtls();
  if (!result) {
    GTEST_SKIP() << "kTLS not available: "
                 << result.error().message;
  }
}

TEST_F(KtlsTest, CtxInit) {
  KtlsCtx ctx;
  auto result = KtlsCtxInit(
      &ctx, cert_path_.c_str(), key_path_.c_str());
  ASSERT_TRUE(result.has_value())
      << result.error().message;
  EXPECT_NE(ctx.ssl_ctx, nullptr);
  KtlsCtxDestroy(&ctx);
}

TEST_F(KtlsTest, CtxInitBadCert) {
  KtlsCtx ctx;
  auto result = KtlsCtxInit(
      &ctx, "/nonexistent/cert.pem",
      "/nonexistent/key.pem");
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, KtlsError::SslCtxFailed);
}

TEST_F(KtlsTest, Accept) {
  auto probe = ProbeKtls();
  if (!probe) {
    GTEST_SKIP() << "kTLS not available: "
                 << probe.error().message;
  }

  KtlsCtx ctx;
  auto init = KtlsCtxInit(
      &ctx, cert_path_.c_str(), key_path_.c_str());
  ASSERT_TRUE(init.has_value()) << init.error().message;

  // Listening socket.
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(lfd, 0);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(bind(lfd, reinterpret_cast<sockaddr*>(&addr),
                 sizeof(addr)), 0);
  ASSERT_EQ(listen(lfd, 1), 0);

  socklen_t alen = sizeof(addr);
  getsockname(lfd, reinterpret_cast<sockaddr*>(&addr),
              &alen);
  uint16_t port = ntohs(addr.sin_port);

  // Server thread.
  bool server_ok = false;
  int server_fd = -1;
  std::thread server_thread([&]() {
    int afd = ::accept(lfd, nullptr, nullptr);
    if (afd < 0) return;

    timeval tv{.tv_sec = 5, .tv_usec = 0};
    setsockopt(afd, SOL_SOCKET, SO_RCVTIMEO,
               &tv, sizeof(tv));
    setsockopt(afd, SOL_SOCKET, SO_SNDTIMEO,
               &tv, sizeof(tv));

    auto result = KtlsAccept(&ctx, afd);
    if (result.has_value()) {
      server_ok = true;
      server_fd = afd;
    } else {
      close(afd);
    }
  });

  // Client: TLS 1.3 connect.
  int cfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(cfd, 0);
  addr.sin_port = htons(port);
  ASSERT_EQ(connect(cfd,
                    reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)), 0);

  SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
  ASSERT_NE(client_ctx, nullptr);
  SSL_CTX_set_min_proto_version(client_ctx,
                                TLS1_3_VERSION);
  SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE,
                     nullptr);
  SSL_CTX_set_options(client_ctx, SSL_OP_ENABLE_KTLS);
  SSL_CTX_set_num_tickets(client_ctx, 0);

  SSL* client_ssl = SSL_new(client_ctx);
  ASSERT_NE(client_ssl, nullptr);
  SSL_set_fd(client_ssl, cfd);

  int ret = SSL_connect(client_ssl);
  ASSERT_EQ(ret, 1) << "SSL_connect failed";

  // Check client kTLS state.
  int client_ktls_tx =
      BIO_get_ktls_send(SSL_get_wbio(client_ssl));
  int client_ktls_rx =
      BIO_get_ktls_recv(SSL_get_rbio(client_ssl));

  // Detach client fd from SSL.
  BIO* wbio = SSL_get_wbio(client_ssl);
  BIO* rbio = SSL_get_rbio(client_ssl);
  if (wbio) BIO_set_close(wbio, BIO_NOCLOSE);
  if (rbio && rbio != wbio) {
    BIO_set_close(rbio, BIO_NOCLOSE);
  }
  SSL_set_quiet_shutdown(client_ssl, 1);
  SSL_free(client_ssl);
  SSL_CTX_free(client_ctx);

  server_thread.join();
  close(lfd);

  ASSERT_TRUE(server_ok)
      << "KtlsAccept failed on server side";

  // Test plaintext data exchange over raw fds.
  if (client_ktls_tx && client_ktls_rx) {
    const char* msg = "hello-through-ktls";
    int msg_len = static_cast<int>(strlen(msg));

    ASSERT_EQ(write(cfd, msg, msg_len), msg_len);

    char buf[64] = {};
    int n = static_cast<int>(
        read(server_fd, buf, sizeof(buf)));
    ASSERT_EQ(n, msg_len);
    EXPECT_EQ(memcmp(buf, msg, msg_len), 0);

    const char* reply = "reply-through-ktls";
    int reply_len = static_cast<int>(strlen(reply));
    ASSERT_EQ(write(server_fd, reply, reply_len),
              reply_len);

    memset(buf, 0, sizeof(buf));
    n = static_cast<int>(read(cfd, buf, sizeof(buf)));
    ASSERT_EQ(n, reply_len);
    EXPECT_EQ(memcmp(buf, reply, reply_len), 0);
  } else {
    GTEST_LOG_(INFO)
        << "Client kTLS not installed (tx="
        << client_ktls_tx << " rx=" << client_ktls_rx
        << ") — raw fd exchange skipped";
  }

  close(cfd);
  if (server_fd >= 0) close(server_fd);
  KtlsCtxDestroy(&ctx);
}

}  // namespace
}  // namespace hyper_derp
