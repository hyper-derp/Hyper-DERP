/// @file client.h
/// @brief Client-side DERP protocol: connect, handshake,
///   send/recv frames.

#ifndef INCLUDE_HYPER_DERP_CLIENT_H_
#define INCLUDE_HYPER_DERP_CLIENT_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "hyper_derp/error.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Error codes for client-side DERP operations.
enum class ClientError {
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
  /// ServerKey frame has bad magic bytes.
  BadMagic,
  /// NaCl box encryption failed.
  EncryptionFailed,
  /// Client info frame buffer overflow.
  BufferOverflow,
};

/// Human-readable name for a ClientError code.
constexpr auto ClientErrorName(ClientError e)
    -> std::string_view {
  switch (e) {
    case ClientError::SodiumInitFailed:
      return "SodiumInitFailed";
    case ClientError::SocketFailed:
      return "SocketFailed";
    case ClientError::BadAddress:
      return "BadAddress";
    case ClientError::ConnectFailed:
      return "ConnectFailed";
    case ClientError::IoFailed:
      return "IoFailed";
    case ClientError::UpgradeFailed:
      return "UpgradeFailed";
    case ClientError::UnexpectedFrame:
      return "UnexpectedFrame";
    case ClientError::BadPayloadLength:
      return "BadPayloadLength";
    case ClientError::BadMagic:
      return "BadMagic";
    case ClientError::EncryptionFailed:
      return "EncryptionFailed";
    case ClientError::BufferOverflow:
      return "BufferOverflow";
  }
  return "Unknown";
}

/// @brief Client-side DERP connection state.
struct DerpClient {
  int fd = -1;
  Key public_key{};
  Key private_key{};
  Key server_key{};
  bool connected = false;
  std::string host;
  uint16_t port = 0;
};

/// @brief Initialize client with random Curve25519 keys.
/// @param c Client state to initialize.
/// @returns void on success, or ClientError.
auto ClientInit(DerpClient* c)
    -> std::expected<void, Error<ClientError>>;

/// @brief Initialize client with provided key pair.
/// @param c Client state to initialize.
/// @param pub 32-byte public key.
/// @param priv 32-byte private key.
void ClientInitWithKeys(DerpClient* c,
                        const uint8_t* pub,
                        const uint8_t* priv);

/// @brief Connect to a DERP relay via TCP.
/// @param c Initialized client.
/// @param host Server IP address (dotted quad).
/// @param port Server port.
/// @returns void on success, or ClientError.
auto ClientConnect(DerpClient* c, const char* host,
                   uint16_t port)
    -> std::expected<void, Error<ClientError>>;

/// @brief Send HTTP upgrade request and read 101 response.
/// @param c Connected client.
/// @returns void on success, or ClientError.
auto ClientUpgrade(DerpClient* c)
    -> std::expected<void, Error<ClientError>>;

/// @brief Perform DERP handshake (client side).
///
/// Receives ServerKey, sends ClientInfo, receives
/// ServerInfo.
/// @param c Upgraded client.
/// @returns void on success, or ClientError.
auto ClientHandshake(DerpClient* c)
    -> std::expected<void, Error<ClientError>>;

/// @brief Send a packet to a peer through the relay.
/// @param c Handshaked client.
/// @param dst_key Destination peer's 32-byte public key.
/// @param data Packet payload.
/// @param data_len Payload length.
/// @returns void on success, or ClientError.
auto ClientSendPacket(DerpClient* c,
                      const Key& dst_key,
                      const uint8_t* data, int data_len)
    -> std::expected<void, Error<ClientError>>;

/// @brief Receive the next DERP frame from the relay.
/// @param c Handshaked client.
/// @param type Output: received frame type.
/// @param payload Output: payload buffer.
/// @param payload_len Output: actual payload length.
/// @param buf_size Capacity of payload buffer.
/// @returns void on success, or ClientError.
auto ClientRecvFrame(DerpClient* c, FrameType* type,
                     uint8_t* payload, int* payload_len,
                     int buf_size)
    -> std::expected<void, Error<ClientError>>;

/// @brief Close client connection and reset state.
/// @param c Client to close.
void ClientClose(DerpClient* c);

/// @brief Set receive timeout on client socket.
/// @param c Client.
/// @param ms Timeout in milliseconds (0 to disable).
/// @returns void on success, or ClientError.
auto ClientSetTimeout(DerpClient* c, int ms)
    -> std::expected<void, Error<ClientError>>;

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_CLIENT_H_
