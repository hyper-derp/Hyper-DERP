/// @file hd_handshake.h
/// @brief HD enrollment handshake: HMAC-based peer
///   enrollment with auto-approve or manual approval.

#ifndef INCLUDE_HYPER_DERP_HD_HANDSHAKE_H_
#define INCLUDE_HYPER_DERP_HD_HANDSHAKE_H_

#include <cstdint>
#include <expected>
#include <string_view>

#include "hyper_derp/error.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Error codes for the HD enrollment handshake.
enum class HdHandshakeError {
  /// Network I/O failed (read/write/disconnect).
  IoFailed,
  /// Received an unexpected frame type.
  UnexpectedFrame,
  /// Frame payload length is out of range.
  BadPayloadLength,
  /// Enrollment HMAC verification failed.
  InvalidHmac,
  /// Enrollment was denied by the relay.
  EnrollmentDenied,
};

/// Human-readable name for an HdHandshakeError code.
constexpr auto HdHandshakeErrorName(HdHandshakeError e)
    -> std::string_view {
  switch (e) {
    case HdHandshakeError::IoFailed:
      return "IoFailed";
    case HdHandshakeError::UnexpectedFrame:
      return "UnexpectedFrame";
    case HdHandshakeError::BadPayloadLength:
      return "BadPayloadLength";
    case HdHandshakeError::InvalidHmac:
      return "InvalidHmac";
    case HdHandshakeError::EnrollmentDenied:
      return "EnrollmentDenied";
  }
  return "Unknown";
}

/// Result of a successful HD enrollment handshake.
struct HdEnrollResult {
  Key client_key{};
  bool auto_approved = false;
};

/// @brief Server-side HD enrollment handshake.
///
/// Reads an Enroll frame, validates the HMAC, and inserts
/// the peer into the registry. If auto-approve mode is
/// active, sends an Approved frame and sets
/// result->auto_approved = true. If manual mode, the peer
/// stays pending and no response is sent.
/// @param fd Socket file descriptor.
/// @param reg HD peer registry.
/// @param result Output enrollment result.
/// @returns void on success, or HdHandshakeError.
auto HdPerformHandshake(int fd,
                        HdPeerRegistry* reg,
                        HdEnrollResult* result)
    -> std::expected<void, Error<HdHandshakeError>>;

/// @brief Sends an Approved frame to a peer.
///
/// Called by the REST API after admin approval.
/// @param fd Socket file descriptor.
/// @param client_key Approved client's public key.
/// @returns void on success, or HdHandshakeError.
auto HdSendApproved(int fd, const Key& client_key)
    -> std::expected<void, Error<HdHandshakeError>>;

/// @brief Sends a Denied frame to a peer.
/// @param fd Socket file descriptor.
/// @param reason Denial reason code.
/// @param message Human-readable denial message.
/// @returns void on success, or HdHandshakeError.
auto HdSendDenied(int fd, uint8_t reason,
                  const char* message)
    -> std::expected<void, Error<HdHandshakeError>>;

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_HANDSHAKE_H_
