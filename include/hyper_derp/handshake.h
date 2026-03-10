/// @file handshake.h
/// @brief DERP handshake: key exchange and client
///   authentication.

#ifndef INCLUDE_HYPER_DERP_HANDSHAKE_H_
#define INCLUDE_HYPER_DERP_HANDSHAKE_H_

#include <cstdint>

#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// NaCl box overhead (Poly1305 auth tag).
inline constexpr int kBoxOverhead = 16;

/// NaCl nonce size.
inline constexpr int kNonceSize = 24;

/// Magic string sent in the ServerKey frame.
/// "DERP🔑" = 0x44 45 52 50 f0 9f 94 91
inline constexpr int kMagicSize = 8;
inline constexpr uint8_t kMagic[kMagicSize] = {
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
  uint8_t public_key[kKeySize];
  uint8_t private_key[kKeySize];
};

/// Parsed client info from the handshake.
struct ClientInfo {
  uint8_t public_key[kKeySize];
  int version;
  bool can_ack_pings;
  bool is_prober;
};

/// @brief Generates a Curve25519 key pair for the server.
/// @param keys Output key pair.
/// @returns 0 on success, -1 on failure.
int GenerateServerKeys(ServerKeys* keys);

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
/// @returns Total frame size written, or -1 on failure.
int BuildServerInfoFrame(uint8_t* buf, int buf_size,
                         const ServerKeys* server_keys,
                         const uint8_t* client_pub);

/// @brief Parses and decrypts a ClientInfo frame payload.
/// @param payload Frame payload (after the 5-byte header).
/// @param payload_len Payload length.
/// @param server_keys Server key pair (private key for
///   NaCl box).
/// @param info Output parsed client info.
/// @returns 0 on success, -1 on failure.
int ParseClientInfo(const uint8_t* payload,
                    int payload_len,
                    const ServerKeys* server_keys,
                    ClientInfo* info);

/// @brief Performs the full DERP handshake on a connected
///   socket (blocking).
/// @param fd Socket file descriptor.
/// @param server_keys Server key pair.
/// @param info Output parsed client info.
/// @returns 0 on success, -1 on failure.
int PerformHandshake(int fd,
                     const ServerKeys* server_keys,
                     ClientInfo* info);

/// @brief Converts a 32-byte key to a 64-char hex string.
/// @param key 32-byte key.
/// @param hex Output buffer (must hold 65 bytes for
///   null terminator).
void KeyToHex(const uint8_t* key, char* hex);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HANDSHAKE_H_
