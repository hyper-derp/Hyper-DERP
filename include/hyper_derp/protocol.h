/// @file protocol.h
/// @brief DERP wire protocol constants and frame codec.

#ifndef INCLUDE_HYPER_DERP_PROTOCOL_H_
#define INCLUDE_HYPER_DERP_PROTOCOL_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace hyper_derp {

// -- Wire constants ----------------------------------------------------------

/// Curve25519 public key size in bytes.
inline constexpr int kKeySize = 32;

/// 32-byte Curve25519 key as a value type.
using Key = std::array<uint8_t, kKeySize>;

/// Create a Key from a raw byte pointer.
inline Key ToKey(const uint8_t* data) {
  Key k;
  std::memcpy(k.data(), data, kKeySize);
  return k;
}

/// Frame header: 1 byte type + 4 bytes big-endian length.
inline constexpr int kFrameHeaderSize = 5;

/// Maximum payload size for a single DERP frame (64 KiB).
inline constexpr int kMaxFramePayload = 64 * 1024;

/// Maximum total frame size including header.
inline constexpr int kMaxFrameSize =
    kFrameHeaderSize + kMaxFramePayload;

/// Ping/pong data size.
inline constexpr int kPingDataSize = 8;

/// DERP magic bytes for the HTTP upgrade path.
inline constexpr const char* kUpgradePath = "/derp";

/// HD protocol HTTP upgrade path.
inline constexpr const char* kHdUpgradePath = "/hd";

/// Protocol version sent during HTTP upgrade.
inline constexpr int kProtocolVersion = 2;

// -- Frame types -------------------------------------------------------------

/// DERP frame type identifiers (wire values).
enum class FrameType : uint8_t {
  kServerKey = 0x01,
  kClientInfo = 0x02,
  kSendPacket = 0x04,
  kRecvPacket = 0x05,
  kKeepAlive = 0x06,
  kNotePreferred = 0x07,
  kPeerGone = 0x08,
  kPeerPresent = 0x09,
  kForwardPacket = 0x0a,
  kWatchConns = 0x10,
  kClosePeer = 0x11,
  kPing = 0x12,
  kPong = 0x13,
  kHealth = 0x14,
  kRestarting = 0x15,
  kServerInfo = 0x03,
};

/// Reason codes for PeerGone frames.
enum class PeerGoneReason : uint8_t {
  kDisconnected = 0x00,
  kNotHere = 0x01,
};

// -- Frame header codec (operates on raw buffers) ----------------------------

/// @brief Reads frame type from a header buffer.
/// @param hdr Pointer to at least kFrameHeaderSize bytes.
/// @returns The frame type byte.
inline constexpr FrameType ReadFrameType(
    const uint8_t* hdr) {
  return static_cast<FrameType>(hdr[0]);
}

/// @brief Reads payload length from a header buffer.
/// @param hdr Pointer to at least kFrameHeaderSize bytes.
/// @returns Payload length in bytes (big-endian decoded).
inline constexpr uint32_t ReadPayloadLen(
    const uint8_t* hdr) {
  return (static_cast<uint32_t>(hdr[1]) << 24) |
         (static_cast<uint32_t>(hdr[2]) << 16) |
         (static_cast<uint32_t>(hdr[3]) << 8) |
         (static_cast<uint32_t>(hdr[4]));
}

/// @brief Writes a frame header into a buffer.
/// @param hdr Pointer to at least kFrameHeaderSize bytes.
/// @param type Frame type.
/// @param payload_len Payload length.
inline void WriteFrameHeader(uint8_t* hdr,
                             FrameType type,
                             uint32_t payload_len) {
  hdr[0] = static_cast<uint8_t>(type);
  hdr[1] = static_cast<uint8_t>(payload_len >> 24);
  hdr[2] = static_cast<uint8_t>(payload_len >> 16);
  hdr[3] = static_cast<uint8_t>(payload_len >> 8);
  hdr[4] = static_cast<uint8_t>(payload_len);
}

/// @brief Validates a payload length against the maximum.
/// @param len The payload length to validate.
/// @returns True if the length is within bounds.
inline constexpr bool IsValidPayloadLen(uint32_t len) {
  return len <= kMaxFramePayload;
}

// -- Frame payload accessors (zero-copy into raw buffers) --------------------

/// @brief Extracts the destination key from a SendPacket
///   payload.
/// @param payload Pointer to the SendPacket payload.
/// @returns Pointer to the 32-byte destination key.
inline const uint8_t* SendPacketDstKey(
    const uint8_t* payload) {
  return payload;
}

/// @brief Extracts the packet data from a SendPacket payload.
/// @param payload Pointer to the SendPacket payload.
/// @returns Pointer to the packet data after the dest key.
inline const uint8_t* SendPacketData(
    const uint8_t* payload) {
  return payload + kKeySize;
}

/// @brief Computes the packet data length from a SendPacket.
/// @param payload_len Total payload length.
/// @returns Length of packet data (payload minus key).
inline constexpr int SendPacketDataLen(int payload_len) {
  return payload_len - kKeySize;
}

/// @brief Builds a RecvPacket frame into a buffer.
/// @param buf Output buffer (must have space for header +
///   key + data).
/// @param src_key Source peer's 32-byte public key.
/// @param data Packet data.
/// @param data_len Length of packet data.
/// @returns Total frame size written.
int BuildRecvPacket(uint8_t* buf,
                    const Key& src_key,
                    const uint8_t* data,
                    int data_len);

/// @brief Builds a PeerGone frame into a buffer.
/// @param buf Output buffer.
/// @param peer_key The departed peer's 32-byte key.
/// @param reason Reason for departure.
/// @returns Total frame size written.
int BuildPeerGone(uint8_t* buf,
                  const Key& peer_key,
                  PeerGoneReason reason);

/// @brief Builds a PeerPresent frame into a buffer.
/// @param buf Output buffer.
/// @param peer_key The present peer's 32-byte key.
/// @returns Total frame size written.
int BuildPeerPresent(uint8_t* buf,
                     const Key& peer_key);

/// @brief Builds a KeepAlive frame into a buffer.
/// @param buf Output buffer.
/// @returns Total frame size written (always kFrameHeaderSize).
int BuildKeepAlive(uint8_t* buf);

/// @brief Builds a Pong frame into a buffer.
/// @param buf Output buffer.
/// @param ping_data 8 bytes of ping data to echo back.
/// @returns Total frame size written.
int BuildPong(uint8_t* buf, const uint8_t* ping_data);

/// @brief Builds a Health frame into a buffer.
/// @param buf Output buffer.
/// @param msg Health message string.
/// @param msg_len Length of health message.
/// @returns Total frame size written.
int BuildHealth(uint8_t* buf,
                const uint8_t* msg, int msg_len);

/// @brief Builds a ServerKey frame into a buffer.
/// @param buf Output buffer.
/// @param server_key Server's 32-byte public key.
/// @returns Total frame size written.
int BuildServerKey(uint8_t* buf,
                   const Key& server_key);

/// @brief Builds a Restarting frame into a buffer.
/// @param buf Output buffer.
/// @param reconnect_in_ms Suggested reconnect delay.
/// @param try_for_ms Duration to keep trying.
/// @returns Total frame size written.
int BuildRestarting(uint8_t* buf,
                    uint32_t reconnect_in_ms,
                    uint32_t try_for_ms);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_PROTOCOL_H_
