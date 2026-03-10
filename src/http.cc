/// @file http.cc
/// @brief Minimal HTTP parser/builder for DERP upgrade.

#include "hyper_derp/http.h"

#include <cstdio>
#include <cstring>

#include "hyper_derp/protocol.h"

namespace hyper_derp {

// Scan for \r\n\r\n end-of-headers marker.
static const uint8_t* FindHeaderEnd(const uint8_t* buf,
                                    int len) {
  for (int i = 0; i + 3 < len; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' &&
        buf[i + 2] == '\r' && buf[i + 3] == '\n') {
      return buf + i;
    }
  }
  return nullptr;
}

// Case-insensitive prefix match.
static bool CiStartsWith(const char* str,
                          const char* prefix) {
  while (*prefix) {
    if ((*str | 0x20) != (*prefix | 0x20)) {
      return false;
    }
    str++;
    prefix++;
  }
  return true;
}

// Skip whitespace.
static const char* SkipWs(const char* p) {
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  return p;
}

int ParseHttpRequest(const uint8_t* buf, int len,
                     HttpRequest* req) {
  memset(req, 0, sizeof(*req));

  if (len > kMaxHttpRequestSize) {
    return -2;
  }

  const uint8_t* end = FindHeaderEnd(buf, len);
  if (!end) {
    return -1;  // Incomplete.
  }

  int header_len = static_cast<int>(end - buf) + 4;
  const char* p = reinterpret_cast<const char*>(buf);

  // Parse request line: METHOD SP PATH SP HTTP/x.x\r\n
  const char* method_end = strchr(p, ' ');
  if (!method_end) {
    return -2;
  }
  int method_len = static_cast<int>(method_end - p);
  if (method_len <= 0 ||
      method_len >= static_cast<int>(sizeof(req->method))) {
    return -2;
  }
  memcpy(req->method, p, method_len);
  req->method[method_len] = '\0';

  p = method_end + 1;
  const char* path_end = strchr(p, ' ');
  if (!path_end) {
    return -2;
  }
  int path_len = static_cast<int>(path_end - p);
  if (path_len <= 0 ||
      path_len >= static_cast<int>(sizeof(req->path))) {
    return -2;
  }
  memcpy(req->path, p, path_len);
  req->path[path_len] = '\0';

  // Skip to end of request line.
  const char* line_end = strstr(path_end, "\r\n");
  if (!line_end) {
    return -2;
  }
  p = line_end + 2;

  // Parse headers until empty line.
  const char* headers_end =
      reinterpret_cast<const char*>(end);
  while (p < headers_end) {
    const char* next = strstr(p, "\r\n");
    if (!next) {
      break;
    }

    if (CiStartsWith(p, "upgrade:")) {
      const char* val = SkipWs(p + 8);
      int val_len = static_cast<int>(next - val);
      if (val_len > 0 &&
          val_len <
              static_cast<int>(sizeof(req->upgrade))) {
        memcpy(req->upgrade, val, val_len);
        req->upgrade[val_len] = '\0';
      }
    } else if (CiStartsWith(p, "connection:")) {
      const char* val = SkipWs(p + 11);
      if (CiStartsWith(val, "upgrade")) {
        req->has_upgrade = true;
      }
    } else if (CiStartsWith(p, "derp-fast-start:")) {
      const char* val = SkipWs(p + 16);
      if (*val == '1') {
        req->fast_start = true;
      }
    }

    p = next + 2;
  }

  return header_len;
}

int WriteUpgradeResponse(uint8_t* buf, int buf_size,
                         const char* server_pub_key_hex) {
  int n = snprintf(
      reinterpret_cast<char*>(buf), buf_size,
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: DERP\r\n"
      "Connection: Upgrade\r\n"
      "Derp-Version: %d\r\n"
      "Derp-Public-Key: %s\r\n\r\n",
      kProtocolVersion, server_pub_key_hex);
  if (n < 0 || n >= buf_size) {
    return -1;
  }
  return n;
}

int WriteProbeResponse(uint8_t* buf, int buf_size) {
  int n = snprintf(
      reinterpret_cast<char*>(buf), buf_size,
      "HTTP/1.1 200 OK\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Content-Length: 0\r\n\r\n");
  if (n < 0 || n >= buf_size) {
    return -1;
  }
  return n;
}

int WriteNoContentResponse(uint8_t* buf, int buf_size,
                           const char* challenge) {
  if (challenge && challenge[0]) {
    // Validate challenge chars.
    int clen = static_cast<int>(strlen(challenge));
    if (clen > 64) {
      challenge = nullptr;
    } else {
      for (int i = 0; i < clen; i++) {
        char c = challenge[i];
        bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_' || c == ':';
        if (!ok) {
          challenge = nullptr;
          break;
        }
      }
    }
  }

  int n;
  if (challenge && challenge[0]) {
    n = snprintf(
        reinterpret_cast<char*>(buf), buf_size,
        "HTTP/1.1 204 No Content\r\n"
        "X-Tailscale-Response: response %s\r\n"
        "Cache-Control: no-cache, no-store, "
        "must-revalidate, no-transform, max-age=0\r\n"
        "\r\n",
        challenge);
  } else {
    n = snprintf(
        reinterpret_cast<char*>(buf), buf_size,
        "HTTP/1.1 204 No Content\r\n"
        "Cache-Control: no-cache, no-store, "
        "must-revalidate, no-transform, max-age=0\r\n"
        "\r\n");
  }
  if (n < 0 || n >= buf_size) {
    return -1;
  }
  return n;
}

int WriteErrorResponse(uint8_t* buf, int buf_size,
                       int status_code,
                       const char* message) {
  const char* reason = "Error";
  switch (status_code) {
    case 400:
      reason = "Bad Request";
      break;
    case 404:
      reason = "Not Found";
      break;
    case 405:
      reason = "Method Not Allowed";
      break;
    case 426:
      reason = "Upgrade Required";
      break;
    case 500:
      reason = "Internal Server Error";
      break;
  }
  int msg_len = static_cast<int>(strlen(message));
  int n = snprintf(
      reinterpret_cast<char*>(buf), buf_size,
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: %d\r\n\r\n"
      "%s",
      status_code, reason, msg_len, message);
  if (n < 0 || n >= buf_size) {
    return -1;
  }
  return n;
}

}  // namespace hyper_derp
