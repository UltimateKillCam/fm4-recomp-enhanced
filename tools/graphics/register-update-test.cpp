// Independent byte-oriented oracle for scalar/SSE2 updates. GPL-3.0-only, 2026-10-17.
#include "register_update.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
static void Check(bool value) { if (!value) throw std::runtime_error("Constant update regression"); }
int main() try {
  uint32_t random = 0x19651975;
  auto next = [&] { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
  size_t cases = 0;
  for (size_t count = 0; count <= 2048; ++count) for (size_t offset = 0; offset < 4; ++offset) {
    std::vector<uint32_t> source(count + 8), output(count + 8, 0xAB12CD34), reference = output;
    for (auto& word : source) word = next();
    const auto* bytes = reinterpret_cast<const uint8_t*>(source.data() + offset);
    for (size_t i = 0; i < count; ++i)
      reference[offset + i] = uint32_t(bytes[i*4]) << 24 | uint32_t(bytes[i*4+1]) << 16 |
                                uint32_t(bytes[i*4+2]) << 8 | uint32_t(bytes[i*4+3]);
    const bool different = output != reference;
    Check(fm4::registers::CopySwappedIfChanged(output.data()+offset, source.data()+offset, count) == different);
    Check(output == reference);
    // Repeated writes must leave a simulated uploaded binding valid.
    bool binding_valid = true;
    binding_valid &= !fm4::registers::CopySwappedIfChanged(output.data()+offset, source.data()+offset, count);
    Check(binding_valid && output == reference);
    if (count) {
      source[offset + count / 2] ^= 0x80000001;
      Check(fm4::registers::CopySwappedIfChanged(output.data()+offset, source.data()+offset, count));
      Check(!fm4::registers::CopySwappedIfChanged(output.data()+offset, source.data()+offset, count));
    }
    ++cases;
  }
  std::printf("PASS: %zu endian, unchanged-binding, changed-word and range-boundary cases\n", cases);
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
