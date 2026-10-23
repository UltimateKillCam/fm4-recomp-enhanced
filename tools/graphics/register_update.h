// Shader constant updates. Modified 2026-10-17. GPL-3.0-only.
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#if !defined(FM4_REGISTER_UPDATE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
#include <emmintrin.h>
#endif
namespace fm4::registers {
// Constant registers have no write side effects. Preserve every value while
// reporting whether a constant-buffer binding really needs invalidation.
inline bool CopySwappedIfChanged(uint32_t* destination, const uint32_t* source, size_t count) {
  bool changed = false;
  size_t i = 0;
#if !defined(FM4_REGISTER_UPDATE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
  for (; i + 4 <= count; i += 4) {
    auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i));
    if constexpr (std::endian::native == std::endian::little) {
      value = _mm_or_si128(_mm_slli_epi16(value, 8), _mm_srli_epi16(value, 8));
      value = _mm_or_si128(_mm_slli_epi32(value, 16), _mm_srli_epi32(value, 16));
    }
    const auto previous = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + i));
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(previous, value)) != 0xFFFF) {
      _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), value);
      changed = true;
    }
  }
#endif
  for (; i < count; ++i) {
    const auto value = std::endian::native == std::endian::little ? std::byteswap(source[i]) : source[i];
    if (destination[i] != value) { destination[i] = value; changed = true; }
  }
  return changed;
}
}  // namespace fm4::registers
