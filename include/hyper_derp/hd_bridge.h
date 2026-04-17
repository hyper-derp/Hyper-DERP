/// @file hd_bridge.h
/// @brief DERP-to-HD and HD-to-DERP frame format bridge.

#ifndef INCLUDE_HYPER_DERP_HD_BRIDGE_H_
#define INCLUDE_HYPER_DERP_HD_BRIDGE_H_

#include <cstdint>

#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Convert a DERP SendPacket payload to an HD Data frame.
/// Strips the 32-byte destination key (HD doesn't need it;
/// identity comes from the connection). Prepends HD 4-byte
/// header.
///
/// @param send_payload DERP SendPacket payload (32B dst_key
///   + data).
/// @param send_len Payload length (including dst_key).
/// @param out Output buffer for HD frame.
/// @param out_size Output buffer capacity.
/// @returns HD frame length (header + data), or -1 on error.
int BridgeDerpToHd(const uint8_t* send_payload,
                   int send_len,
                   uint8_t* out, int out_size);

/// Convert an HD Data payload to a DERP RecvPacket frame.
/// Prepends 5-byte DERP header + 32-byte source key.
///
/// @param hd_payload HD Data payload (raw data, no header).
/// @param hd_len Payload length.
/// @param src_key Source HD peer's 32-byte key (prepended as
///   DERP source key in the RecvPacket).
/// @param out Output buffer for DERP frame.
/// @param out_size Output buffer capacity.
/// @returns DERP frame length, or -1 on error.
int BridgeHdToDerp(const uint8_t* hd_payload,
                   int hd_len,
                   const Key& src_key,
                   uint8_t* out, int out_size);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_BRIDGE_H_
