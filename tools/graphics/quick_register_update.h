// Batched constant ingestion. GPL-3.0-only.
#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__)
#include <tmmintrin.h>
#endif

namespace fm4::registers {
inline bool CopyConstantLayoutIfChanged(uint64_t* destination, const uint64_t* source) {
#if defined(_M_X64) || defined(__SSE2__)
  auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
  auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + 2));
  auto difference = _mm_or_si128(
      _mm_xor_si128(a, _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination))),
      _mm_xor_si128(b, _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + 2))));
  if (_mm_movemask_epi8(_mm_cmpeq_epi32(difference, _mm_setzero_si128())) == 0xFFFF) return false;
  _mm_storeu_si128(reinterpret_cast<__m128i*>(destination), a);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + 2), b);
#else
  if (!std::memcmp(destination, source, 32)) return false;
  std::memcpy(destination, source, 32);
#endif
  return true;
}
// Process a float4 as one dependency. Once a stage is dirty, later changes
// still receive their values without repeating its upload-usage lookup.
inline uint32_t CopySwappedConstantBlocks(uint32_t* destination, const uint32_t* source,
                                         size_t count, uint32_t first_word,
                                         const uint64_t* vertex, const uint64_t* pixel) {
  uint32_t changed = 0;
  auto notify = [&](uint32_t word) {
    uint32_t constant = word >> 2;
    uint32_t stage = constant < 256 ? 1 : 2;
    const uint64_t* map = constant < 256 ? vertex : pixel;
    uint32_t local = constant & 255;
    if (!(changed & stage) && map &&
        (map[local >> 6] & (uint64_t(1) << (local & 63)))) changed |= stage;
  };
  size_t i = 0;
  // Align to guest float4 boundaries, independently of host pointer alignment.
  for (; i < count && ((first_word + i) & 3); ++i) {
    uint32_t next = std::endian::native == std::endian::little
                        ? std::byteswap(source[i]) : source[i];
    if (destination[i] != next) { notify(first_word + uint32_t(i)); destination[i] = next; }
  }
#if defined(_M_X64) || defined(__SSE2__)
  for (; i + 4 <= count; i += 4) {
    auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i));
    if constexpr (std::endian::native == std::endian::little)
      value = _mm_shuffle_epi8(value, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4,
                                                  11, 10, 9, 8, 15, 14, 13, 12));
    auto old = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + i));
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(old, value)) == 0xFFFF) continue;
    notify(first_word + uint32_t(i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), value);
  }
#endif
  for (; i < count; ++i) {
    uint32_t next = std::endian::native == std::endian::little
                        ? std::byteswap(source[i]) : source[i];
    if (destination[i] != next) { notify(first_word + uint32_t(i)); destination[i] = next; }
  }
  return changed;
}

// Draw-state words have no write side effects. Aggregate the dependency domains
// once, excluding changes to the ignored high half of the draw initiator.
inline uint32_t CopySwappedStateDomains(uint32_t* destination, const uint32_t* source,
                                        size_t count, const uint8_t* domains,
                                        size_t masked_word) {
  uint32_t dirty = 0;
  size_t i = 0;
#if defined(_M_X64) || defined(__SSE2__)
  for (; i + 4 <= count; i += 4) {
    auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i));
    if constexpr (std::endian::native == std::endian::little)
      value = _mm_shuffle_epi8(value, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4,
                                                  11, 10, 9, 8, 15, 14, 13, 12));
    auto old = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + i));
    uint32_t changed = uint32_t(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(old, value)))) ^ 15;
    if (!changed) continue;
    while (changed) {
      size_t word = i + std::countr_zero(changed);
      changed &= changed - 1;
      if (!domains[word] || (dirty & domains[word]) == domains[word]) continue;
      if (word == masked_word &&
          !((destination[word] ^ std::byteswap(source[word])) & 0xFFFF)) continue;
      dirty |= domains[word];
    }
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), value);
  }
#endif
  for (; i < count; ++i) {
    uint32_t next = std::endian::native == std::endian::little
                        ? std::byteswap(source[i]) : source[i];
    if (destination[i] != next) {
      if (i != masked_word || ((destination[i] ^ next) & 0xFFFF)) dirty |= domains[i];
      destination[i] = next;
    }
  }
  return dirty;
}
}  // namespace fm4::registers
