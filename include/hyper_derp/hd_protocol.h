/// @file hd_protocol.h
/// @brief HD wire protocol constants and frame codec.

#ifndef INCLUDE_HYPER_DERP_HD_PROTOCOL_H_
#define INCLUDE_HYPER_DERP_HD_PROTOCOL_H_

#include <cstdint>
#include <cstring>

#include "hyper_derp/protocol.h"

namespace hyper_derp {

// -- Wire constants ----------------------------------------------------------

/// HD frame header: 1 byte type + 3 bytes big-endian length.
inline constexpr int kHdFrameHeaderSize = 4;

/// Maximum payload size for a single HD frame (64 KiB).
inline constexpr int kHdMaxFramePayload = 64 * 1024;

/// Ping/pong data size.
inline constexpr int kHdPingDataSize = 8;

// -- Frame types -------------------------------------------------------------

/// HD frame type identifiers (wire values).
enum class HdFrameType : uint8_t {
  kData = 0x01,
  kPing = 0x02,
  kPong = 0x03,
  kEnroll = 0x10,
  kApproved = 0x11,
  kDenied = 0x12,
  kPeerInfo = 0x20,
  kPeerGone = 0x21,
};

// -- Frame header codec (operates on raw buffers) ----------------------------

/// @brief Reads frame type from an HD header buffer.
/// @param hdr Pointer to at least kHdFrameHeaderSize bytes.
/// @returns The frame type byte.
inline constexpr HdFrameType HdReadFrameType(
    const uint8_t* hdr) {
  return static_cast<HdFrameType>(hdr[0]);
}

/// @brief Reads payload length from an HD header buffer.
/// @param hdr Pointer to at least kHdFrameHeaderSize bytes.
/// @returns Payload length in bytes (3-byte big-endian
///   decoded).
inline constexpr uint32_t HdReadPayloadLen(
    const uint8_t* hdr) {
  return (static_cast<uint32_t>(hdr[1]) << 16) |
         (static_cast<uint32_t>(hdr[2]) << 8) |
         (static_cast<uint32_t>(hdr[3]));
}

/// @brief Writes an HD frame header into a buffer.
/// @param hdr Pointer to at least kHdFrameHeaderSize bytes.
/// @param type Frame type.
/// @param payload_len Payload length (must fit in 24 bits).
inline void HdWriteFrameHeader(uint8_t* hdr,
                               HdFrameType type,
                               uint32_t payload_len) {
  hdr[0] = static_cast<uint8_t>(type);
  hdr[1] = static_cast<uint8_t>(payload_len >> 16);
  hdr[2] = static_cast<uint8_t>(payload_len >> 8);
  hdr[3] = static_cast<uint8_t>(payload_len);
}

/// @brief Validates a payload length against the maximum.
/// @param len The payload length to validate.
/// @returns True if the length is within bounds.
inline constexpr bool HdIsValidPayloadLen(uint32_t len) {
  return len <= kHdMaxFramePayload;
}

// -- Frame builders (implemented in hd_protocol.cc) --------------------------

/// @brief Builds an HD Data frame header.
/// @param buf Output buffer (at least kHdFrameHeaderSize
///   bytes).
/// @param payload_len Length of the data payload to follow.
/// @returns Total header size written (always
///   kHdFrameHeaderSize).
int HdBuildDataHeader(uint8_t* buf, int payload_len);

/// @brief Builds an HD Ping frame.
/// @param buf Output buffer (at least kHdFrameHeaderSize +
///   kHdPingDataSize bytes).
/// @param data 8 bytes of ping data.
/// @returns Total frame size written.
int HdBuildPing(uint8_t* buf, const uint8_t* data);

/// @brief Builds an HD Pong frame.
/// @param buf Output buffer (at least kHdFrameHeaderSize +
///   kHdPingDataSize bytes).
/// @param data 8 bytes of ping data to echo back.
/// @returns Total frame size written.
int HdBuildPong(uint8_t* buf, const uint8_t* data);

/// @brief Builds an HD Enroll frame.
/// @param buf Output buffer.
/// @param client_key Client's 32-byte public key.
/// @param hmac HMAC data.
/// @param hmac_len Length of HMAC data.
/// @returns Total frame size written.
int HdBuildEnroll(uint8_t* buf,
                  const Key& client_key,
                  const uint8_t* hmac,
                  int hmac_len);

/// @brief Builds an HD Approved frame.
/// @param buf Output buffer.
/// @param client_key Approved client's 32-byte public key.
/// @returns Total frame size written.
int HdBuildApproved(uint8_t* buf,
                    const Key& client_key);

/// @brief Builds an HD Denied frame.
/// @param buf Output buffer.
/// @param reason Denial reason code.
/// @param msg Human-readable denial message.
/// @param msg_len Length of denial message.
/// @returns Total frame size written.
int HdBuildDenied(uint8_t* buf,
                  uint8_t reason,
                  const char* msg,
                  int msg_len);

/// @brief Builds an HD PeerGone frame.
/// @param buf Output buffer.
/// @param peer_key The departed peer's 32-byte key.
/// @param reason Reason for departure.
/// @returns Total frame size written.
int HdBuildPeerGone(uint8_t* buf,
                    const Key& peer_key,
                    uint8_t reason);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_PROTOCOL_H_
