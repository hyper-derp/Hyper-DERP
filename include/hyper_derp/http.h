/// @file http.h
/// @brief Minimal HTTP request parser and response builder for
///   the DERP upgrade handshake.

#ifndef INCLUDE_HYPER_DERP_HTTP_H_
#define INCLUDE_HYPER_DERP_HTTP_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "hyper_derp/error.h"

namespace hyper_derp {

/// Error codes for ParseHttpRequest.
enum class HttpParseError {
  /// Headers not yet complete (need more data).
  Incomplete,
  /// Request is malformed or exceeds size limit.
  BadRequest,
};

/// Human-readable name for an HttpParseError code.
constexpr auto HttpParseErrorName(HttpParseError e)
    -> std::string_view {
  switch (e) {
    case HttpParseError::Incomplete:
      return "Incomplete";
    case HttpParseError::BadRequest:
      return "BadRequest";
  }
  return "Unknown";
}

/// Maximum HTTP request size we'll accept.
inline constexpr int kMaxHttpRequestSize = 4096;

/// Parsed HTTP request (only the fields we need).
struct HttpRequest {
  std::string method;
  std::string path;
  bool has_upgrade = false;
  std::string upgrade;
  bool fast_start = false;
};

/// @brief Parses an HTTP request from a buffer.
/// @param buf Raw request bytes.
/// @param len Number of bytes available.
/// @param req Output parsed request.
/// @returns Bytes consumed on success, or HttpParseError.
auto ParseHttpRequest(const uint8_t* buf, int len,
                      HttpRequest* req)
    -> std::expected<int, Error<HttpParseError>>;

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

/// @brief Writes an HTTP 101 response for HD protocol
///   upgrade.
/// @param buf Output buffer.
/// @param buf_size Buffer capacity.
/// @returns Bytes written, or -1 if buffer too small.
int WriteHdUpgradeResponse(uint8_t* buf, int buf_size);

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
