/// @file client.cc
/// @brief Client-side DERP protocol implementation.

#include "hyper_derp/client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <expected>
#include <format>
#include <netinet/tcp.h>
#include <poll.h>
#include <sodium.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "hyper_derp/error.h"
#include "hyper_derp/handshake.h"

namespace hyper_derp {

static int ReadAllFd(int fd, uint8_t* buf, int n) {
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

static int ReadAllSsl(SSL* ssl, uint8_t* buf, int n) {
  int total = 0;
  while (total < n) {
    int r = SSL_read(ssl, buf + total, n - total);
    if (r <= 0) return -1;
    total += r;
  }
  return 0;
}

static int ReadAll(DerpClient* c, uint8_t* buf, int n) {
  if (c->ssl) return ReadAllSsl(c->ssl, buf, n);
  return ReadAllFd(c->fd, buf, n);
}

static int WriteAllFd(int fd, const uint8_t* buf, int n) {
  int total = 0;
  while (total < n) {
    int w = write(fd, buf + total, n - total);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        int r = poll(&pfd, 1, 5000);
        if (r <= 0) return -1;
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

static int WriteAllSsl(SSL* ssl, const uint8_t* buf,
                       int n) {
  int total = 0;
  while (total < n) {
    int w = SSL_write(ssl, buf + total, n - total);
    if (w <= 0) return -1;
    total += w;
  }
  return 0;
}

static int WriteAll(DerpClient* c, const uint8_t* buf,
                    int n) {
  if (c->ssl) return WriteAllSsl(c->ssl, buf, n);
  return WriteAllFd(c->fd, buf, n);
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

auto ClientTlsConnect(DerpClient* c)
    -> std::expected<void, Error<ClientError>> {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    return MakeError(ClientError::IoFailed,
                     "SSL_CTX_new failed");
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
  SSL_CTX_set_num_tickets(ctx, 0);

  SSL* ssl = SSL_new(ctx);
  if (!ssl) {
    SSL_CTX_free(ctx);
    return MakeError(ClientError::IoFailed,
                     "SSL_new failed");
  }

  SSL_set_fd(ssl, c->fd);
  // Set SNI for servers that require it (e.g. Go's TLS).
  SSL_set_tlsext_host_name(ssl, c->host.c_str());
  int ret = SSL_connect(ssl);
  if (ret != 1) {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return MakeError(ClientError::IoFailed,
                     std::string("SSL_connect: ") + buf);
  }

  // Keep SSL alive for userspace TLS. Using SSL_read/
  // SSL_write ensures compatibility with all TLS servers
  // (kTLS and Go's crypto/tls). The test client doesn't
  // benefit from client-side kTLS — it's the server's
  // kTLS that matters for throughput.
  c->ssl = ssl;
  c->ssl_ctx = ctx;
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
  if (WriteAll(c,
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
    int n;
    if (c->ssl) {
      n = SSL_read(c->ssl, buf + total, 1);
    } else {
      n = read(c->fd, buf + total, 1);
    }
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
  if (ReadAll(c, hdr, kFrameHeaderSize) < 0) {
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
  if (ReadAll(c, sk_payload,
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

  if (WriteAll(c, ci_buf, ci_frame_len) < 0) {
    return MakeError(ClientError::IoFailed,
                     "write ClientInfo failed");
  }

  // Receive ServerInfo frame.
  if (ReadAll(c, hdr, kFrameHeaderSize) < 0) {
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
  if (ReadAll(c, si_buf,
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

  // Single write produces one TLS record under kTLS.
  // Three separate writes would create three records,
  // tripling per-frame crypto overhead.
  int frame_len = kFrameHeaderSize + payload_len;
  uint8_t stack[kFrameHeaderSize + kKeySize + 2048];
  uint8_t* buf = stack;
  bool heap = false;
  if (frame_len > static_cast<int>(sizeof(stack))) {
    buf = new uint8_t[frame_len];
    heap = true;
  }

  WriteFrameHeader(buf, FrameType::kSendPacket,
                   static_cast<uint32_t>(payload_len));
  std::memcpy(buf + kFrameHeaderSize,
              dst_key.data(), kKeySize);
  if (data_len > 0) {
    std::memcpy(buf + kFrameHeaderSize + kKeySize,
                data, data_len);
  }

  int rc = WriteAll(c, buf, frame_len);
  if (heap) {
    delete[] buf;
  }
  if (rc < 0) {
    return MakeError(ClientError::IoFailed,
                     "write SendPacket failed");
  }
  return {};
}

auto ClientRecvFrame(DerpClient* c, FrameType* type,
                     uint8_t* payload, int* payload_len,
                     int buf_size)
    -> std::expected<void, Error<ClientError>> {
  uint8_t hdr[kFrameHeaderSize];
  if (ReadAll(c, hdr, kFrameHeaderSize) < 0) {
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
    if (ReadAll(c, payload,
                static_cast<int>(len)) < 0) {
      return MakeError(ClientError::IoFailed,
                       "read frame payload failed");
    }
  }
  *payload_len = static_cast<int>(len);
  return {};
}

void ClientClose(DerpClient* c) {
  if (c->ssl) {
    SSL_set_quiet_shutdown(c->ssl, 1);
    SSL_shutdown(c->ssl);
    SSL_free(c->ssl);
    c->ssl = nullptr;
  }
  if (c->ssl_ctx) {
    SSL_CTX_free(c->ssl_ctx);
    c->ssl_ctx = nullptr;
  }
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
