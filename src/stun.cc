/// @file stun.cc
/// @brief STUN message codec implementation (RFC 5389).

#include "hyper_derp/stun.h"

#include <arpa/inet.h>
#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace hyper_derp {

namespace {

/// Write a 16-bit value in big-endian at the given offset.
void WriteBe16(uint8_t* buf, uint16_t val) {
  buf[0] = static_cast<uint8_t>(val >> 8);
  buf[1] = static_cast<uint8_t>(val);
}

/// Write a 32-bit value in big-endian at the given offset.
void WriteBe32(uint8_t* buf, uint32_t val) {
  buf[0] = static_cast<uint8_t>(val >> 24);
  buf[1] = static_cast<uint8_t>(val >> 16);
  buf[2] = static_cast<uint8_t>(val >> 8);
  buf[3] = static_cast<uint8_t>(val);
}

/// Read a 16-bit big-endian value.
uint16_t ReadBe16(const uint8_t* buf) {
  return static_cast<uint16_t>(
      (static_cast<uint16_t>(buf[0]) << 8) | buf[1]);
}

/// Read a 32-bit big-endian value.
uint32_t ReadBe32(const uint8_t* buf) {
  return (static_cast<uint32_t>(buf[0]) << 24) |
         (static_cast<uint32_t>(buf[1]) << 16) |
         (static_cast<uint32_t>(buf[2]) << 8) |
         static_cast<uint32_t>(buf[3]);
}

/// Round up to 4-byte boundary.
int PadTo4(int len) {
  return (len + 3) & ~3;
}

/// Write a STUN header into buf.
void WriteStunHeader(uint8_t* buf, uint16_t type,
                     uint16_t payload_len,
                     const uint8_t* txn_id) {
  WriteBe16(buf, type);
  WriteBe16(buf + 2, payload_len);
  WriteBe32(buf + 4, kStunMagicCookie);
  std::memcpy(buf + 8, txn_id, kStunTransactionIdSize);
}

/// Parse a single XOR-MAPPED-ADDRESS attribute value.
void ParseXorMappedAddress(const uint8_t* val, int val_len,
                           StunMessage* msg) {
  // Minimum: 1 reserved + 1 family + 2 port + 4 addr = 8.
  if (val_len < 8) return;
  uint8_t family = val[1];
  if (family != kStunAddrFamilyIPv4) return;
  msg->has_xor_mapped = true;
  msg->xor_mapped_port = ReadBe16(val + 2);
  msg->xor_mapped_ip = ReadBe32(val + 4);
}

/// Parse ERROR-CODE attribute value.
void ParseErrorCode(const uint8_t* val, int val_len,
                    StunMessage* msg) {
  // Minimum 4 bytes: 2 reserved + class + number.
  if (val_len < 4) return;
  int error_class = val[2] & 0x07;
  int error_number = val[3];
  msg->has_error = true;
  msg->error_code =
      static_cast<uint16_t>(error_class * 100 + error_number);
  int reason_len = val_len - 4;
  if (reason_len > 0) {
    int copy_len = std::min(reason_len,
        static_cast<int>(sizeof(msg->error_reason) - 1));
    std::memcpy(msg->error_reason, val + 4, copy_len);
    msg->error_reason[copy_len] = '\0';
    msg->error_reason_len = copy_len;
  }
}

}  // namespace

bool StunIsStun(const uint8_t* data, int len) {
  if (len < kStunHeaderSize) return false;
  // First two bits must be zero.
  if ((data[0] & 0xC0) != 0) return false;
  uint32_t cookie = ReadBe32(data + 4);
  return cookie == kStunMagicCookie;
}

bool StunParse(const uint8_t* data, int len,
               StunMessage* msg) {
  if (len < kStunHeaderSize) return false;
  // First two bits must be zero.
  if ((data[0] & 0xC0) != 0) return false;

  msg->type = ReadBe16(data);
  uint16_t payload_len = ReadBe16(data + 2);
  uint32_t cookie = ReadBe32(data + 4);
  if (cookie != kStunMagicCookie) return false;
  if (kStunHeaderSize + payload_len > len) return false;

  std::memcpy(msg->transaction_id, data + 8,
              kStunTransactionIdSize);

  // Walk attributes.
  int offset = kStunHeaderSize;
  int end = kStunHeaderSize + payload_len;
  while (offset + 4 <= end) {
    uint16_t attr_type = ReadBe16(data + offset);
    uint16_t attr_len = ReadBe16(data + offset + 2);
    int val_offset = offset + 4;
    if (val_offset + attr_len > end) break;
    const uint8_t* val = data + val_offset;

    switch (attr_type) {
      case kStunAttrXorMappedAddress:
        ParseXorMappedAddress(val, attr_len, msg);
        break;
      case kStunAttrUsername:
        if (attr_len > 0) {
          int copy_len = std::min(
              static_cast<int>(attr_len),
              static_cast<int>(sizeof(msg->username) - 1));
          std::memcpy(msg->username, val, copy_len);
          msg->username[copy_len] = '\0';
          msg->username_len = copy_len;
          msg->has_username = true;
        }
        break;
      case kStunAttrMessageIntegrity:
        if (attr_len == 20) {
          std::memcpy(msg->integrity, val, 20);
          msg->has_integrity = true;
        }
        break;
      case kStunAttrFingerprint:
        if (attr_len == 4) {
          msg->fingerprint = ReadBe32(val);
          msg->has_fingerprint = true;
        }
        break;
      case kStunAttrErrorCode:
        ParseErrorCode(val, attr_len, msg);
        break;
      default:
        break;
    }
    // Advance past value + padding to 4-byte boundary.
    offset = val_offset + PadTo4(attr_len);
  }
  return true;
}

int StunBuildBindingRequest(uint8_t* buf, int buf_size,
                            const uint8_t* txn_id) {
  if (buf_size < kStunHeaderSize) return 0;
  uint8_t id[kStunTransactionIdSize];
  if (txn_id) {
    std::memcpy(id, txn_id, kStunTransactionIdSize);
  } else {
    StunGenerateTransactionId(id);
  }
  WriteStunHeader(buf, kStunBindingRequest, 0, id);
  return kStunHeaderSize;
}

int StunBuildBindingResponse(uint8_t* buf, int buf_size,
                             const uint8_t* txn_id,
                             uint32_t client_ip,
                             uint16_t client_port) {
  // Header (20) + attr header (4) + attr value (8) = 32.
  constexpr int kXorAttrValueLen = 8;
  constexpr int kAttrHeaderLen = 4;
  constexpr int kTotalAttrLen =
      kAttrHeaderLen + kXorAttrValueLen;
  constexpr int kTotalLen = kStunHeaderSize + kTotalAttrLen;
  if (buf_size < kTotalLen) return 0;

  WriteStunHeader(buf, kStunBindingResponse,
                  kTotalAttrLen, txn_id);

  int off = kStunHeaderSize;
  // Attribute type: XOR-MAPPED-ADDRESS.
  WriteBe16(buf + off, kStunAttrXorMappedAddress);
  off += 2;
  // Attribute length.
  WriteBe16(buf + off, kXorAttrValueLen);
  off += 2;
  // Reserved byte.
  buf[off] = 0x00;
  off += 1;
  // Family.
  buf[off] = kStunAddrFamilyIPv4;
  off += 1;
  // XOR'd port: client_port XOR upper 16 bits of cookie.
  uint16_t xor_port =
      client_port ^ htons(0x2112);
  WriteBe16(buf + off, ntohs(xor_port));
  off += 2;
  // XOR'd address: client_ip XOR cookie.
  uint32_t xor_addr =
      client_ip ^ htonl(kStunMagicCookie);
  WriteBe32(buf + off, ntohl(xor_addr));
  return kTotalLen;
}

int StunBuildErrorResponse(uint8_t* buf, int buf_size,
                           const uint8_t* txn_id,
                           uint16_t error_code,
                           const char* reason) {
  int reason_len = reason ? static_cast<int>(strlen(reason))
                          : 0;
  // Error-code value: 4 bytes fixed + reason.
  int attr_value_len = 4 + reason_len;
  int padded_value_len = PadTo4(attr_value_len);
  int attr_total = 4 + padded_value_len;
  int total = kStunHeaderSize + attr_total;
  if (buf_size < total) return 0;

  WriteStunHeader(buf, kStunBindingError,
                  static_cast<uint16_t>(attr_total), txn_id);

  int off = kStunHeaderSize;
  // Attribute type: ERROR-CODE.
  WriteBe16(buf + off, kStunAttrErrorCode);
  off += 2;
  // Attribute length (unpadded).
  WriteBe16(buf + off, static_cast<uint16_t>(attr_value_len));
  off += 2;
  // 2 reserved bytes.
  buf[off] = 0x00;
  buf[off + 1] = 0x00;
  off += 2;
  // Error class (hundreds digit) and number (remainder).
  buf[off] = static_cast<uint8_t>(error_code / 100);
  off += 1;
  buf[off] = static_cast<uint8_t>(error_code % 100);
  off += 1;
  // Reason phrase.
  if (reason_len > 0) {
    std::memcpy(buf + off, reason, reason_len);
    off += reason_len;
  }
  // Zero padding bytes.
  int pad = padded_value_len - attr_value_len;
  if (pad > 0) {
    std::memset(buf + off, 0, pad);
  }
  return total;
}

void StunDecodeXorAddress(uint16_t xor_port,
                          uint32_t xor_addr,
                          uint16_t* out_port,
                          uint32_t* out_addr) {
  *out_port = xor_port ^ htons(0x2112);
  *out_addr = xor_addr ^ htonl(kStunMagicCookie);
}

void StunGenerateTransactionId(uint8_t* txn_id) {
  randombytes_buf(txn_id, kStunTransactionIdSize);
}

}  // namespace hyper_derp
