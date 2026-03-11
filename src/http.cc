/// @file http.cc
/// @brief Minimal HTTP parser/builder for DERP upgrade.

#include "hyper_derp/http.h"

#include <cstring>
#include <expected>
#include <format>
#include <string_view>

#include "hyper_derp/error.h"
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
static bool CiStartsWith(std::string_view str,
                          std::string_view prefix) {
  if (str.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); i++) {
    if ((str[i] | 0x20) != (prefix[i] | 0x20)) {
      return false;
    }
  }
  return true;
}

// Skip whitespace.
static std::string_view LTrim(std::string_view s) {
  while (!s.empty() &&
         (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  return s;
}

auto ParseHttpRequest(const uint8_t* buf, int len,
                      HttpRequest* req)
    -> std::expected<int, Error<HttpParseError>> {
  *req = {};

  if (len > kMaxHttpRequestSize) {
    return MakeError(HttpParseError::BadRequest,
                     "request exceeds max size");
  }

  const uint8_t* end = FindHeaderEnd(buf, len);
  if (!end) {
    return MakeError(HttpParseError::Incomplete,
                     "headers not yet complete");
  }

  int header_len = static_cast<int>(end - buf) + 4;
  std::string_view input(
      reinterpret_cast<const char*>(buf), header_len);

  // Parse request line: METHOD SP PATH SP HTTP/x.x\r\n
  auto sp1 = input.find(' ');
  if (sp1 == std::string_view::npos) {
    return MakeError(HttpParseError::BadRequest,
                     "no space after method");
  }
  req->method = std::string(input.substr(0, sp1));
  if (req->method.empty()) {
    return MakeError(HttpParseError::BadRequest,
                     "empty method");
  }

  auto rest = input.substr(sp1 + 1);
  auto sp2 = rest.find(' ');
  if (sp2 == std::string_view::npos) {
    return MakeError(HttpParseError::BadRequest,
                     "no space after path");
  }
  req->path = std::string(rest.substr(0, sp2));
  if (req->path.empty()) {
    return MakeError(HttpParseError::BadRequest,
                     "empty path");
  }

  // Skip to end of request line.
  auto crlf = input.find("\r\n");
  if (crlf == std::string_view::npos) {
    return MakeError(HttpParseError::BadRequest,
                     "no CRLF after request line");
  }
  auto headers = input.substr(crlf + 2);

  // Parse headers until empty line.
  while (!headers.empty()) {
    auto line_end = headers.find("\r\n");
    if (line_end == std::string_view::npos) {
      break;
    }
    auto line = headers.substr(0, line_end);
    headers = headers.substr(line_end + 2);

    if (CiStartsWith(line, "upgrade:")) {
      auto val = LTrim(line.substr(8));
      req->upgrade = std::string(val);
    } else if (CiStartsWith(line, "connection:")) {
      auto val = LTrim(line.substr(11));
      if (CiStartsWith(val, "upgrade")) {
        req->has_upgrade = true;
      }
    } else if (CiStartsWith(line, "derp-fast-start:")) {
      auto val = LTrim(line.substr(16));
      if (!val.empty() && val.front() == '1') {
        req->fast_start = true;
      }
    }
  }

  return header_len;
}

int WriteUpgradeResponse(uint8_t* buf, int buf_size,
                         const char* server_pub_key_hex) {
  auto r = std::format_to_n(
      reinterpret_cast<char*>(buf), buf_size - 1,
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: DERP\r\n"
      "Connection: Upgrade\r\n"
      "Derp-Version: {}\r\n"
      "Derp-Public-Key: {}\r\n\r\n",
      kProtocolVersion, server_pub_key_hex);
  int n = static_cast<int>(r.size);
  if (n < 0 || n >= buf_size) {
    return -1;
  }
  *r.out = '\0';
  return n;
}

int WriteProbeResponse(uint8_t* buf, int buf_size) {
  auto r = std::format_to_n(
      reinterpret_cast<char*>(buf), buf_size - 1,
      "HTTP/1.1 200 OK\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Content-Length: 0\r\n\r\n");
  int n = static_cast<int>(r.size);
  if (n < 0 || n >= buf_size) {
    return -1;
  }
  *r.out = '\0';
  return n;
}

int WriteNoContentResponse(uint8_t* buf, int buf_size,
                           const char* challenge) {
  if (challenge && challenge[0]) {
    // Validate challenge chars.
    std::string_view ch(challenge);
    if (ch.size() > 64) {
      challenge = nullptr;
    } else {
      for (char c : ch) {
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

  std::format_to_n_result<char*> r;
  auto* out = reinterpret_cast<char*>(buf);
  if (challenge && challenge[0]) {
    r = std::format_to_n(
        out, buf_size - 1,
        "HTTP/1.1 204 No Content\r\n"
        "X-Tailscale-Response: response {}\r\n"
        "Cache-Control: no-cache, no-store, "
        "must-revalidate, no-transform, max-age=0\r\n"
        "\r\n",
        challenge);
  } else {
    r = std::format_to_n(
        out, buf_size - 1,
        "HTTP/1.1 204 No Content\r\n"
        "Cache-Control: no-cache, no-store, "
        "must-revalidate, no-transform, max-age=0\r\n"
        "\r\n");
  }
  int n = static_cast<int>(r.size);
  if (n < 0 || n >= buf_size) {
    return -1;
  }
  *r.out = '\0';
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
  auto msg = std::string_view(message);
  auto r = std::format_to_n(
      reinterpret_cast<char*>(buf), buf_size - 1,
      "HTTP/1.1 {} {}\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: {}\r\n\r\n"
      "{}",
      status_code, reason, msg.size(), msg);
  int n = static_cast<int>(r.size);
  if (n < 0 || n >= buf_size) {
    return -1;
  }
  *r.out = '\0';
  return n;
}

}  // namespace hyper_derp
