/// @file blake2s.cc
/// @brief RFC 7693 Blake2s reference implementation, single-shot.
// Copyright (c) 2026 Hyper-DERP contributors

#include "blake2s.h"

#include <cstring>

namespace hyper_derp::crypto {
namespace {

constexpr uint32_t kIV[8] = {
    0x6A09E667UL, 0xBB67AE85UL, 0x3C6EF372UL, 0xA54FF53AUL,
    0x510E527FUL, 0x9B05688CUL, 0x1F83D9ABUL, 0x5BE0CD19UL,
};

constexpr uint8_t kSigma[10][16] = {
    {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15},
    {14, 10, 4,  8,  9,  15, 13, 6,  1,  12, 0,  2,  11, 7,  5,  3},
    {11, 8,  12, 0,  5,  2,  15, 13, 10, 14, 3,  6,  7,  1,  9,  4},
    {7,  9,  3,  1,  13, 12, 11, 14, 2,  6,  5,  10, 4,  0,  15, 8},
    {9,  0,  5,  7,  2,  4,  10, 15, 14, 1,  11, 12, 6,  8,  3,  13},
    {2,  12, 6,  10, 0,  11, 8,  3,  4,  13, 7,  5,  15, 14, 1,  9},
    {12, 5,  1,  15, 14, 13, 4,  10, 0,  7,  6,  3,  9,  2,  8,  11},
    {13, 11, 7,  14, 12, 1,  3,  9,  5,  0,  15, 4,  8,  6,  2,  10},
    {6,  15, 14, 9,  11, 3,  0,  8,  12, 2,  13, 7,  1,  4,  10, 5},
    {10, 2,  8,  4,  7,  6,  1,  5,  15, 11, 9,  14, 3,  12, 13, 0},
};

inline uint32_t Load32LE(const uint8_t* p) {
  return uint32_t{p[0]} | (uint32_t{p[1]} << 8) |
         (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

inline void Store32LE(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint32_t Rotr(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

inline void G(uint32_t v[16], int a, int b, int c, int d,
              uint32_t x, uint32_t y) {
  v[a] = v[a] + v[b] + x;
  v[d] = Rotr(v[d] ^ v[a], 16);
  v[c] = v[c] + v[d];
  v[b] = Rotr(v[b] ^ v[c], 12);
  v[a] = v[a] + v[b] + y;
  v[d] = Rotr(v[d] ^ v[a], 8);
  v[c] = v[c] + v[d];
  v[b] = Rotr(v[b] ^ v[c], 7);
}

void Compress(uint32_t h[8], const uint8_t block[64], uint64_t t,
              bool last) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = Load32LE(block + i * 4);
  }
  uint32_t v[16];
  for (int i = 0; i < 8; ++i) v[i] = h[i];
  for (int i = 0; i < 8; ++i) v[i + 8] = kIV[i];
  v[12] ^= static_cast<uint32_t>(t);
  v[13] ^= static_cast<uint32_t>(t >> 32);
  if (last) v[14] = ~v[14];
  for (int r = 0; r < 10; ++r) {
    const uint8_t* s = kSigma[r];
    G(v, 0, 4, 8,  12, m[s[0]],  m[s[1]]);
    G(v, 1, 5, 9,  13, m[s[2]],  m[s[3]]);
    G(v, 2, 6, 10, 14, m[s[4]],  m[s[5]]);
    G(v, 3, 7, 11, 15, m[s[6]],  m[s[7]]);
    G(v, 0, 5, 10, 15, m[s[8]],  m[s[9]]);
    G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
    G(v, 2, 7, 8,  13, m[s[12]], m[s[13]]);
    G(v, 3, 4, 9,  14, m[s[14]], m[s[15]]);
  }
  for (int i = 0; i < 8; ++i) h[i] ^= v[i] ^ v[i + 8];
}

}  // namespace

void Blake2s(uint8_t* out, size_t out_len, const uint8_t* key,
             size_t key_len, const uint8_t* in, size_t in_len) {
  uint32_t h[8];
  for (int i = 0; i < 8; ++i) h[i] = kIV[i];
  // Parameter block: digest_length || key_length || fanout || depth.
  h[0] ^= 0x01010000UL ^ (static_cast<uint32_t>(key_len) << 8) ^
          static_cast<uint32_t>(out_len);

  uint8_t block[64];
  uint64_t t = 0;
  size_t cursor = 0;
  bool finalized = false;

  // If keyed, the first block is the key padded with zeros and
  // counts as 64 bytes consumed.  When there's no message the
  // keyed block is also the only (and final) block.
  if (key_len > 0) {
    std::memset(block, 0, sizeof(block));
    std::memcpy(block, key, key_len);
    if (in_len == 0) {
      Compress(h, block, 64, true);
      finalized = true;
    } else {
      Compress(h, block, 64, false);
      t = 64;
    }
  }

  if (!finalized) {
    // Bulk: full 64-byte blocks except possibly the last.
    while (in_len - cursor > 64) {
      std::memcpy(block, in + cursor, 64);
      cursor += 64;
      t += 64;
      Compress(h, block, t, false);
    }
    // Final block (possibly short, possibly zero remaining).
    std::memset(block, 0, sizeof(block));
    size_t last = in_len - cursor;
    std::memcpy(block, in + cursor, last);
    t += last;
    Compress(h, block, t, true);
  }

  uint8_t buf[32];
  for (int i = 0; i < 8; ++i) Store32LE(buf + i * 4, h[i]);
  std::memcpy(out, buf, out_len);
}

}  // namespace hyper_derp::crypto
