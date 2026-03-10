/// @file http.h
/// @brief Minimal HTTP request parser and response builder for
///   the DERP upgrade handshake.

#ifndef INCLUDE_HYPER_DERP_HTTP_H_
#define INCLUDE_HYPER_DERP_HTTP_H_

#include <cstdint>

namespace hyper_derp {

/// Maximum HTTP request size we'll accept.
inline constexpr int kMaxHttpRequestSize = 4096;

/// Parsed HTTP request (only the fields we need).
struct HttpRequest {
  char method[8];
  char path[256];
  bool has_upgrade;
  char upgrade[32];
  bool fast_start;
};

/// @brief Parses an HTTP request from a buffer.
/// @param buf Raw request bytes.
/// @param len Number of bytes available.
/// @param req Output parsed request.
/// @returns Bytes consumed on success, -1 if incomplete
///   (need more data), -2 on parse error.
int ParseHttpRequest(const uint8_t* buf, int len,
                     HttpRequest* req);

/// @brief Writes an HTTP 101 Switching Protocols response.
/// @param buf Output buffer.
/// @param buf_size Buffer capacity.
/// @param server_pub_key_hex Hex-encoded server public key
///   (64 chars).
/// @returns Bytes written, or -1 if buffer too small.
int WriteUpgradeResponse(uint8_t* buf, int buf_size,
                         const char* server_pub_key_hex);

/// @brief Writes an HTTP 200 response for /derp/probe.
/// @param buf Output buffer.
/// @param buf_size Buffer capacity.
/// @returns Bytes written, or -1 if buffer too small.
int WriteProbeResponse(uint8_t* buf, int buf_size);

/// @brief Writes an HTTP 204 No Content response.
/// @param buf Output buffer.
/// @param buf_size Buffer capacity.
/// @param challenge X-Tailscale-Challenge header value
///   (may be null).
/// @returns Bytes written, or -1 if buffer too small.
int WriteNoContentResponse(uint8_t* buf, int buf_size,
                           const char* challenge);

/// @brief Writes an HTTP error response.
/// @param buf Output buffer.
/// @param buf_size Buffer capacity.
/// @param status_code HTTP status code.
/// @param message Error message body.
/// @returns Bytes written, or -1 if buffer too small.
int WriteErrorResponse(uint8_t* buf, int buf_size,
                       int status_code,
                       const char* message);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HTTP_H_
