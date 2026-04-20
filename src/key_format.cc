/// @file key_format.cc
/// @brief Prefixed string format for HD keys.

#include "hyper_derp/key_format.h"

#include <cstring>

#include <sodium.h>

namespace hyper_derp {

namespace {

std::string FormatWithPrefix(const Key& key,
                             const char* prefix) {
  char hex[kKeySize * 2 + 1];
  sodium_bin2hex(hex, sizeof(hex),
                 key.data(), kKeySize);
  std::string out;
  out.reserve(kPrefixedKeyLen);
  out.append(prefix, kKeyPrefixLen);
  out.append(hex, kKeySize * 2);
  return out;
}

bool HexToKey(const char* hex, int hex_len, Key* out) {
  if (hex_len != kKeySize * 2) return false;
  size_t decoded = 0;
  if (sodium_hex2bin(out->data(), kKeySize,
                     hex, hex_len,
                     nullptr, &decoded, nullptr) != 0) {
    return false;
  }
  return decoded == static_cast<size_t>(kKeySize);
}

}  // namespace

std::string KeyToRkString(const Key& key) {
  return FormatWithPrefix(key, kKeyPrefixRelay);
}

std::string KeyToCkString(const Key& key) {
  return FormatWithPrefix(key, kKeyPrefixClient);
}

KeyPrefix ParseKeyString(std::string_view s, Key* out) {
  // Raw 64-hex form.
  if (s.size() == kKeySize * 2) {
    if (HexToKey(s.data(), static_cast<int>(s.size()),
                 out)) {
      return KeyPrefix::kRawHex;
    }
    return KeyPrefix::kInvalid;
  }
  // Prefixed form (3 + 64 = 67 chars).
  if (s.size() == kPrefixedKeyLen) {
    const char* body = s.data() + kKeyPrefixLen;
    int body_len = kKeySize * 2;
    if (std::memcmp(s.data(), kKeyPrefixRelay,
                    kKeyPrefixLen) == 0 &&
        HexToKey(body, body_len, out)) {
      return KeyPrefix::kRelay;
    }
    if (std::memcmp(s.data(), kKeyPrefixClient,
                    kKeyPrefixLen) == 0 &&
        HexToKey(body, body_len, out)) {
      return KeyPrefix::kClient;
    }
  }
  return KeyPrefix::kInvalid;
}

}  // namespace hyper_derp
