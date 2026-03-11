/// @file handshake.h
/// @brief DERP handshake: key exchange and client
///   authentication.

#ifndef INCLUDE_HYPER_DERP_HANDSHAKE_H_
#define INCLUDE_HYPER_DERP_HANDSHAKE_H_

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

#include "hyper_derp/error.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Error codes for the DERP handshake path.
enum class HandshakeError {
  /// Network I/O failed (read/write/disconnect).
  IoFailed,
  /// Received an unexpected frame type.
  UnexpectedFrame,
  /// Frame payload length is out of range.
  BadPayloadLength,
  /// NaCl box encryption failed.
  EncryptionFailed,
  /// NaCl box decryption failed.
  DecryptionFailed,
  /// Memory allocation failed.
  AllocFailed,
  /// libsodium initialization failed.
  SodiumInitFailed,
  /// Client info payload is malformed.
  BadClientInfo,
};

/// Human-readable name for a HandshakeError code.
constexpr auto HandshakeErrorName(HandshakeError e)
    -> std::string_view {
  switch (e) {
    case HandshakeError::IoFailed:
      return "IoFailed";
    case HandshakeError::UnexpectedFrame:
      return "UnexpectedFrame";
    case HandshakeError::BadPayloadLength:
      return "BadPayloadLength";
    case HandshakeError::EncryptionFailed:
      return "EncryptionFailed";
    case HandshakeError::DecryptionFailed:
      return "DecryptionFailed";
    case HandshakeError::AllocFailed:
      return "AllocFailed";
    case HandshakeError::SodiumInitFailed:
      return "SodiumInitFailed";
    case HandshakeError::BadClientInfo:
      return "BadClientInfo";
  }
  return "Unknown";
}

/// NaCl box overhead (Poly1305 auth tag).
inline constexpr int kBoxOverhead = 16;

/// NaCl nonce size.
inline constexpr int kNonceSize = 24;

/// Magic string sent in the ServerKey frame.
/// "DERP🔑" = 0x44 45 52 50 f0 9f 94 91
inline constexpr int kMagicSize = 8;
inline constexpr std::array<uint8_t, kMagicSize> kMagic = {
    0x44, 0x45, 0x52, 0x50, 0xf0, 0x9f, 0x94, 0x91};

/// ServerKey frame payload size: magic + key.
inline constexpr int kServerKeyPayloadSize =
    kMagicSize + kKeySize;

/// Minimum ClientInfo frame payload size: key + nonce.
inline constexpr int kMinClientInfoPayload =
    kKeySize + kNonceSize;

/// Maximum ClientInfo frame payload size (256 KiB).
inline constexpr int kMaxClientInfoPayload = 256 * 1024;

/// Server key pair (Curve25519).
struct ServerKeys {
  Key public_key{};
  Key private_key{};
};

/// Parsed client info from the handshake.
struct ClientInfo {
  Key public_key{};
  int version = kProtocolVersion;
  bool can_ack_pings = false;
  bool is_prober = false;
};

/// @brief Generates a Curve25519 key pair for the server.
/// @param keys Output key pair.
/// @returns void on success, or HandshakeError.
auto GenerateServerKeys(ServerKeys* keys)
    -> std::expected<void, Error<HandshakeError>>;

/// @brief Builds a ServerKey frame into a buffer.
/// @param buf Output buffer (must hold kFrameHeaderSize +
///   kServerKeyPayloadSize bytes).
/// @param server_keys Server key pair.
/// @returns Total frame size written.
int BuildServerKeyFrame(uint8_t* buf,
                        const ServerKeys* server_keys);

/// @brief Builds a ServerInfo frame into a buffer.
/// @param buf Output buffer.
/// @param buf_size Buffer capacity.
/// @param server_keys Server key pair (private key for
///   NaCl box).
/// @param client_pub Client's public key (for NaCl box).
/// @returns Total frame size written, or HandshakeError.
auto BuildServerInfoFrame(uint8_t* buf, int buf_size,
                          const ServerKeys* server_keys,
                          const Key& client_pub)
    -> std::expected<int, Error<HandshakeError>>;

/// @brief Parses and decrypts a ClientInfo frame payload.
/// @param payload Frame payload (after the 5-byte header).
/// @param payload_len Payload length.
/// @param server_keys Server key pair (private key for
///   NaCl box).
/// @param info Output parsed client info.
/// @returns void on success, or HandshakeError.
auto ParseClientInfo(const uint8_t* payload,
                     int payload_len,
                     const ServerKeys* server_keys,
                     ClientInfo* info)
    -> std::expected<void, Error<HandshakeError>>;

/// @brief Performs the full DERP handshake on a connected
///   socket (blocking).
/// @param fd Socket file descriptor.
/// @param server_keys Server key pair.
/// @param info Output parsed client info.
/// @returns void on success, or HandshakeError.
auto PerformHandshake(int fd,
                      const ServerKeys* server_keys,
                      ClientInfo* info)
    -> std::expected<void, Error<HandshakeError>>;

/// @brief Converts a 32-byte key to a 64-char hex string.
/// @param key 32-byte key.
/// @param hex Output buffer (must hold 65 bytes for
///   null terminator).
void KeyToHex(const Key& key, char* hex);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HANDSHAKE_H_
