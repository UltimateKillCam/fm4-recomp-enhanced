// Native PM4 range ingestion. GPL-3.0-only.
#include "pm4_native.h"
#include <bit>
#include <immintrin.h>

namespace fm4::pm4 {
namespace {
inline __m256i LoadSwapped(const uint32_t* source) {
  const auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source));
  return _mm256_shuffle_epi8(bytes, _mm256_setr_epi8(
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
inline uint32_t Changed(__m256i value, const uint32_t* destination) {
  const auto old = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destination));
  return uint32_t(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(old, value)))) ^ 255;
}
}  // namespace

bool CopySwapped(uint32_t* destination, const uint32_t* source, size_t count) {
  bool changed = false;
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    const auto value = LoadSwapped(source + i);
    if (!Changed(value, destination + i)) continue;
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), value);
    changed = true;
  }
  for (; i < count; ++i) {
    const uint32_t value = std::byteswap(source[i]);
    if (destination[i] != value) { destination[i] = value; changed = true; }
  }
  return changed;
}

uint32_t CopyConstants(uint32_t* destination, const uint32_t* source, size_t count,
                       uint32_t first_word, const uint64_t* vertex, const uint64_t* pixel) {
  uint32_t dirty = 0;
  auto notify = [&](uint32_t word) {
    const uint32_t constant = word >> 2;
    const uint32_t stage = constant < 256 ? 1 : 2;
    const uint64_t* map = constant < 256 ? vertex : pixel;
    const uint32_t local = constant & 255;
    if (!(dirty & stage) && map && (map[local >> 6] & (uint64_t(1) << (local & 63))))
      dirty |= stage;
  };
  size_t i = 0;
  // Align the guest float4 boundary so each half of a vector has one dependency.
  for (; i < count && ((first_word + i) & 3); ++i) {
    const uint32_t value = std::byteswap(source[i]);
    if (destination[i] != value) { notify(first_word + uint32_t(i)); destination[i] = value; }
  }
  for (; i + 8 <= count; i += 8) {
    const auto value = LoadSwapped(source + i);
    const uint32_t changed = Changed(value, destination + i);
    if (!changed) continue;
    if (changed & 15) notify(first_word + uint32_t(i));
    if (changed & 240) notify(first_word + uint32_t(i) + 4);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), value);
  }
  for (; i < count; ++i) {
    const uint32_t value = std::byteswap(source[i]);
    if (destination[i] != value) { notify(first_word + uint32_t(i)); destination[i] = value; }
  }
  return dirty;
}

uint32_t CopyState(uint32_t* destination, const uint32_t* source, size_t count,
                   const uint8_t* domains, size_t masked_word) {
  uint32_t dirty = 0;
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    const auto value = LoadSwapped(source + i);
    uint32_t changed = Changed(value, destination + i);
    if (!changed) continue;
    // Extract from the loaded vector; never read guest memory a second time.
    alignas(32) uint32_t words[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(words), value);
    while (changed) {
      const size_t lane = std::countr_zero(changed);
      const size_t word = i + lane;
      changed &= changed - 1;
      if (word != masked_word || ((destination[word] ^ words[lane]) & 0xFFFF))
        dirty |= domains[word];
    }
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), value);
  }
  for (; i < count; ++i) {
    const uint32_t value = std::byteswap(source[i]);
    if (destination[i] != value) {
      if (i != masked_word || ((destination[i] ^ value) & 0xFFFF)) dirty |= domains[i];
      destination[i] = value;
    }
  }
  return dirty;
}
}  // namespace fm4::pm4
