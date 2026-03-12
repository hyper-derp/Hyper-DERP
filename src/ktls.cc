/// @file ktls.cc
/// @brief Kernel TLS offload: OpenSSL handshake with
///   automatic kTLS installation via SSL_accept.

#include "hyper_derp/ktls.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace hyper_derp {

// TCP_ULP option number for kTLS probe.
static constexpr int kTcpUlp = 31;

static std::string SslError() {
  char buf[256];
  ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
  return buf;
}

auto ProbeKtls()
    -> std::expected<void, Error<KtlsError>> {
  int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (lfd < 0) {
    return MakeError(KtlsError::ModuleNotAvailable,
                     "socket() failed");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(lfd, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr)) < 0 ||
      listen(lfd, 1) < 0) {
    close(lfd);
    return MakeError(KtlsError::ModuleNotAvailable,
                     "bind/listen failed");
  }

  socklen_t alen = sizeof(addr);
  getsockname(lfd, reinterpret_cast<sockaddr*>(&addr),
              &alen);

  int cfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (cfd < 0) {
    close(lfd);
    return MakeError(KtlsError::ModuleNotAvailable,
                     "client socket() failed");
  }

  if (connect(cfd, reinterpret_cast<sockaddr*>(&addr),
              sizeof(addr)) < 0) {
    close(cfd);
    close(lfd);
    return MakeError(KtlsError::ModuleNotAvailable,
                     "connect() failed");
  }

  int afd = accept(lfd, nullptr, nullptr);
  close(lfd);
  if (afd < 0) {
    close(cfd);
    return MakeError(KtlsError::ModuleNotAvailable,
                     "accept() failed");
  }

  // Try to install TLS ULP on the connected socket.
  int rc = setsockopt(afd, SOL_TCP, kTcpUlp, "tls", 4);
  int err = errno;
  close(afd);
  close(cfd);

  if (rc < 0) {
    return MakeError(
        KtlsError::ModuleNotAvailable,
        std::string("TCP_ULP setsockopt: ") +
            strerror(err));
  }
  return {};
}

auto KtlsCtxInit(KtlsCtx* ctx,
                  const char* cert_path,
                  const char* key_path)
    -> std::expected<void, Error<KtlsError>> {
  SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!ssl_ctx) {
    return MakeError(KtlsError::SslCtxFailed,
                     "SSL_CTX_new: " + SslError());
  }

  // TLS 1.3 only.
  SSL_CTX_set_min_proto_version(ssl_ctx,
                                TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx,
                                TLS1_3_VERSION);

  // AES-GCM ciphers only (kTLS-compatible).
  SSL_CTX_set_ciphersuites(
      ssl_ctx,
      "TLS_AES_128_GCM_SHA256:"
      "TLS_AES_256_GCM_SHA384");

  // Enable kTLS explicitly. OpenSSL 3.x may enable this
  // by default, but being explicit ensures it.
  SSL_CTX_set_options(ssl_ctx, SSL_OP_ENABLE_KTLS);

  // Disable session tickets — prevents NewSessionTicket
  // records that would advance the TX record sequence
  // counter before kTLS takes over.
  SSL_CTX_set_num_tickets(ssl_ctx, 0);

  if (SSL_CTX_use_certificate_chain_file(
          ssl_ctx, cert_path) != 1) {
    SSL_CTX_free(ssl_ctx);
    return MakeError(
        KtlsError::SslCtxFailed,
        std::string("load cert: ") + SslError());
  }

  if (SSL_CTX_use_PrivateKey_file(
          ssl_ctx, key_path, SSL_FILETYPE_PEM) != 1) {
    SSL_CTX_free(ssl_ctx);
    return MakeError(
        KtlsError::SslCtxFailed,
        std::string("load key: ") + SslError());
  }

  if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
    SSL_CTX_free(ssl_ctx);
    return MakeError(KtlsError::SslCtxFailed,
                     "cert/key mismatch");
  }

  ctx->ssl_ctx = ssl_ctx;
  return {};
}

void KtlsCtxDestroy(KtlsCtx* ctx) {
  if (ctx->ssl_ctx) {
    SSL_CTX_free(ctx->ssl_ctx);
    ctx->ssl_ctx = nullptr;
  }
}

auto KtlsAccept(KtlsCtx* ctx, int fd)
    -> std::expected<void, Error<KtlsError>> {
  SSL* ssl = SSL_new(ctx->ssl_ctx);
  if (!ssl) {
    return MakeError(KtlsError::HandshakeFailed,
                     "SSL_new: " + SslError());
  }

  SSL_set_fd(ssl, fd);

  int ret = SSL_accept(ssl);
  if (ret != 1) {
    int err = SSL_get_error(ssl, ret);
    std::string msg = "SSL_accept error " +
                      std::to_string(err) + ": " +
                      SslError();
    SSL_free(ssl);
    return MakeError(KtlsError::HandshakeFailed, msg);
  }

  // Verify OpenSSL auto-installed kTLS for both
  // directions. BIO_get_ktls_send/recv return 1 if
  // the kernel is handling encryption.
  int ktls_tx = BIO_get_ktls_send(SSL_get_wbio(ssl));
  int ktls_rx = BIO_get_ktls_recv(SSL_get_rbio(ssl));

  if (!ktls_tx || !ktls_rx) {
    const SSL_CIPHER* cipher =
        SSL_get_current_cipher(ssl);
    const char* name = cipher
        ? SSL_CIPHER_get_name(cipher)
        : "unknown";
    spdlog::warn(
        "kTLS not installed by OpenSSL "
        "(tx={}, rx={}, cipher={})",
        ktls_tx, ktls_rx, name);
    SSL_set_quiet_shutdown(ssl, 1);
    SSL_free(ssl);
    return MakeError(
        KtlsError::KtlsNotInstalled,
        std::string("cipher: ") + name);
  }

  spdlog::debug("kTLS active (tx={}, rx={})",
                ktls_tx, ktls_rx);

  // Detach the fd from the SSL BIOs so SSL_free
  // does not close it. We keep the fd for direct
  // use with io_uring.
  BIO* wbio = SSL_get_wbio(ssl);
  BIO* rbio = SSL_get_rbio(ssl);
  if (wbio) {
    BIO_set_close(wbio, BIO_NOCLOSE);
  }
  if (rbio && rbio != wbio) {
    BIO_set_close(rbio, BIO_NOCLOSE);
  }

  SSL_set_quiet_shutdown(ssl, 1);
  SSL_free(ssl);
  return {};
}

}  // namespace hyper_derp
