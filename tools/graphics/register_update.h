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
// Notify only changed words before storing them, preserving masked-state comparisons.
template <typename Notify>
inline void CopySwappedAndNotify(uint32_t* destination, const uint32_t* source, size_t count,
                                 Notify notify) {
  size_t i = 0;
#if !defined(FM4_REGISTER_UPDATE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
  for (; i + 4 <= count; i += 4) {
    auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i));
    if constexpr (std::endian::native == std::endian::little) {
      value = _mm_or_si128(_mm_slli_epi16(value, 8), _mm_srli_epi16(value, 8));
      value = _mm_or_si128(_mm_slli_epi32(value, 16), _mm_srli_epi32(value, 16));
    }
    const auto previous = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + i));
    uint32_t changed =
        uint32_t(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(previous, value)))) ^ 0xF;
    if (!changed)
      continue;
    while (changed) {
      size_t word = i + std::countr_zero(changed);
      changed &= changed - 1;
      uint32_t next =
          std::endian::native == std::endian::little ? std::byteswap(source[word]) : source[word];
      notify(word, destination[word], next);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), value);
  }
#endif
  for (; i < count; ++i) {
    uint32_t value =
        std::endian::native == std::endian::little ? std::byteswap(source[i]) : source[i];
    if (destination[i] != value) {
      notify(i, destination[i], value);
      destination[i] = value;
    }
  }
}
// Return bit 0/1 only when a changed word belongs to a constant read by a
// live vertex/pixel upload. Null maps denote uploads that are already dirty.
inline uint32_t CopySwappedConstantUsage(uint32_t* destination, const uint32_t* source,
                                        size_t count, uint32_t first_word,
                                        const uint64_t* vertex, const uint64_t* pixel) {
  uint32_t changed_stages = 0;
  CopySwappedAndNotify(destination, source, count,
      [&](size_t offset, uint32_t, uint32_t) {
        uint32_t constant = (first_word + uint32_t(offset)) >> 2;
        const uint64_t* usage = constant < 256 ? vertex : pixel;
        uint32_t local = constant & 255;
        if (usage && (usage[local >> 6] & (uint64_t(1) << (local & 63))))
          changed_stages |= constant < 256 ? 1u : 2u;
      });
  return changed_stages;
}
}  // namespace fm4::registers
