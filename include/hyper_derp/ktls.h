/// @file ktls.h
/// @brief Kernel TLS (kTLS) offload: TLS 1.3 handshake via
///   OpenSSL with automatic kTLS installation. After
///   SSL_accept, the kernel handles AES-GCM encryption
///   and io_uring recv/send operates on plaintext.

#ifndef INCLUDE_HYPER_DERP_KTLS_H_
#define INCLUDE_HYPER_DERP_KTLS_H_

#include <cstdint>
#include <expected>

#include <openssl/ssl.h>

#include "hyper_derp/error.h"

namespace hyper_derp {

/// kTLS error codes.
enum class KtlsError {
  /// Kernel doesn't support kTLS (CONFIG_TLS not loaded).
  ModuleNotAvailable,
  /// SSL_CTX creation or configuration failed.
  SslCtxFailed,
  /// TLS handshake failed.
  HandshakeFailed,
  /// OpenSSL did not auto-install kTLS on the socket.
  KtlsNotInstalled,
  /// Cipher suite not supported by kTLS.
  UnsupportedCipher,
};

/// Human-readable name for a KtlsError code.
constexpr auto KtlsErrorName(KtlsError e)
    -> std::string_view {
  switch (e) {
    case KtlsError::ModuleNotAvailable:
      return "ModuleNotAvailable";
    case KtlsError::SslCtxFailed:
      return "SslCtxFailed";
    case KtlsError::HandshakeFailed:
      return "HandshakeFailed";
    case KtlsError::KtlsNotInstalled:
      return "KtlsNotInstalled";
    case KtlsError::UnsupportedCipher:
      return "UnsupportedCipher";
  }
  return "Unknown";
}

/// Shared SSL_CTX for all connections.
struct KtlsCtx {
  SSL_CTX* ssl_ctx = nullptr;
};

/// @brief Probes whether the kernel supports kTLS.
/// Creates a loopback socket pair and attempts to install
/// the TLS ULP. Call once at startup.
auto ProbeKtls()
    -> std::expected<void, Error<KtlsError>>;

/// @brief Initializes the shared SSL_CTX.
/// @param ctx Output context.
/// @param cert_path Path to PEM certificate file.
/// @param key_path Path to PEM private key file.
auto KtlsCtxInit(KtlsCtx* ctx,
                  const char* cert_path,
                  const char* key_path)
    -> std::expected<void, Error<KtlsError>>;

/// @brief Destroys the SSL_CTX.
void KtlsCtxDestroy(KtlsCtx* ctx);

/// @brief Performs TLS 1.3 handshake and verifies kTLS.
/// OpenSSL 3.x auto-installs kTLS during SSL_accept when
/// the kernel supports it. This function verifies kTLS was
/// installed for both TX and RX, then detaches the fd from
/// the SSL object so it can be used directly with io_uring.
/// @param ctx Initialized kTLS context.
/// @param fd Connected TCP socket.
/// @returns void on success, or KtlsError.
auto KtlsAccept(KtlsCtx* ctx, int fd)
    -> std::expected<void, Error<KtlsError>>;

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_KTLS_H_
