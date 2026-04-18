/// @file hd_client.h
/// @brief Client-side HD protocol: connect, enroll,
///   send/recv frames.

#ifndef INCLUDE_HYPER_DERP_HD_CLIENT_H_
#define INCLUDE_HYPER_DERP_HD_CLIENT_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <openssl/ssl.h>

#include "hyper_derp/error.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Error codes for client-side HD operations.
enum class HdClientError {
  /// libsodium initialization failed.
  SodiumInitFailed,
  /// socket() call failed.
  SocketFailed,
  /// Invalid host address.
  BadAddress,
  /// connect() call failed.
  ConnectFailed,
  /// Network I/O failed (read/write/disconnect).
  IoFailed,
  /// HTTP upgrade was not accepted (no 101 response).
  UpgradeFailed,
  /// Received an unexpected frame type.
  UnexpectedFrame,
  /// Frame payload length is out of range.
  BadPayloadLength,
  /// Enrollment was denied by the relay.
  EnrollmentDenied,
  /// Frame payload exceeds buffer capacity.
  BufferOverflow,
};

/// Human-readable name for an HdClientError code.
constexpr auto HdClientErrorName(HdClientError e)
    -> std::string_view {
  switch (e) {
    case HdClientError::SodiumInitFailed:
      return "SodiumInitFailed";
    case HdClientError::SocketFailed:
      return "SocketFailed";
    case HdClientError::BadAddress:
      return "BadAddress";
    case HdClientError::ConnectFailed:
      return "ConnectFailed";
    case HdClientError::IoFailed:
      return "IoFailed";
    case HdClientError::UpgradeFailed:
      return "UpgradeFailed";
    case HdClientError::UnexpectedFrame:
      return "UnexpectedFrame";
    case HdClientError::BadPayloadLength:
      return "BadPayloadLength";
    case HdClientError::EnrollmentDenied:
      return "EnrollmentDenied";
    case HdClientError::BufferOverflow:
      return "BufferOverflow";
  }
  return "Unknown";
}

/// @brief Client-side HD connection state.
struct HdClient {
  int fd = -1;
  Key public_key{};
  Key private_key{};
  Key relay_key{};
  bool connected = false;
  bool approved = false;
  std::string host;
  uint16_t port = 0;
  /// Peer ID assigned by relay during enrollment.
  uint16_t peer_id = 0;
  // Userspace TLS (non-kTLS fallback). When set, all
  // I/O uses SSL_read/SSL_write instead of raw
  // read/write.
  SSL* ssl = nullptr;
  SSL_CTX* ssl_ctx = nullptr;
};

/// @brief Initialize client with random Curve25519 keys.
/// @param c Client state to initialize.
/// @returns void on success, or HdClientError.
auto HdClientInit(HdClient* c)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Initialize client with provided key pair and
///   relay key.
/// @param c Client state to initialize.
/// @param pub 32-byte public key.
/// @param priv 32-byte private key.
/// @param relay_key Relay's shared secret key for HMAC.
void HdClientInitWithKeys(HdClient* c,
                          const uint8_t* pub,
                          const uint8_t* priv,
                          const Key& relay_key);

/// @brief Connect to an HD relay via TCP.
/// @param c Initialized client.
/// @param host Server IP address (dotted quad).
/// @param port Server port.
/// @returns void on success, or HdClientError.
auto HdClientConnect(HdClient* c, const char* host,
                     uint16_t port)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Upgrade connection to TLS 1.3.
/// Must be called after HdClientConnect and before
/// HdClientUpgrade.
/// @param c Connected client.
/// @returns void on success, or HdClientError.
auto HdClientTlsConnect(HdClient* c)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Send HTTP upgrade request for /hd path and read
///   101 response.
/// @param c Connected client.
/// @returns void on success, or HdClientError.
auto HdClientUpgrade(HdClient* c)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Perform HD enrollment handshake.
///
/// Sends Enroll frame (client key + HMAC), waits for
/// Approved or Denied response.
/// @param c Upgraded client.
/// @returns void on success, or HdClientError.
auto HdClientEnroll(HdClient* c)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Send an HD Data frame.
/// @param c Enrolled client.
/// @param data Packet payload.
/// @param len Payload length.
/// @returns void on success, or HdClientError.
auto HdClientSendData(HdClient* c,
                      const uint8_t* data, int len)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Receive the next HD frame from the relay.
/// @param c Enrolled client.
/// @param type Output: received frame type.
/// @param payload Output: payload buffer.
/// @param payload_len Output: actual payload length.
/// @param buf_size Capacity of payload buffer.
/// @returns void on success, or HdClientError.
auto HdClientRecvFrame(HdClient* c,
                       HdFrameType* type,
                       uint8_t* payload,
                       int* payload_len,
                       int buf_size)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Send an HD MeshData frame to a specific peer
///   ID.
/// Used for 1:N selective routing.
/// @param c Enrolled client.
/// @param dst_peer_id Local peer ID of the destination.
/// @param data Packet payload.
/// @param len Payload length.
/// @returns void on success, or HdClientError.
auto HdClientSendMeshData(HdClient* c,
                          uint16_t dst_peer_id,
                          const uint8_t* data, int len)
    -> std::expected<void, Error<HdClientError>>;

/// @brief Close client connection and reset state.
/// @param c Client to close.
void HdClientClose(HdClient* c);

/// @brief Set receive timeout on client socket.
/// @param c Client.
/// @param ms Timeout in milliseconds (0 to disable).
/// @returns void on success, or HdClientError.
auto HdClientSetTimeout(HdClient* c, int ms)
    -> std::expected<void, Error<HdClientError>>;

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_CLIENT_H_
