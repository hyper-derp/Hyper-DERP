/// @file blake2s.h
/// @brief Blake2s hash used by WireGuard's MAC1 derivation.
///
/// Self-contained because libsodium ships Blake2b, not the
/// 32-bit Blake2s variant WireGuard's protocol uses. This is a
/// straight port of the RFC 7693 reference, scoped to what
/// wg-relay needs (single-shot, ≤32 byte output, ≤32 byte
/// key, no streaming API).
// Copyright (c) 2026 Hyper-DERP contributors

#pragma once

#include <cstddef>
#include <cstdint>

namespace hyper_derp::crypto {

/// One-shot keyed Blake2s.
///   * out_len: 1..32
///   * key_len: 0..32 (0 = unkeyed)
/// Reference: RFC 7693 §3.2.
void Blake2s(uint8_t* out, size_t out_len, const uint8_t* key,
             size_t key_len, const uint8_t* in, size_t in_len);

}  // namespace hyper_derp::crypto
