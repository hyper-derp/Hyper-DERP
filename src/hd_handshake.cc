/// @file hd_handshake.cc
/// @brief HD enrollment handshake: Enroll frame reading,
///   HMAC validation, Approved/Denied frame sending.

#include "hyper_derp/hd_handshake.h"

#include <cerrno>
#include <cstring>
#include <expected>
#include <memory>
#include <unistd.h>

#include "hyper_derp/error.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/hd_protocol.h"

namespace hyper_derp {

// Blocking read of exactly n bytes.
static int ReadAll(int fd, uint8_t* buf, int n) {
  int total = 0;
  while (total < n) {
    int r = read(fd, buf + total, n - total);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (r == 0) {
      return -1;
    }
    total += r;
  }
  return 0;
}

// Blocking write of exactly n bytes.
static int WriteAll(int fd, const uint8_t* buf, int n) {
  int total = 0;
  while (total < n) {
    int w = write(fd, buf + total, n - total);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return -1;
    }
    if (w == 0) {
      return -1;
    }
    total += w;
  }
  return 0;
}

/// Minimum Enroll payload: 32-byte key + 32-byte HMAC.
static constexpr int kMinEnrollPayload =
    kKeySize + kHdHmacSize;

auto HdPerformHandshake(int fd,
                        HdPeerRegistry* reg,
                        HdEnrollResult* result,
                        uint32_t peer_ipv4_be)
    -> std::expected<void, Error<HdHandshakeError>> {
  *result = {};

  // Step 1: Read HD frame header.
  uint8_t hdr[kHdFrameHeaderSize];
  if (ReadAll(fd, hdr, kHdFrameHeaderSize) < 0) {
    return MakeError(HdHandshakeError::IoFailed,
                     "read Enroll header failed");
  }

  // Step 2: Verify frame type.
  if (HdReadFrameType(hdr) != HdFrameType::kEnroll) {
    return MakeError(HdHandshakeError::UnexpectedFrame,
                     "expected Enroll frame");
  }

  // Step 3: Validate payload length.
  uint32_t payload_len = HdReadPayloadLen(hdr);
  if (payload_len < static_cast<uint32_t>(
          kMinEnrollPayload)) {
    return MakeError(HdHandshakeError::BadPayloadLength,
                     "Enroll payload too short");
  }
  if (!HdIsValidPayloadLen(payload_len)) {
    return MakeError(HdHandshakeError::BadPayloadLength,
                     "Enroll payload too long");
  }

  // Step 4: Read payload.
  auto payload =
      std::make_unique<uint8_t[]>(payload_len);
  if (ReadAll(fd, payload.get(),
              static_cast<int>(payload_len)) < 0) {
    return MakeError(HdHandshakeError::IoFailed,
                     "read Enroll payload failed");
  }

  // Step 5: Extract client key and HMAC.
  Key client_key = ToKey(payload.get());
  const uint8_t* hmac = payload.get() + kKeySize;

  // Step 6: Verify HMAC.
  if (!HdVerifyEnrollment(reg, client_key, hmac)) {
    // Send Denied frame before returning error.
    HdSendDenied(fd, 0x01, "invalid HMAC");
    return MakeError(HdHandshakeError::InvalidHmac,
                     "enrollment HMAC verification failed");
  }

  // Step 6a: Persistent denylist check (survives removal
  // of the in-memory registry slot).
  if (HdPeersIsDenied(reg, client_key.data())) {
    HdSendDenied(fd, 0x03, "revoked");
    return MakeError(HdHandshakeError::EnrollmentDenied,
                     "client key revoked");
  }

  // Step 6.5: Detect relay enrollment extension.
  // Relay Enroll: [32B key][32B hmac][2B relay_id]
  //   ["RELAY"].
  int ext_offset = kKeySize + kHdHmacSize;
  if (static_cast<int>(payload_len) >=
      ext_offset + kHdRelayExtSize &&
      std::memcmp(payload.get() + ext_offset + 2,
                  kHdRelayMagic, 5) == 0) {
    result->is_relay = true;
    result->relay_id = static_cast<uint16_t>(
        (payload[ext_offset] << 8) |
        payload[ext_offset + 1]);
  }

  // Step 7: Apply policy (auto-approve mode only).
  if (reg->enroll_mode == HdEnrollMode::kAutoApprove) {
    const char* reject_reason = nullptr;
    if (!HdPolicyAllows(reg, client_key, peer_ipv4_be,
                        &reject_reason)) {
      HdSendDenied(fd, 0x02,
                   reject_reason ? reject_reason
                                 : "policy rejected");
      return MakeError(HdHandshakeError::EnrollmentDenied,
                       reject_reason ? reject_reason
                                     : "policy rejected");
    }
  }

  // Step 8: Insert peer into registry.
  HdPeersInsert(reg, client_key, fd);

  // Step 9: Auto-approve if configured.
  if (reg->enroll_mode == HdEnrollMode::kAutoApprove) {
    HdPeersApprove(reg, client_key.data());
    auto send_result = HdSendApproved(fd, client_key);
    if (!send_result) {
      return std::unexpected(send_result.error());
    }
    result->auto_approved = true;
  }

  // Step 9: Set result.
  result->client_key = client_key;
  return {};
}

auto HdSendApproved(int fd, const Key& client_key)
    -> std::expected<void, Error<HdHandshakeError>> {
  uint8_t buf[kHdFrameHeaderSize + kKeySize];
  int n = HdBuildApproved(buf, client_key);
  if (WriteAll(fd, buf, n) < 0) {
    return MakeError(HdHandshakeError::IoFailed,
                     "write Approved frame failed");
  }
  return {};
}

auto HdSendDenied(int fd, uint8_t reason,
                  const char* message)
    -> std::expected<void, Error<HdHandshakeError>> {
  int msg_len = static_cast<int>(std::strlen(message));
  // Header + 1 byte reason + message.
  int frame_size = kHdFrameHeaderSize + 1 + msg_len;
  auto buf = std::make_unique<uint8_t[]>(frame_size);
  int n = HdBuildDenied(buf.get(), reason,
                        message, msg_len);
  if (WriteAll(fd, buf.get(), n) < 0) {
    return MakeError(HdHandshakeError::IoFailed,
                     "write Denied frame failed");
  }
  return {};
}

auto HdSendRedirect(int fd,
                    HdRedirectReason reason,
                    std::string_view target_url)
    -> std::expected<void, Error<HdHandshakeError>> {
  int url_len = static_cast<int>(target_url.size());
  if (url_len > kHdRedirectMaxUrl) {
    return MakeError(HdHandshakeError::BadPayloadLength,
                     "redirect url exceeds max length");
  }
  int frame_size = kHdFrameHeaderSize + 1 + url_len;
  auto buf = std::make_unique<uint8_t[]>(frame_size);
  int n = HdBuildRedirect(buf.get(), reason,
                          target_url.data(), url_len);
  if (n < 0) {
    return MakeError(HdHandshakeError::BadPayloadLength,
                     "HdBuildRedirect rejected inputs");
  }
  if (WriteAll(fd, buf.get(), n) < 0) {
    return MakeError(HdHandshakeError::IoFailed,
                     "write Redirect frame failed");
  }
  return {};
}

}  // namespace hyper_derp
