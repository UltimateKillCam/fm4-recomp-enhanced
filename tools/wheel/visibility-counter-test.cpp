// GPL-3.0-only. Modified 2026-10-18.
#include "../sdk/Visibility-Counter.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
int main() try {
  size_t checked = 0;
  for (uint32_t seed : {0u, 17u, 0xfffffff0u, 0x7fffffffu}) {
    for (uint32_t increment : {0u, 1u, 127u, 1000000u, 0x7fffffffu}) {
      uint32_t state = seed;
      auto previous = fm4::visibility::Next(state, 0);
      for (unsigned step = 0; step < 1000; ++step) {
        const auto current = fm4::visibility::Next(state, increment);
        const uint32_t begin = previous[4] + previous[5];
        const uint32_t end = current[4] + current[5];
        if (uint32_t(end - begin) != increment || end != state ||
            current[0] + current[1] != end || current[0] != current[4] ||
            current[1] != current[5] || current[4] - current[5] > 1 ||
            current[2] || current[3] || current[6] || current[7])
          throw std::runtime_error("Adjacent QueryBatch snapshots or wrap differ");
        std::array<uint8_t, 48> memory;
        memory.fill(0xed);  // Arbitrary stale sentinels cannot classify the report.
        fm4::visibility::Publish(memory.data() + 4, current);
        if (std::memcmp(memory.data() + 4, current.data(), 32) ||
            !std::all_of(memory.begin(), memory.begin() + 4, [](auto c){return c == 0xed;}) ||
            !std::all_of(memory.begin() + 36, memory.end(), [](auto c){return c == 0xed;}))
          throw std::runtime_error("Report publication damaged guards or fields");
        previous = current; ++checked;
      }
    }
  }
  std::printf("PASS: %zu batched visibility snapshots, balanced lanes, wrapping and guarded publication\n", checked);
  return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
