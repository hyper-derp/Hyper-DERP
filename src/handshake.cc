/// @file handshake.cc
/// @brief DERP handshake: key generation, ServerKey/
///   ClientInfo/ServerInfo frame exchange.

#include "hyper_derp/handshake.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sodium.h>
#include <unistd.h>

namespace hyper_derp {

int GenerateServerKeys(ServerKeys* keys) {
  if (sodium_init() < 0) {
    return -1;
  }
  // crypto_box_keypair generates a Curve25519 key pair.
  crypto_box_keypair(keys->public_key, keys->private_key);
  return 0;
}

void KeyToHex(const uint8_t* key, char* hex) {
  sodium_bin2hex(hex, kKeySize * 2 + 1, key, kKeySize);
}

int BuildServerKeyFrame(uint8_t* buf,
                        const ServerKeys* server_keys) {
  int payload_len = kServerKeyPayloadSize;
  WriteFrameHeader(buf, FrameType::kServerKey,
                   static_cast<uint32_t>(payload_len));
  memcpy(buf + kFrameHeaderSize, kMagic, kMagicSize);
  memcpy(buf + kFrameHeaderSize + kMagicSize,
         server_keys->public_key, kKeySize);
  return kFrameHeaderSize + payload_len;
}

int BuildServerInfoFrame(uint8_t* buf, int buf_size,
                         const ServerKeys* server_keys,
                         const uint8_t* client_pub) {
  // ServerInfo JSON: {"version":2}
  const char* json = "{\"version\":2}";
  int json_len = static_cast<int>(strlen(json));

  int sealed_len = kNonceSize + json_len + kBoxOverhead;
  if (kFrameHeaderSize + sealed_len > buf_size) {
    return -1;
  }

  // Generate random nonce.
  uint8_t nonce[kNonceSize];
  randombytes_buf(nonce, kNonceSize);

  // Payload = [24B nonce][ciphertext]
  uint8_t* payload = buf + kFrameHeaderSize;
  memcpy(payload, nonce, kNonceSize);

  // NaCl box: encrypt JSON with server_priv + client_pub.
  if (crypto_box_easy(
          payload + kNonceSize,
          reinterpret_cast<const uint8_t*>(json),
          json_len,
          nonce,
          client_pub,
          server_keys->private_key) != 0) {
    return -1;
  }

  WriteFrameHeader(buf, FrameType::kServerInfo,
                   static_cast<uint32_t>(sealed_len));
  return kFrameHeaderSize + sealed_len;
}

int ParseClientInfo(const uint8_t* payload,
                    int payload_len,
                    const ServerKeys* server_keys,
                    ClientInfo* info) {
  memset(info, 0, sizeof(*info));

  if (payload_len < kMinClientInfoPayload) {
    return -1;
  }
  if (payload_len > kMaxClientInfoPayload) {
    return -1;
  }

  // First 32 bytes: client public key.
  memcpy(info->public_key, payload, kKeySize);

  // Remaining: [24B nonce][ciphertext + 16B tag]
  const uint8_t* sealed = payload + kKeySize;
  int sealed_len = payload_len - kKeySize;

  if (sealed_len < kNonceSize + kBoxOverhead) {
    return -1;
  }

  const uint8_t* nonce = sealed;
  const uint8_t* ciphertext = sealed + kNonceSize;
  int ciphertext_len = sealed_len - kNonceSize;

  int plaintext_len = ciphertext_len - kBoxOverhead;
  auto* plaintext = static_cast<uint8_t*>(
      malloc(plaintext_len + 1));
  if (!plaintext) {
    return -1;
  }

  // NaCl box open: decrypt with server_priv + client_pub.
  if (crypto_box_open_easy(
          plaintext, ciphertext, ciphertext_len,
          nonce, info->public_key,
          server_keys->private_key) != 0) {
    free(plaintext);
    return -1;
  }
  plaintext[plaintext_len] = '\0';

  // Minimal JSON parsing for ClientInfo fields.
  // Fields: version, CanAckPings, IsProber
  info->version = kProtocolVersion;
  info->can_ack_pings = false;
  info->is_prober = false;

  const char* json =
      reinterpret_cast<const char*>(plaintext);
  const char* ver = strstr(json, "\"version\"");
  if (ver) {
    ver = strchr(ver + 9, ':');
    if (ver) {
      info->version = atoi(ver + 1);
    }
  }
  const char* ack = strstr(json, "\"CanAckPings\"");
  if (ack) {
    ack = strchr(ack + 13, ':');
    if (ack) {
      const char* val = ack + 1;
      while (*val == ' ') val++;
      info->can_ack_pings = (strncmp(val, "true", 4) == 0);
    }
  }
  const char* prober = strstr(json, "\"IsProber\"");
  if (prober) {
    prober = strchr(prober + 10, ':');
    if (prober) {
      const char* val = prober + 1;
      while (*val == ' ') val++;
      info->is_prober = (strncmp(val, "true", 4) == 0);
    }
  }

  free(plaintext);
  return 0;
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

int PerformHandshake(int fd,
                     const ServerKeys* server_keys,
                     ClientInfo* info) {
  // Step 1: Send ServerKey frame.
  uint8_t sk_buf[kFrameHeaderSize + kServerKeyPayloadSize];
  int sk_len = BuildServerKeyFrame(sk_buf, server_keys);
  if (WriteAll(fd, sk_buf, sk_len) < 0) {
    return -1;
  }

  // Step 2: Read ClientInfo frame header.
  uint8_t hdr[kFrameHeaderSize];
  if (ReadAll(fd, hdr, kFrameHeaderSize) < 0) {
    return -1;
  }
  if (ReadFrameType(hdr) != FrameType::kClientInfo) {
    return -1;
  }
  uint32_t payload_len = ReadPayloadLen(hdr);
  if (payload_len < static_cast<uint32_t>(
          kMinClientInfoPayload)) {
    return -1;
  }
  if (payload_len > static_cast<uint32_t>(
          kMaxClientInfoPayload)) {
    return -1;
  }

  // Step 3: Read ClientInfo payload.
  auto* payload = static_cast<uint8_t*>(
      malloc(payload_len));
  if (!payload) {
    return -1;
  }
  if (ReadAll(fd, payload,
              static_cast<int>(payload_len)) < 0) {
    free(payload);
    return -1;
  }

  // Step 4: Parse and decrypt ClientInfo.
  int rc = ParseClientInfo(payload,
                           static_cast<int>(payload_len),
                           server_keys, info);
  free(payload);
  if (rc < 0) {
    return -1;
  }

  // Step 5: Send ServerInfo frame.
  uint8_t si_buf[256];
  int si_len = BuildServerInfoFrame(
      si_buf, sizeof(si_buf),
      server_keys, info->public_key);
  if (si_len < 0) {
    return -1;
  }
  if (WriteAll(fd, si_buf, si_len) < 0) {
    return -1;
  }

  return 0;
}

}  // namespace hyper_derp
