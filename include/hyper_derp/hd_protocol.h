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

/// MeshData destination header size.
inline constexpr int kHdMeshDstSize = 2;

/// FleetData destination header size.
inline constexpr int kHdFleetDstSize = 4;

/// RouteAnnounce entry size (2B relay_id + 1B hops).
inline constexpr int kHdRouteEntrySize = 3;

/// Maximum Redirect target_relay_url length.
inline constexpr int kHdRedirectMaxUrl = 256;

// -- Frame types -------------------------------------------------------------

/// HD frame type identifiers (wire values).
enum class HdFrameType : uint8_t {
  kData = 0x01,
  kPing = 0x02,
  kPong = 0x03,
  kMeshData = 0x04,   // Local 1:N with 2B peer ID
  kFleetData = 0x05,  // Cross-relay with 2B relay + 2B peer
  kEnroll = 0x10,
  kApproved = 0x11,
  kDenied = 0x12,
  kPeerInfo = 0x20,
  kPeerGone = 0x21,
  kRedirect = 0x22,
  // Routing policy (Phase 2 of HD_ROUTING_POLICY).
  kOpenConnection = 0x23,
  kOpenConnectionResult = 0x24,
  kIncomingConnection = 0x25,
  kIncomingConnResponse = 0x26,
  kIncomingConnResult = 0x27,
  kRouteAnnounce = 0x30,
};

/// Routing intent for a connection. Wire byte.
enum class HdIntent : uint8_t {
  kPreferDirect = 0,
  kRequireDirect = 1,
  kPreferRelay = 2,
  kRequireRelay = 3,
};

/// Resolved connection mode.
enum class HdConnMode : uint8_t {
  kDenied = 0,
  kDirect = 1,
  kRelayed = 2,
};

/// Deny reason. uint16 wire; reserved ranges:
///   0x0000-0x00FF  resolver-intrinsic
///   0x0100-0x01FF  peer-policy layer
///   0x0200-0x02FF  relay-policy layer
///   0x0300-0x03FF  fleet-policy layer
///   0x0400-0x04FF  federation layer
///   0x0500-0x05FF  capability layer
enum class HdDenyReason : uint16_t {
  kNone = 0x0000,
  kPolicyForbids = 0x0001,
  kPairForbids = 0x0002,
  kPeerUnreachable = 0x0003,
  kTargetUnresponsive = 0x0004,
  kTooManyOpenConns = 0x0005,
  kFleetRoutingNotImplemented = 0x0006,
  kIntentConflict = 0x0101,
  kPeerOverride = 0x0102,
  kDirectCapExceeded = 0x0201,
  kRegionPolicyViolation = 0x0301,
  kFederationDenied = 0x0401,
  kNatIncompatible = 0x0501,
};

/// Flags for OpenConnection / IncomingConnResponse.
inline constexpr uint8_t kHdFlagAllowUpgrade = 0x01;
inline constexpr uint8_t kHdFlagAllowDowngrade = 0x02;
inline constexpr uint8_t kHdFlagAccept = 0x01;

/// Sub-reason byte space; currently unused, reserved 0.
inline constexpr uint8_t kHdSubReasonNone = 0;

/// Redirect reason codes (wire values).
enum class HdRedirectReason : uint8_t {
  /// Relay is shutting down for maintenance.
  kDraining = 0x01,
  /// Relay is shedding load.
  kRebalancing = 0x02,
  /// Peer should use a closer relay.
  kGeoCorrection = 0x03,
  /// Peer policy requires a different relay.
  kPolicyRequired = 0x04,
  /// Relay is at capacity.
  kCapacityFull = 0x05,
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

/// @brief Builds an HD PeerInfo frame.
///
/// Carries ICE candidate information for Level 2 direct
/// path negotiation. Payload format:
///   [32B peer_key][candidate_data...]
/// where candidate_data is IceSerializeCandidates output.
/// @param buf Output buffer (must be large enough).
/// @param peer_key The peer this info is about.
/// @param candidate_data Serialized ICE candidates.
/// @param candidate_len Length of candidate data.
/// @returns Total frame size written.
int HdBuildPeerInfo(uint8_t* buf,
                    const Key& peer_key,
                    const uint8_t* candidate_data,
                    int candidate_len);

/// @brief Builds a MeshData frame header + 2-byte
///   destination.
/// @param buf Output buffer (at least kHdFrameHeaderSize +
///   kHdMeshDstSize bytes).
/// @param dst_peer_id Local peer ID of the destination.
/// @param payload_len Length of the data payload to follow.
/// @returns Total header bytes written (6).
int HdBuildMeshDataHeader(uint8_t* buf,
                          uint16_t dst_peer_id,
                          int payload_len);

/// @brief Reads the 2-byte destination peer ID from a
///   MeshData frame's payload.
/// @param payload Pointer to the MeshData payload start.
/// @returns The destination peer ID.
/// @pre Caller must verify payload_len >= 2.
inline uint16_t HdReadMeshDst(const uint8_t* payload) {
  return static_cast<uint16_t>(
      (payload[0] << 8) | payload[1]);
}

/// @brief Builds a FleetData frame header + 4-byte
///   destination.
/// @param buf Output buffer (at least kHdFrameHeaderSize +
///   kHdFleetDstSize bytes).
/// @param dst_relay_id Destination relay ID.
/// @param dst_peer_id Destination peer ID on that relay.
/// @param payload_len Length of the data payload to follow.
/// @returns Total header bytes written (8).
int HdBuildFleetDataHeader(uint8_t* buf,
                           uint16_t dst_relay_id,
                           uint16_t dst_peer_id,
                           int payload_len);

/// @brief Reads relay ID from FleetData payload.
/// @param payload Pointer to the FleetData payload start.
/// @returns The destination relay ID.
/// @pre Caller must verify payload_len >= 2.
inline uint16_t HdReadFleetRelay(
    const uint8_t* payload) {
  return static_cast<uint16_t>(
      (payload[0] << 8) | payload[1]);
}

/// @brief Reads peer ID from FleetData payload (after
///   relay ID).
/// @param payload Pointer to the FleetData payload start.
/// @returns The destination peer ID.
/// @pre Caller must verify payload_len >= 4.
inline uint16_t HdReadFleetPeer(
    const uint8_t* payload) {
  return static_cast<uint16_t>(
      (payload[2] << 8) | payload[3]);
}

/// @brief Builds a RouteAnnounce frame from relay table
///   data.
/// @param buf Output buffer (must be large enough for
///   header + count * kHdRouteEntrySize bytes).
/// @param buf_size Size of the output buffer.
/// @param relay_ids Array of relay IDs.
/// @param hops Array of hop counts.
/// @param count Number of entries.
/// @returns Total frame size written, or -1 if the buffer
///   is too small.
int HdBuildRouteAnnounce(uint8_t* buf, int buf_size,
                         const uint16_t* relay_ids,
                         const uint8_t* hops,
                         int count);

/// @brief Parses a RouteAnnounce frame payload.
/// @param payload Pointer to the payload start (after
///   frame header).
/// @param payload_len Length of the payload.
/// @param out_ids Output array for relay IDs.
/// @param out_hops Output array for hop counts.
/// @param max_out Maximum entries to write.
/// @returns Number of entries parsed.
int HdParseRouteAnnounce(const uint8_t* payload,
                         int payload_len,
                         uint16_t* out_ids,
                         uint8_t* out_hops,
                         int max_out);

// -- Routing policy: OpenConnection et al. -----------------------------------

/// OpenConnection fixed payload size (no variable fields).
inline constexpr int kHdOpenConnSize = 14;

/// IncomingConnection fixed payload size.
inline constexpr int kHdIncomingConnSize = 44;

/// IncomingConnResponse fixed payload size.
inline constexpr int kHdIncomingRespSize = 11;

/// IncomingConnResult fixed payload size.
inline constexpr int kHdIncomingResultSize = 12;

/// OpenConnectionResult minimum payload size
/// (before relay_path and endpoint_hint).
inline constexpr int kHdOpenResultMinSize = 16;

/// Maximum relay path entries in OpenConnectionResult.
inline constexpr int kHdMaxRelayPath = 32;

/// Maximum endpoint_hint string length.
inline constexpr int kHdMaxEndpointHint = 128;

/// @brief Builds an OpenConnection frame.
///
/// Payload layout:
///   [2B target_peer_id][2B target_relay_id][1B intent]
///   [1B flags][8B correlation_id]
/// @returns Total frame size written.
int HdBuildOpenConnection(uint8_t* buf,
                          uint16_t target_peer_id,
                          uint16_t target_relay_id,
                          HdIntent intent,
                          uint8_t flags,
                          uint64_t correlation_id);

/// @brief Parsed view of an OpenConnection payload.
struct HdOpenConnection {
  uint16_t target_peer_id;
  uint16_t target_relay_id;
  HdIntent intent;
  uint8_t flags;
  uint64_t correlation_id;
};

/// @brief Parses an OpenConnection payload.
/// @returns True on success, false if payload is malformed.
bool HdParseOpenConnection(const uint8_t* payload,
                           int payload_len,
                           HdOpenConnection* out);

/// @brief Builds an OpenConnectionResult frame.
///
/// Payload layout:
///   [8B correlation_id][1B mode][2B deny_reason big-endian]
///   [1B sub_reason][2B relay_path_len big-endian]
///   [N * 2B relay_id entries]
///   [2B endpoint_hint_len big-endian][M bytes endpoint_hint]
/// @param buf Output buffer.
/// @param buf_size Size of output buffer.
/// @param correlation_id Correlation echoed from request.
/// @param mode Resolved connection mode.
/// @param deny_reason Deny reason (kNone when mode != Denied).
/// @param sub_reason Optional sub-reason byte.
/// @param relay_path Array of relay_ids for transit path,
///   may be nullptr when relay_path_len is 0.
/// @param relay_path_len Number of relay-path entries.
/// @param endpoint_hint UTF-8 string, may be nullptr when
///   endpoint_hint_len is 0.
/// @param endpoint_hint_len Length of endpoint_hint bytes.
/// @returns Total frame size written, or -1 on buffer
///   overflow or invalid-length arguments.
int HdBuildOpenConnectionResult(uint8_t* buf,
                                int buf_size,
                                uint64_t correlation_id,
                                HdConnMode mode,
                                HdDenyReason deny_reason,
                                uint8_t sub_reason,
                                const uint16_t* relay_path,
                                int relay_path_len,
                                const char* endpoint_hint,
                                int endpoint_hint_len);

/// Parsed view of an OpenConnectionResult payload.
struct HdOpenConnectionResult {
  uint64_t correlation_id;
  HdConnMode mode;
  HdDenyReason deny_reason;
  uint8_t sub_reason;
  uint16_t relay_path[kHdMaxRelayPath];
  int relay_path_len;
  char endpoint_hint[kHdMaxEndpointHint + 1];
  int endpoint_hint_len;
};

/// @brief Parses an OpenConnectionResult payload.
/// @returns True on success.
bool HdParseOpenConnectionResult(
    const uint8_t* payload,
    int payload_len,
    HdOpenConnectionResult* out);

/// @brief Builds an IncomingConnection frame.
///
/// Payload: [32B initiator_key][2B initiator_peer_id]
///          [1B intent][1B flags][8B correlation_id]
int HdBuildIncomingConnection(uint8_t* buf,
                              const Key& initiator_key,
                              uint16_t initiator_peer_id,
                              HdIntent intent,
                              uint8_t flags,
                              uint64_t correlation_id);

/// Parsed view of an IncomingConnection payload.
struct HdIncomingConnection {
  Key initiator_key;
  uint16_t initiator_peer_id;
  HdIntent intent;
  uint8_t flags;
  uint64_t correlation_id;
};

bool HdParseIncomingConnection(const uint8_t* payload,
                               int payload_len,
                               HdIncomingConnection* out);

/// @brief Builds an IncomingConnResponse frame.
///
/// Payload: [8B correlation_id][1B intent][1B flags]
///          [1B accept_flag]
int HdBuildIncomingConnResponse(uint8_t* buf,
                                uint64_t correlation_id,
                                HdIntent intent,
                                uint8_t flags,
                                uint8_t accept);

/// Parsed view of an IncomingConnResponse payload.
struct HdIncomingConnResponse {
  uint64_t correlation_id;
  HdIntent intent;
  uint8_t flags;
  uint8_t accept;
};

bool HdParseIncomingConnResponse(
    const uint8_t* payload,
    int payload_len,
    HdIncomingConnResponse* out);

/// @brief Builds an IncomingConnResult frame.
///
/// Payload: [8B correlation_id][1B mode][2B deny_reason]
///          [1B sub_reason]
int HdBuildIncomingConnResult(uint8_t* buf,
                              uint64_t correlation_id,
                              HdConnMode mode,
                              HdDenyReason deny_reason,
                              uint8_t sub_reason);

/// Parsed view of an IncomingConnResult payload.
struct HdIncomingConnResult {
  uint64_t correlation_id;
  HdConnMode mode;
  HdDenyReason deny_reason;
  uint8_t sub_reason;
};

bool HdParseIncomingConnResult(const uint8_t* payload,
                               int payload_len,
                               HdIncomingConnResult* out);

/// @brief Builds an HD Redirect frame.
///
/// Payload: [1B reason][N bytes target_relay_url].
/// @param buf Output buffer (must be large enough).
/// @param reason Redirect reason code.
/// @param target_url Target relay URL (UTF-8 string).
/// @param url_len Length of target_url in bytes.
/// @returns Total frame size written, or -1 if url_len
///   exceeds kHdRedirectMaxUrl.
int HdBuildRedirect(uint8_t* buf,
                    HdRedirectReason reason,
                    const char* target_url,
                    int url_len);

/// @brief Parses an HD Redirect frame payload.
/// @param payload Pointer to the payload start (after
///   frame header).
/// @param payload_len Length of the payload.
/// @param out_reason Output reason code.
/// @param out_url Output buffer for target URL (at least
///   kHdRedirectMaxUrl + 1 bytes for NUL).
/// @param out_url_size Size of out_url buffer in bytes.
/// @returns Length of URL written, or -1 on parse error.
int HdParseRedirect(const uint8_t* payload,
                    int payload_len,
                    HdRedirectReason* out_reason,
                    char* out_url,
                    int out_url_size);

/// Relay enrollment extension magic bytes.
inline constexpr char kHdRelayMagic[] = "RELAY";

/// Relay enrollment extension size (2B relay_id + 5B magic).
inline constexpr int kHdRelayExtSize = 7;

/// @brief Builds an Enroll frame with relay extension.
///
/// Layout: [4B hdr][32B key][32B hmac][2B relay_id]
///   ["RELAY"].
/// @param buf Output buffer.
/// @param client_key Client's 32-byte public key.
/// @param hmac HMAC data.
/// @param hmac_len Length of HMAC data.
/// @param relay_id This relay's ID.
/// @returns Total frame size written.
int HdBuildRelayEnroll(uint8_t* buf,
                       const Key& client_key,
                       const uint8_t* hmac,
                       int hmac_len,
                       uint16_t relay_id);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_PROTOCOL_H_
