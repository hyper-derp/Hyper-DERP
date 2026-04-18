/// @file hd_client.cc
/// @brief Client-side HD protocol implementation.

#include "hyper_derp/hd_client.h"

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
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/hd_protocol.h"

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

static int ReadAll(HdClient* c, uint8_t* buf, int n) {
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

static int WriteAll(HdClient* c, const uint8_t* buf,
                    int n) {
  if (c->ssl) return WriteAllSsl(c->ssl, buf, n);
  return WriteAllFd(c->fd, buf, n);
}

auto HdClientInit(HdClient* c)
    -> std::expected<void, Error<HdClientError>> {
  *c = {};
  if (sodium_init() < 0) {
    return MakeError(HdClientError::SodiumInitFailed,
                     "sodium_init failed");
  }
  crypto_box_keypair(c->public_key.data(),
                     c->private_key.data());
  return {};
}

void HdClientInitWithKeys(HdClient* c,
                          const uint8_t* pub,
                          const uint8_t* priv,
                          const Key& relay_key) {
  *c = {};
  c->public_key = ToKey(pub);
  c->private_key = ToKey(priv);
  c->relay_key = relay_key;
}

auto HdClientConnect(HdClient* c, const char* host,
                     uint16_t port)
    -> std::expected<void, Error<HdClientError>> {
  c->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0) {
    return MakeError(HdClientError::SocketFailed,
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
    return MakeError(HdClientError::BadAddress, host);
  }

  if (connect(c->fd,
              reinterpret_cast<sockaddr*>(&addr),
              sizeof(addr)) < 0) {
    close(c->fd);
    c->fd = -1;
    return MakeError(HdClientError::ConnectFailed,
                     strerror(errno));
  }

  c->host = host;
  c->port = port;
  c->connected = true;
  return {};
}

auto HdClientTlsConnect(HdClient* c)
    -> std::expected<void, Error<HdClientError>> {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    return MakeError(HdClientError::IoFailed,
                     "SSL_CTX_new failed");
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
  SSL_CTX_set_num_tickets(ctx, 0);

  SSL* ssl = SSL_new(ctx);
  if (!ssl) {
    SSL_CTX_free(ctx);
    return MakeError(HdClientError::IoFailed,
                     "SSL_new failed");
  }

  SSL_set_fd(ssl, c->fd);
  const char* sni = c->host.c_str();
  if (!c->host.empty() &&
      c->host[0] >= '0' && c->host[0] <= '9') {
    sni = "derp.tailscale.com";
  }
  SSL_set_tlsext_host_name(ssl, sni);
  int ret = SSL_connect(ssl);
  if (ret != 1) {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return MakeError(HdClientError::IoFailed,
                     std::string("SSL_connect: ") + buf);
  }

  c->ssl = ssl;
  c->ssl_ctx = ctx;
  return {};
}

auto HdClientUpgrade(HdClient* c)
    -> std::expected<void, Error<HdClientError>> {
  char req[256];
  auto r = std::format_to_n(req, sizeof(req) - 1,
      "GET /hd HTTP/1.1\r\n"
      "Host: {}:{}\r\n"
      "Upgrade: HD\r\n"
      "Connection: Upgrade\r\n"
      "\r\n",
      c->host, c->port);
  *r.out = '\0';
  int req_len = static_cast<int>(r.size);
  if (WriteAll(c,
               reinterpret_cast<const uint8_t*>(req),
               req_len) < 0) {
    return MakeError(HdClientError::IoFailed,
                     "write upgrade request failed");
  }

  // Read HTTP response one byte at a time to avoid
  // consuming post-header data (Enroll response may
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
      return MakeError(HdClientError::IoFailed,
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
    return MakeError(HdClientError::UpgradeFailed,
                     "no 101 in response");
  }
  return {};
}

auto HdClientEnroll(HdClient* c)
    -> std::expected<void, Error<HdClientError>> {
  // Compute HMAC over client public key using relay key.
  uint8_t hmac[kHdHmacSize];
  crypto_auth(hmac, c->public_key.data(), kKeySize,
              c->relay_key.data());

  // Build and send the Enroll frame.
  uint8_t enroll_buf[kHdFrameHeaderSize + kKeySize +
                     kHdHmacSize];
  int frame_len = HdBuildEnroll(
      enroll_buf, c->public_key, hmac, kHdHmacSize);
  if (WriteAll(c, enroll_buf, frame_len) < 0) {
    return MakeError(HdClientError::IoFailed,
                     "write Enroll frame failed");
  }

  // Read response header (4 bytes).
  uint8_t hdr[kHdFrameHeaderSize];
  if (ReadAll(c, hdr, kHdFrameHeaderSize) < 0) {
    return MakeError(HdClientError::IoFailed,
                     "read enrollment response failed");
  }

  HdFrameType type = HdReadFrameType(hdr);
  uint32_t plen = HdReadPayloadLen(hdr);

  if (!HdIsValidPayloadLen(plen)) {
    return MakeError(HdClientError::BadPayloadLength,
                     "enrollment response payload "
                     "too large");
  }

  // Read payload (drain it even for Denied).
  uint8_t payload[kKeySize + 256];
  if (plen > sizeof(payload)) {
    return MakeError(HdClientError::BufferOverflow,
                     "enrollment response exceeds "
                     "buffer");
  }
  if (plen > 0) {
    if (ReadAll(c, payload,
                static_cast<int>(plen)) < 0) {
      return MakeError(HdClientError::IoFailed,
                       "read enrollment payload failed");
    }
  }

  if (type == HdFrameType::kApproved) {
    c->approved = true;
    return {};
  }

  if (type == HdFrameType::kDenied) {
    return MakeError(HdClientError::EnrollmentDenied,
                     "enrollment denied by relay");
  }

  return MakeError(HdClientError::UnexpectedFrame,
                   "expected Approved or Denied frame");
}

auto HdClientEnrollAsRelay(HdClient* c,
                           uint16_t relay_id)
    -> std::expected<void, Error<HdClientError>> {
  // Compute HMAC over client public key using relay key.
  uint8_t hmac[kHdHmacSize];
  crypto_auth(hmac, c->public_key.data(), kKeySize,
              c->relay_key.data());

  // Build and send relay Enroll frame.
  uint8_t enroll_buf[kHdFrameHeaderSize + kKeySize +
                     kHdHmacSize + kHdRelayExtSize];
  int frame_len = HdBuildRelayEnroll(
      enroll_buf, c->public_key, hmac, kHdHmacSize,
      relay_id);
  if (WriteAll(c, enroll_buf, frame_len) < 0) {
    return MakeError(HdClientError::IoFailed,
                     "write relay Enroll frame failed");
  }

  // Read response header.
  uint8_t hdr[kHdFrameHeaderSize];
  if (ReadAll(c, hdr, kHdFrameHeaderSize) < 0) {
    return MakeError(HdClientError::IoFailed,
                     "read relay enrollment response "
                     "failed");
  }

  HdFrameType type = HdReadFrameType(hdr);
  uint32_t plen = HdReadPayloadLen(hdr);

  if (!HdIsValidPayloadLen(plen)) {
    return MakeError(HdClientError::BadPayloadLength,
                     "relay enrollment response payload "
                     "too large");
  }

  // Read payload.
  uint8_t payload[kKeySize + 256];
  if (plen > sizeof(payload)) {
    return MakeError(HdClientError::BufferOverflow,
                     "relay enrollment response exceeds "
                     "buffer");
  }
  if (plen > 0) {
    if (ReadAll(c, payload,
                static_cast<int>(plen)) < 0) {
      return MakeError(HdClientError::IoFailed,
                       "read relay enrollment payload "
                       "failed");
    }
  }

  if (type == HdFrameType::kApproved) {
    c->approved = true;
    return {};
  }

  if (type == HdFrameType::kDenied) {
    return MakeError(HdClientError::EnrollmentDenied,
                     "relay enrollment denied");
  }

  return MakeError(HdClientError::UnexpectedFrame,
                   "expected Approved or Denied frame");
}

auto HdClientSendData(HdClient* c,
                      const uint8_t* data, int len)
    -> std::expected<void, Error<HdClientError>> {
  if (len > kHdMaxFramePayload) {
    return MakeError(HdClientError::BufferOverflow,
                     "data exceeds max frame payload");
  }

  // Single write produces one TLS record under kTLS.
  int frame_len = kHdFrameHeaderSize + len;
  uint8_t stack[kHdFrameHeaderSize + 2048];
  uint8_t* buf = stack;
  bool heap = false;
  if (frame_len > static_cast<int>(sizeof(stack))) {
    buf = new uint8_t[frame_len];
    heap = true;
  }

  HdBuildDataHeader(buf, len);
  if (len > 0) {
    std::memcpy(buf + kHdFrameHeaderSize, data, len);
  }

  int rc = WriteAll(c, buf, frame_len);
  if (heap) {
    delete[] buf;
  }
  if (rc < 0) {
    return MakeError(HdClientError::IoFailed,
                     "write Data frame failed");
  }
  return {};
}

auto HdClientSendMeshData(HdClient* c,
                          uint16_t dst_peer_id,
                          const uint8_t* data, int len)
    -> std::expected<void, Error<HdClientError>> {
  int hdr_len = kHdFrameHeaderSize + kHdMeshDstSize;
  int total = hdr_len + len;
  if (total > kHdFrameHeaderSize + kHdMaxFramePayload) {
    return MakeError(HdClientError::BufferOverflow,
                     "MeshData frame too large");
  }

  // Single write for kTLS record coalescing.
  uint8_t stack[kHdFrameHeaderSize + kHdMeshDstSize +
                2048];
  uint8_t* buf = stack;
  bool heap = false;
  if (total > static_cast<int>(sizeof(stack))) {
    buf = new uint8_t[total];
    heap = true;
  }

  HdBuildMeshDataHeader(buf, dst_peer_id, len);
  if (len > 0) {
    std::memcpy(buf + hdr_len, data, len);
  }

  int rc = WriteAll(c, buf, total);
  if (heap) {
    delete[] buf;
  }
  if (rc < 0) {
    return MakeError(HdClientError::IoFailed,
                     "write MeshData frame failed");
  }
  return {};
}

auto HdClientRecvFrame(HdClient* c,
                       HdFrameType* type,
                       uint8_t* payload,
                       int* payload_len,
                       int buf_size)
    -> std::expected<void, Error<HdClientError>> {
  // Lazy-allocate recv buffer.
  if (!c->recv_buf) {
    c->recv_buf = new uint8_t[HdClient::kRecvBufSize];
    c->recv_len = 0;
    c->recv_pos = 0;
  }

  // Try to parse a frame from buffered data first.
  for (;;) {
    int avail = c->recv_len - c->recv_pos;

    // Need at least the header.
    if (avail >= kHdFrameHeaderSize) {
      uint8_t* hdr = c->recv_buf + c->recv_pos;
      *type = HdReadFrameType(hdr);
      uint32_t plen = HdReadPayloadLen(hdr);

      if (!HdIsValidPayloadLen(plen)) {
        return MakeError(HdClientError::BadPayloadLength,
                         "invalid frame payload length");
      }

      int frame_len =
          kHdFrameHeaderSize + static_cast<int>(plen);

      // Complete frame in buffer?
      if (avail >= frame_len) {
        if (static_cast<int>(plen) > buf_size) {
          return MakeError(
              HdClientError::BufferOverflow,
              "frame payload exceeds buffer");
        }
        if (plen > 0) {
          memcpy(payload,
                 hdr + kHdFrameHeaderSize, plen);
        }
        *payload_len = static_cast<int>(plen);
        c->recv_pos += frame_len;
        return {};
      }
    }

    // Not enough data. Compact buffer and read more.
    if (c->recv_pos > 0) {
      avail = c->recv_len - c->recv_pos;
      if (avail > 0) {
        memmove(c->recv_buf,
                c->recv_buf + c->recv_pos, avail);
      }
      c->recv_len = avail;
      c->recv_pos = 0;
    }

    // Read a large chunk.
    int space = HdClient::kRecvBufSize - c->recv_len;
    if (space <= 0) {
      return MakeError(HdClientError::BufferOverflow,
                       "recv buffer full");
    }
    int n;
    if (c->ssl) {
      n = SSL_read(c->ssl,
                   c->recv_buf + c->recv_len, space);
    } else {
      n = read(c->fd,
               c->recv_buf + c->recv_len, space);
    }
    if (n <= 0) {
      return MakeError(HdClientError::IoFailed,
                       "recv failed");
    }
    c->recv_len += n;
  }
}

auto HdClientReconnect(HdClient* c)
    -> std::expected<void, Error<HdClientError>> {
  // Close old connection.
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
  c->approved = false;

  // Reconnect.
  auto conn = HdClientConnect(c, c->host.c_str(),
                               c->port);
  if (!conn) return conn;

  // Re-upgrade.
  auto up = HdClientUpgrade(c);
  if (!up) return std::unexpected(
      Error<HdClientError>{
          HdClientError::UpgradeFailed,
          up.error().message});

  // Re-enroll with same keys.
  auto enroll = HdClientEnroll(c);
  if (!enroll) return std::unexpected(
      Error<HdClientError>{
          HdClientError::EnrollmentDenied,
          enroll.error().message});

  return {};
}

auto HdClientSendPing(HdClient* c)
    -> std::expected<void, Error<HdClientError>> {
  uint8_t frame[kHdFrameHeaderSize + kHdPingDataSize];
  // Generate 8 bytes of random ping data.
  randombytes_buf(frame + kHdFrameHeaderSize,
                  kHdPingDataSize);
  HdBuildPing(frame, frame + kHdFrameHeaderSize);
  if (WriteAll(c, frame, sizeof(frame)) < 0) {
    return MakeError(HdClientError::IoFailed,
                     "write Ping frame failed");
  }
  return {};
}

void HdClientClose(HdClient* c) {
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
  c->approved = false;
  delete[] c->recv_buf;
  c->recv_buf = nullptr;
  c->recv_len = 0;
  c->recv_pos = 0;
}

auto HdClientSetTimeout(HdClient* c, int ms)
    -> std::expected<void, Error<HdClientError>> {
  timeval tv{
      .tv_sec = ms / 1000,
      .tv_usec = (ms % 1000) * 1000};
  if (setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO,
                 &tv, sizeof(tv)) < 0) {
    return MakeError(HdClientError::IoFailed,
                     "setsockopt SO_RCVTIMEO failed");
  }
  return {};
}

}  // namespace hyper_derp
