/// @file stun.h
/// @brief STUN message codec (RFC 5389).

#ifndef INCLUDE_HYPER_DERP_STUN_H_
#define INCLUDE_HYPER_DERP_STUN_H_

#include <cstdint>

namespace hyper_derp {

// -- STUN wire constants ----------------------------------------------------

/// STUN header size in bytes.
inline constexpr int kStunHeaderSize = 20;

/// STUN magic cookie (RFC 5389 section 6).
inline constexpr uint32_t kStunMagicCookie = 0x2112A442;

/// STUN transaction ID size in bytes.
inline constexpr int kStunTransactionIdSize = 12;

// -- STUN message types (method + class) ------------------------------------
// Method: Binding = 0x001
// Class: Request = 0x00, Response = 0x10, Error = 0x11

/// Binding Request.
inline constexpr uint16_t kStunBindingRequest = 0x0001;

/// Binding Success Response.
inline constexpr uint16_t kStunBindingResponse = 0x0101;

/// Binding Error Response.
inline constexpr uint16_t kStunBindingError = 0x0111;

// -- STUN attribute types ---------------------------------------------------

/// MAPPED-ADDRESS attribute.
inline constexpr uint16_t kStunAttrMappedAddress = 0x0001;

/// XOR-MAPPED-ADDRESS attribute.
inline constexpr uint16_t kStunAttrXorMappedAddress = 0x0020;

/// USERNAME attribute.
inline constexpr uint16_t kStunAttrUsername = 0x0006;

/// MESSAGE-INTEGRITY attribute (HMAC-SHA1).
inline constexpr uint16_t kStunAttrMessageIntegrity = 0x0008;

/// FINGERPRINT attribute (CRC-32).
inline constexpr uint16_t kStunAttrFingerprint = 0x8028;

/// ERROR-CODE attribute.
inline constexpr uint16_t kStunAttrErrorCode = 0x0009;

/// SOFTWARE attribute.
inline constexpr uint16_t kStunAttrSoftware = 0x8022;

// -- Address families -------------------------------------------------------

/// IPv4 address family.
inline constexpr uint8_t kStunAddrFamilyIPv4 = 0x01;

/// IPv6 address family.
inline constexpr uint8_t kStunAddrFamilyIPv6 = 0x02;

// -- Structs ----------------------------------------------------------------

/// STUN message header (on-wire layout reference).
struct StunHeader {
  uint16_t type;
  uint16_t length;
  uint32_t cookie;
  uint8_t transaction_id[kStunTransactionIdSize];
};

/// XOR-MAPPED-ADDRESS attribute value (IPv4).
struct StunXorMappedAddress {
  uint8_t family;
  uint16_t port;
  uint32_t addr;
};

/// Parsed STUN message with extracted attributes.
struct StunMessage {
  uint16_t type = 0;
  uint8_t transaction_id[kStunTransactionIdSize]{};
  // XOR-MAPPED-ADDRESS (if present).
  bool has_xor_mapped = false;
  uint32_t xor_mapped_ip = 0;
  uint16_t xor_mapped_port = 0;
  // USERNAME (if present).
  bool has_username = false;
  char username[256]{};
  int username_len = 0;
  // MESSAGE-INTEGRITY (if present).
  bool has_integrity = false;
  uint8_t integrity[20]{};
  // FINGERPRINT (if present).
  bool has_fingerprint = false;
  uint32_t fingerprint = 0;
  // ERROR-CODE (if present).
  bool has_error = false;
  uint16_t error_code = 0;
  char error_reason[128]{};
  int error_reason_len = 0;
};

// -- Functions --------------------------------------------------------------

/// Parse a STUN message from a buffer.
/// Returns true if valid STUN (magic cookie matches,
/// length consistent). Populates msg with parsed attrs.
bool StunParse(const uint8_t* data, int len,
               StunMessage* msg);

/// Check if a buffer starts with a valid STUN header.
bool StunIsStun(const uint8_t* data, int len);

/// Build a STUN Binding Request.
/// Generates random transaction ID if txn_id is nullptr.
/// Returns bytes written, or 0 on error.
int StunBuildBindingRequest(uint8_t* buf, int buf_size,
                            const uint8_t* txn_id);

/// Build a STUN Binding Response with XOR-MAPPED-ADDRESS.
/// @param buf Output buffer.
/// @param buf_size Size of output buffer.
/// @param txn_id Transaction ID from the request.
/// @param client_ip Client's IP in network byte order.
/// @param client_port Client's port in network byte order.
/// @return Bytes written, or 0 on error.
int StunBuildBindingResponse(uint8_t* buf, int buf_size,
                             const uint8_t* txn_id,
                             uint32_t client_ip,
                             uint16_t client_port);

/// Build a STUN Binding Error Response.
/// @param buf Output buffer.
/// @param buf_size Size of output buffer.
/// @param txn_id Transaction ID from the request.
/// @param error_code STUN error code (e.g. 400, 401).
/// @param reason Human-readable reason string.
/// @return Bytes written, or 0 on error.
int StunBuildErrorResponse(uint8_t* buf, int buf_size,
                           const uint8_t* txn_id,
                           uint16_t error_code,
                           const char* reason);

/// XOR-decode a mapped address.
/// @param xor_port XOR'd port from attribute.
/// @param xor_addr XOR'd IPv4 addr from attribute.
/// @param out_port Decoded port (network byte order).
/// @param out_addr Decoded addr (network byte order).
void StunDecodeXorAddress(uint16_t xor_port,
                          uint32_t xor_addr,
                          uint16_t* out_port,
                          uint32_t* out_addr);

/// Generate a random transaction ID.
void StunGenerateTransactionId(uint8_t* txn_id);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_STUN_H_
