/// @file client.cc
/// @brief Client-side DERP protocol implementation.

#include "hyper_derp/client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <expected>
#include <format>
#include <netinet/tcp.h>
#include <sodium.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/error.h"
#include "hyper_derp/handshake.h"

namespace hyper_derp {

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

auto ClientInit(DerpClient* c)
    -> std::expected<void, Error<ClientError>> {
  *c = {};
  if (sodium_init() < 0) {
    return MakeError(ClientError::SodiumInitFailed,
                     "sodium_init failed");
  }
  crypto_box_keypair(c->public_key.data(),
                     c->private_key.data());
  return {};
}

void ClientInitWithKeys(DerpClient* c,
                        const uint8_t* pub,
                        const uint8_t* priv) {
  *c = {};
  c->public_key = ToKey(pub);
  c->private_key = ToKey(priv);
}

auto ClientConnect(DerpClient* c, const char* host,
                   uint16_t port)
    -> std::expected<void, Error<ClientError>> {
  c->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0) {
    return MakeError(ClientError::SocketFailed,
                     strerror(errno));
  }

  int one = 1;
  setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY,
             &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(c->fd);
    c->fd = -1;
    return MakeError(ClientError::BadAddress, host);
  }

  if (connect(c->fd,
              reinterpret_cast<sockaddr*>(&addr),
              sizeof(addr)) < 0) {
    close(c->fd);
    c->fd = -1;
    return MakeError(ClientError::ConnectFailed,
                     strerror(errno));
  }

  c->host = host;
  c->port = port;
  c->connected = true;
  return {};
}

auto ClientUpgrade(DerpClient* c)
    -> std::expected<void, Error<ClientError>> {
  char req[256];
  auto r = std::format_to_n(req, sizeof(req) - 1,
      "GET /derp HTTP/1.1\r\n"
      "Host: {}:{}\r\n"
      "Upgrade: DERP\r\n"
      "Connection: Upgrade\r\n"
      "\r\n",
      c->host, c->port);
  *r.out = '\0';
  int req_len = static_cast<int>(r.size);
  if (WriteAll(c->fd,
               reinterpret_cast<const uint8_t*>(req),
               req_len) < 0) {
    return MakeError(ClientError::IoFailed,
                     "write upgrade request failed");
  }

  // Read HTTP response one byte at a time to avoid
  // consuming post-header data (ServerKey frame may
  // arrive in the same TCP segment).
  uint8_t buf[1024];
  int total = 0;
  while (total < static_cast<int>(sizeof(buf)) - 1) {
    int n = read(c->fd, buf + total, 1);
    if (n <= 0) {
      return MakeError(ClientError::IoFailed,
                       "read upgrade response failed");
    }
    total += n;
    if (total >= 4 &&
        buf[total - 4] == '\r' &&
        buf[total - 3] == '\n' &&
        buf[total - 2] == '\r' &&
        buf[total - 1] == '\n') {
      break;
    }
  }
  buf[total] = '\0';

  std::string_view resp(
      reinterpret_cast<char*>(buf), total);
  if (resp.find("101") == std::string_view::npos) {
    return MakeError(ClientError::UpgradeFailed,
                     "no 101 in response");
  }
  return {};
}

auto ClientHandshake(DerpClient* c)
    -> std::expected<void, Error<ClientError>> {
  // Receive ServerKey frame.
  uint8_t hdr[kFrameHeaderSize];
  if (ReadAll(c->fd, hdr, kFrameHeaderSize) < 0) {
    return MakeError(ClientError::IoFailed,
                     "read ServerKey header failed");
  }
  if (ReadFrameType(hdr) != FrameType::kServerKey) {
    return MakeError(ClientError::UnexpectedFrame,
                     "expected ServerKey frame");
  }
  uint32_t plen = ReadPayloadLen(hdr);
  if (plen != kServerKeyPayloadSize) {
    return MakeError(ClientError::BadPayloadLength,
                     "bad ServerKey payload length");
  }

  uint8_t sk_payload[kServerKeyPayloadSize];
  if (ReadAll(c->fd, sk_payload,
              static_cast<int>(plen)) < 0) {
    return MakeError(ClientError::IoFailed,
                     "read ServerKey payload failed");
  }
  if (std::memcmp(sk_payload,
                  kMagic.data(), kMagicSize) != 0) {
    return MakeError(ClientError::BadMagic,
                     "ServerKey magic mismatch");
  }
  c->server_key = ToKey(sk_payload + kMagicSize);

  // Send ClientInfo frame.
  std::string_view json =
      "{\"version\":2,\"CanAckPings\":true}";
  int json_len = static_cast<int>(json.size());

  uint8_t nonce[kNonceSize];
  randombytes_buf(nonce, kNonceSize);

  int ciphertext_len = json_len + kBoxOverhead;
  int ci_payload_len =
      kKeySize + kNonceSize + ciphertext_len;
  int ci_frame_len = kFrameHeaderSize + ci_payload_len;

  // Stack buffer: header + key + nonce + ciphertext.
  uint8_t ci_buf[kFrameHeaderSize + kKeySize +
                 kNonceSize + 256];
  if (ci_frame_len > static_cast<int>(sizeof(ci_buf))) {
    return MakeError(ClientError::BufferOverflow,
                     "ClientInfo frame too large");
  }

  WriteFrameHeader(ci_buf, FrameType::kClientInfo,
                   static_cast<uint32_t>(ci_payload_len));
  std::memcpy(ci_buf + kFrameHeaderSize,
              c->public_key.data(), kKeySize);
  std::memcpy(ci_buf + kFrameHeaderSize + kKeySize,
              nonce, kNonceSize);

  if (crypto_box_easy(
          ci_buf + kFrameHeaderSize + kKeySize +
              kNonceSize,
          reinterpret_cast<const uint8_t*>(json.data()),
          json_len,
          nonce, c->server_key.data(),
          c->private_key.data()) != 0) {
    return MakeError(ClientError::EncryptionFailed,
                     "crypto_box_easy failed");
  }

  if (WriteAll(c->fd, ci_buf, ci_frame_len) < 0) {
    return MakeError(ClientError::IoFailed,
                     "write ClientInfo failed");
  }

  // Receive ServerInfo frame.
  if (ReadAll(c->fd, hdr, kFrameHeaderSize) < 0) {
    return MakeError(ClientError::IoFailed,
                     "read ServerInfo header failed");
  }
  if (ReadFrameType(hdr) != FrameType::kServerInfo) {
    return MakeError(ClientError::UnexpectedFrame,
                     "expected ServerInfo frame");
  }
  plen = ReadPayloadLen(hdr);
  if (plen > 1024) {
    return MakeError(ClientError::BadPayloadLength,
                     "ServerInfo payload too large");
  }

  uint8_t si_buf[1024];
  if (ReadAll(c->fd, si_buf,
              static_cast<int>(plen)) < 0) {
    return MakeError(ClientError::IoFailed,
                     "read ServerInfo payload failed");
  }

  return {};
}

auto ClientSendPacket(DerpClient* c,
                      const Key& dst_key,
                      const uint8_t* data, int data_len)
    -> std::expected<void, Error<ClientError>> {
  int payload_len = kKeySize + data_len;
  if (payload_len > kMaxFramePayload) {
    return MakeError(ClientError::BufferOverflow,
                     "packet exceeds max frame payload");
  }

  uint8_t hdr[kFrameHeaderSize];
  WriteFrameHeader(hdr, FrameType::kSendPacket,
                   static_cast<uint32_t>(payload_len));

  if (WriteAll(c->fd, hdr, kFrameHeaderSize) < 0) {
    return MakeError(ClientError::IoFailed,
                     "write SendPacket header failed");
  }
  if (WriteAll(c->fd, dst_key.data(), kKeySize) < 0) {
    return MakeError(ClientError::IoFailed,
                     "write SendPacket key failed");
  }
  if (data_len > 0) {
    if (WriteAll(c->fd, data, data_len) < 0) {
      return MakeError(ClientError::IoFailed,
                       "write SendPacket data failed");
    }
  }
  return {};
}

auto ClientRecvFrame(DerpClient* c, FrameType* type,
                     uint8_t* payload, int* payload_len,
                     int buf_size)
    -> std::expected<void, Error<ClientError>> {
  uint8_t hdr[kFrameHeaderSize];
  if (ReadAll(c->fd, hdr, kFrameHeaderSize) < 0) {
    return MakeError(ClientError::IoFailed,
                     "read frame header failed");
  }

  *type = ReadFrameType(hdr);
  uint32_t len = ReadPayloadLen(hdr);
  if (static_cast<int>(len) > buf_size) {
    return MakeError(ClientError::BufferOverflow,
                     "frame payload exceeds buffer");
  }

  if (len > 0) {
    if (ReadAll(c->fd, payload,
                static_cast<int>(len)) < 0) {
      return MakeError(ClientError::IoFailed,
                       "read frame payload failed");
    }
  }
  *payload_len = static_cast<int>(len);
  return {};
}

void ClientClose(DerpClient* c) {
  if (c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  c->connected = false;
}

auto ClientSetTimeout(DerpClient* c, int ms)
    -> std::expected<void, Error<ClientError>> {
  timeval tv{
      .tv_sec = ms / 1000,
      .tv_usec = (ms % 1000) * 1000};
  if (setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO,
                 &tv, sizeof(tv)) < 0) {
    return MakeError(ClientError::IoFailed,
                     "setsockopt SO_RCVTIMEO failed");
  }
  return {};
}

}  // namespace hyper_derp
