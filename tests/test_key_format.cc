/// @file test_key_format.cc
/// @brief Unit tests for rk_/ck_ key-prefix string format.

#include "hyper_derp/key_format.h"

#include <cstring>

#include <gtest/gtest.h>

namespace hyper_derp {

TEST(KeyFormatTest, RkRoundtrip) {
  Key key{};
  for (int i = 0; i < kKeySize; i++) key[i] = i;
  std::string s = KeyToRkString(key);
  EXPECT_EQ(static_cast<int>(s.size()), kPrefixedKeyLen);
  EXPECT_EQ(s.substr(0, 3), "rk_");

  Key parsed{};
  EXPECT_EQ(ParseKeyString(s, &parsed), KeyPrefix::kRelay);
  EXPECT_EQ(memcmp(parsed.data(), key.data(), kKeySize),
            0);
}

TEST(KeyFormatTest, CkRoundtrip) {
  Key key{};
  for (int i = 0; i < kKeySize; i++) {
    key[i] = static_cast<uint8_t>(0xAA ^ i);
  }
  std::string s = KeyToCkString(key);
  EXPECT_EQ(static_cast<int>(s.size()), kPrefixedKeyLen);
  EXPECT_EQ(s.substr(0, 3), "ck_");

  Key parsed{};
  EXPECT_EQ(ParseKeyString(s, &parsed),
            KeyPrefix::kClient);
  EXPECT_EQ(memcmp(parsed.data(), key.data(), kKeySize),
            0);
}

TEST(KeyFormatTest, RawHexAccepted) {
  Key key{};
  for (int i = 0; i < kKeySize; i++) key[i] = 0xCC;
  std::string s = KeyToRkString(key).substr(kKeyPrefixLen);
  EXPECT_EQ(static_cast<int>(s.size()), kKeySize * 2);

  Key parsed{};
  EXPECT_EQ(ParseKeyString(s, &parsed),
            KeyPrefix::kRawHex);
  EXPECT_EQ(memcmp(parsed.data(), key.data(), kKeySize),
            0);
}

TEST(KeyFormatTest, InvalidPrefixRejected) {
  Key parsed{};
  std::string bad =
      "xy_abcdef0123456789abcdef0123456789abcdef0123"
      "456789abcdef0123456789abcd";
  EXPECT_EQ(ParseKeyString(bad, &parsed),
            KeyPrefix::kInvalid);
}

TEST(KeyFormatTest, InvalidHexRejected) {
  Key parsed{};
  // Correct length, bad hex chars.
  std::string bad =
      "rk_ZZ2bcdef0123456789abcdef0123456789abcdef01"
      "23456789abcdef0123456789ab";
  EXPECT_EQ(ParseKeyString(bad, &parsed),
            KeyPrefix::kInvalid);
}

TEST(KeyFormatTest, WrongLengthRejected) {
  Key parsed{};
  EXPECT_EQ(ParseKeyString("rk_abcd", &parsed),
            KeyPrefix::kInvalid);
  EXPECT_EQ(ParseKeyString("", &parsed),
            KeyPrefix::kInvalid);
  EXPECT_EQ(ParseKeyString("abcdef", &parsed),
            KeyPrefix::kInvalid);
}

}  // namespace hyper_derp
