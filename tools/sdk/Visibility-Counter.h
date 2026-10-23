// Cumulative ZPD report semantics adapted from Xenia Canary.
// Copyright 2026 Ben Vanik. All rights reserved. Modified 2026-10-18.
// SPDX-License-Identifier: BSD-3-Clause
// The complete notice is in licenses/rexglue-xenia-bsd.txt.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
namespace fm4::visibility {
using Snapshot = std::array<uint32_t, 8>;
static_assert(sizeof(Snapshot) == 32);
inline Snapshot Next(uint32_t& counter, uint32_t increment) {
  counter += increment;  // Native counters and guest subtraction wrap at 32 bits.
  const uint32_t b = counter >> 1;
  const uint32_t a = counter - b;
  return {a, b, 0, 0, a, b, 0, 0};
}
inline void Publish(void* destination, const Snapshot& report) {
  // Total and ZFail must be ready before the guest sees the ZPass/Stencil half.
  std::memcpy(destination, report.data(), 16);
  std::atomic_thread_fence(std::memory_order_release);
  std::memcpy(static_cast<uint8_t*>(destination) + 16, report.data() + 4, 16);
}
}  // namespace fm4::visibility
