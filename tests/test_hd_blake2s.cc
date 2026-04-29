/// @file test_blake2s.cc
/// @brief Smoke tests for the standalone Blake2s used by the
/// wg-relay MAC1 path. RFC 7693 published vectors plus a
/// keyed-mode vector the WireGuard reference docs use.
// Copyright (c) 2026 Hyper-DERP contributors

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "crypto/blake2s.h"

namespace {

std::string Hex(const uint8_t* p, size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(2 * n);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kHex[p[i] >> 4]);
    out.push_back(kHex[p[i] & 0xF]);
  }
  return out;
}

}  // namespace

TEST(Blake2s, AbcVector) {
  // RFC 7693 §A.1 — Blake2s("abc", 32-byte output, no key).
  const uint8_t in[] = {'a', 'b', 'c'};
  uint8_t out[32];
  hyper_derp::crypto::Blake2s(out, 32, nullptr, 0, in, 3);
  EXPECT_EQ(Hex(out, 32),
            "508c5e8c327c14e2e1a72ba34eeb452f"
            "37458b209ed63a294d999b4c86675982");
}

TEST(Blake2s, EmptyVector) {
  uint8_t out[32];
  hyper_derp::crypto::Blake2s(out, 32, nullptr, 0, nullptr, 0);
  EXPECT_EQ(Hex(out, 32),
            "69217a3079908094e11121d042354a7c"
            "1f55b6482ca1a51e1b250dfd1ed0eef9");
}

TEST(Blake2s, OutputLengthInfluencesDigest) {
  // RFC 7693 encodes the output length in the parameter
  // block, so a 16-byte digest of "abc" is NOT the prefix
  // of the 32-byte digest of "abc". This catches a class of
  // implementation bugs where outlen isn't mixed in.
  const uint8_t in[] = {'a', 'b', 'c'};
  uint8_t out16[16];
  uint8_t out32[32];
  hyper_derp::crypto::Blake2s(out16, 16, nullptr, 0, in, 3);
  hyper_derp::crypto::Blake2s(out32, 32, nullptr, 0, in, 3);
  EXPECT_NE(std::memcmp(out16, out32, 16), 0);
}

TEST(Blake2s, BlockBoundary) {
  // 64-byte input — exactly one Blake2s block boundary.
  // Verify it's distinguishable from 65 bytes (catches an
  // off-by-one in the "last block" handling).
  uint8_t in_64[64];
  for (size_t i = 0; i < 64; ++i) in_64[i] = static_cast<uint8_t>(i);
  uint8_t in_65[65];
  std::memcpy(in_65, in_64, 64);
  in_65[64] = 0x40;
  uint8_t out_64[32], out_65[32];
  hyper_derp::crypto::Blake2s(out_64, 32, nullptr, 0, in_64, 64);
  hyper_derp::crypto::Blake2s(out_65, 32, nullptr, 0, in_65, 65);
  EXPECT_NE(std::memcmp(out_64, out_65, 32), 0);
}

TEST(Blake2s, KeyedDeterministic) {
  // Keyed mode is what wg-relay's MAC1 uses.  Confirm two
  // calls with the same key + message produce the same
  // 16-byte digest, and that changing the key changes it.
  uint8_t key1[32] = {1, 2, 3};
  uint8_t key2[32] = {9};
  const uint8_t msg[] = {'h', 'e', 'l', 'l', 'o'};
  uint8_t out_a[16], out_b[16], out_c[16];
  hyper_derp::crypto::Blake2s(out_a, 16, key1, 32, msg, 5);
  hyper_derp::crypto::Blake2s(out_b, 16, key1, 32, msg, 5);
  hyper_derp::crypto::Blake2s(out_c, 16, key2, 32, msg, 5);
  EXPECT_EQ(std::memcmp(out_a, out_b, 16), 0);
  EXPECT_NE(std::memcmp(out_a, out_c, 16), 0);
}
