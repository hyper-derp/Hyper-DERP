/// @file key_format.h
/// @brief Prefixed string format for HD keys.
///
/// The HD protocol design doc uses short-prefix strings to
/// distinguish key kinds visually and at parse time:
///   `rk_<64 hex>`  relay key (enrollment shared secret)
///   `ck_<64 hex>`  client key (Curve25519 public key)
///
/// Wire formats continue to carry raw 32-byte keys; these
/// helpers exist for config files, CLI, REST responses,
/// and logs.

#ifndef INCLUDE_HYPER_DERP_KEY_FORMAT_H_
#define INCLUDE_HYPER_DERP_KEY_FORMAT_H_

#include <string>
#include <string_view>

#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Prefix for relay keys ("rk_").
inline constexpr const char kKeyPrefixRelay[] = "rk_";

/// Prefix for client keys ("ck_").
inline constexpr const char kKeyPrefixClient[] = "ck_";

/// Length of the prefix (3 bytes).
inline constexpr int kKeyPrefixLen = 3;

/// Total length of a prefixed key string (3 + 64 hex).
inline constexpr int kPrefixedKeyLen =
    kKeyPrefixLen + kKeySize * 2;

/// @brief Formats a key as "rk_<64 hex>".
/// @param key The 32-byte key.
/// @returns A std::string of length kPrefixedKeyLen.
std::string KeyToRkString(const Key& key);

/// @brief Formats a key as "ck_<64 hex>".
/// @param key The 32-byte key.
/// @returns A std::string of length kPrefixedKeyLen.
std::string KeyToCkString(const Key& key);

/// @brief Prefix returned by ParseKeyString.
enum class KeyPrefix : uint8_t {
  /// Input was invalid.
  kInvalid = 0,
  /// Input was raw 64-char hex with no prefix.
  kRawHex = 1,
  /// Input was "rk_..." (relay key).
  kRelay = 2,
  /// Input was "ck_..." (client key).
  kClient = 3,
};

/// @brief Parses "rk_...", "ck_...", or raw 64-hex into a
///   Key.
/// @param s Input string.
/// @param out Output 32-byte key (written on success).
/// @returns The detected prefix kind, or kInvalid if the
///   input was malformed.
KeyPrefix ParseKeyString(std::string_view s, Key* out);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_KEY_FORMAT_H_
