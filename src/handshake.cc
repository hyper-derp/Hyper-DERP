/// @file handshake.cc
/// @brief DERP handshake: key generation, ServerKey/
///   ClientInfo/ServerInfo frame exchange.

#include "hyper_derp/handshake.h"

#include <cerrno>
#include <cstring>
#include <expected>
#include <memory>
#include <sodium.h>
#include <string_view>
#include <unistd.h>

#include "hyper_derp/error.h"

namespace hyper_derp {

auto GenerateServerKeys(ServerKeys* keys)
    -> std::expected<void, Error<HandshakeError>> {
  if (sodium_init() < 0) {
    return MakeError(HandshakeError::SodiumInitFailed,
                     "sodium_init failed");
  }
  crypto_box_keypair(keys->public_key.data(),
                     keys->private_key.data());
  return {};
}

void KeyToHex(const Key& key, char* hex) {
  sodium_bin2hex(hex, kKeySize * 2 + 1,
                 key.data(), kKeySize);
}

int BuildServerKeyFrame(uint8_t* buf,
                        const ServerKeys* server_keys) {
  int payload_len = kServerKeyPayloadSize;
  WriteFrameHeader(buf, FrameType::kServerKey,
                   static_cast<uint32_t>(payload_len));
  std::memcpy(buf + kFrameHeaderSize,
              kMagic.data(), kMagicSize);
  std::memcpy(buf + kFrameHeaderSize + kMagicSize,
              server_keys->public_key.data(), kKeySize);
  return kFrameHeaderSize + payload_len;
}

auto BuildServerInfoFrame(uint8_t* buf, int buf_size,
                          const ServerKeys* server_keys,
                          const Key& client_pub)
    -> std::expected<int, Error<HandshakeError>> {
  // ServerInfo JSON: {"version":2}
  std::string_view json = "{\"version\":2}";
  int json_len = static_cast<int>(json.size());

  int sealed_len = kNonceSize + json_len + kBoxOverhead;
  if (kFrameHeaderSize + sealed_len > buf_size) {
    return MakeError(HandshakeError::BadPayloadLength,
                     "ServerInfo frame exceeds buffer");
  }

  // Generate random nonce.
  uint8_t nonce[kNonceSize];
  randombytes_buf(nonce, kNonceSize);

  // Payload = [24B nonce][ciphertext]
  uint8_t* payload = buf + kFrameHeaderSize;
  std::memcpy(payload, nonce, kNonceSize);

  // NaCl box: encrypt JSON with server_priv + client_pub.
  if (crypto_box_easy(
          payload + kNonceSize,
          reinterpret_cast<const uint8_t*>(json.data()),
          json_len,
          nonce,
          client_pub.data(),
          server_keys->private_key.data()) != 0) {
    return MakeError(HandshakeError::EncryptionFailed,
                     "crypto_box_easy failed");
  }

  WriteFrameHeader(buf, FrameType::kServerInfo,
                   static_cast<uint32_t>(sealed_len));
  return kFrameHeaderSize + sealed_len;
}

auto ParseClientInfo(const uint8_t* payload,
                     int payload_len,
                     const ServerKeys* server_keys,
                     ClientInfo* info)
    -> std::expected<void, Error<HandshakeError>> {
  *info = {};

  if (payload_len < kMinClientInfoPayload) {
    return MakeError(HandshakeError::BadClientInfo,
                     "payload too short");
  }
  if (payload_len > kMaxClientInfoPayload) {
    return MakeError(HandshakeError::BadClientInfo,
                     "payload too long");
  }

  // First 32 bytes: client public key.
  info->public_key = ToKey(payload);

  // Remaining: [24B nonce][ciphertext + 16B tag]
  const uint8_t* sealed = payload + kKeySize;
  int sealed_len = payload_len - kKeySize;

  if (sealed_len < kNonceSize + kBoxOverhead) {
    return MakeError(HandshakeError::BadClientInfo,
                     "sealed data too short");
  }

  const uint8_t* nonce = sealed;
  const uint8_t* ciphertext = sealed + kNonceSize;
  int ciphertext_len = sealed_len - kNonceSize;

  int plaintext_len = ciphertext_len - kBoxOverhead;
  auto plaintext =
      std::make_unique<uint8_t[]>(plaintext_len + 1);

  // NaCl box open: decrypt with server_priv + client_pub.
  if (crypto_box_open_easy(
          plaintext.get(), ciphertext, ciphertext_len,
          nonce, info->public_key.data(),
          server_keys->private_key.data()) != 0) {
    return MakeError(HandshakeError::DecryptionFailed,
                     "crypto_box_open_easy failed");
  }
  plaintext[plaintext_len] = '\0';

  // Minimal JSON parsing for ClientInfo fields.
  std::string_view json(
      reinterpret_cast<const char*>(plaintext.get()),
      plaintext_len);

  auto find_int = [&](std::string_view key) -> int {
    auto pos = json.find(key);
    if (pos == std::string_view::npos) return -1;
    pos = json.find(':', pos + key.size());
    if (pos == std::string_view::npos) return -1;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    int val = 0;
    while (pos < json.size() &&
           json[pos] >= '0' && json[pos] <= '9') {
      val = val * 10 + (json[pos] - '0');
      pos++;
    }
    return val;
  };

  auto find_bool = [&](std::string_view key) -> bool {
    auto pos = json.find(key);
    if (pos == std::string_view::npos) return false;
    pos = json.find(':', pos + key.size());
    if (pos == std::string_view::npos) return false;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return json.substr(pos).starts_with("true");
  };

  int ver = find_int("\"version\"");
  if (ver > 0) info->version = ver;
  info->can_ack_pings = find_bool("\"CanAckPings\"");
  info->is_prober = find_bool("\"IsProber\"");

  return {};
}

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

auto PerformHandshake(int fd,
                      const ServerKeys* server_keys,
                      ClientInfo* info)
    -> std::expected<void, Error<HandshakeError>> {
  // Step 1: Send ServerKey frame.
  uint8_t sk_buf[kFrameHeaderSize + kServerKeyPayloadSize];
  int sk_len = BuildServerKeyFrame(sk_buf, server_keys);
  if (WriteAll(fd, sk_buf, sk_len) < 0) {
    return MakeError(HandshakeError::IoFailed,
                     "write ServerKey failed");
  }

  // Step 2: Read ClientInfo frame header.
  uint8_t hdr[kFrameHeaderSize];
  if (ReadAll(fd, hdr, kFrameHeaderSize) < 0) {
    return MakeError(HandshakeError::IoFailed,
                     "read ClientInfo header failed");
  }
  if (ReadFrameType(hdr) != FrameType::kClientInfo) {
    return MakeError(HandshakeError::UnexpectedFrame,
                     "expected ClientInfo frame");
  }
  uint32_t payload_len = ReadPayloadLen(hdr);
  if (payload_len < static_cast<uint32_t>(
          kMinClientInfoPayload)) {
    return MakeError(HandshakeError::BadPayloadLength,
                     "ClientInfo payload too short");
  }
  if (payload_len > static_cast<uint32_t>(
          kMaxClientInfoPayload)) {
    return MakeError(HandshakeError::BadPayloadLength,
                     "ClientInfo payload too long");
  }

  // Step 3: Read ClientInfo payload.
  auto payload =
      std::make_unique<uint8_t[]>(payload_len);
  if (ReadAll(fd, payload.get(),
              static_cast<int>(payload_len)) < 0) {
    return MakeError(HandshakeError::IoFailed,
                     "read ClientInfo payload failed");
  }

  // Step 4: Parse and decrypt ClientInfo.
  auto result = ParseClientInfo(
      payload.get(), static_cast<int>(payload_len),
      server_keys, info);
  if (!result) {
    return std::unexpected(result.error());
  }

  // Step 5: Send ServerInfo frame.
  uint8_t si_buf[256];
  auto si_result = BuildServerInfoFrame(
      si_buf, sizeof(si_buf),
      server_keys, info->public_key);
  if (!si_result) {
    return std::unexpected(si_result.error());
  }
  if (WriteAll(fd, si_buf, *si_result) < 0) {
    return MakeError(HandshakeError::IoFailed,
                     "write ServerInfo failed");
  }

  return {};
}

}  // namespace hyper_derp
