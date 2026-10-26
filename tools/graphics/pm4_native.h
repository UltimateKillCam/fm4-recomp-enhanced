// Native PM4 range ingestion. GPL-3.0-only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <intrin.h>

namespace fm4::pm4 {
inline bool HasAvx2() {
  static const bool available = [] {
    int cpu[4];
    __cpuid(cpu, 0);
    if (cpu[0] < 7) return false;
    __cpuid(cpu, 1);
    if ((cpu[2] & 0x18000000) != 0x18000000 || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(cpu, 7, 0);
    return (cpu[1] & (1 << 5)) != 0;
  }();
  return available;
}

bool CopySwapped(uint32_t* destination, const uint32_t* source, size_t count);
uint32_t CopyConstants(uint32_t* destination, const uint32_t* source, size_t count,
                       uint32_t first_word, const uint64_t* vertex, const uint64_t* pixel);
uint32_t CopyState(uint32_t* destination, const uint32_t* source, size_t count,
                   const uint8_t* domains, size_t masked_word);
}  // namespace fm4::pm4
