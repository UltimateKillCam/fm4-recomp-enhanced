// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace fm4::controller {
// SDL's 47-byte DS5EffectsState_t payload. SDL adds USB/BT framing and BT CRC.
// Trigger modes follow SDL/test/testcontroller.c: 0x01 resistance, 0x05 clear.
inline std::array<uint8_t, 47> TriggerEffect(uint16_t low, uint16_t high,
    uint8_t brake, uint8_t throttle, float gain) {
  std::array<uint8_t, 47> output{};
  output[0] = 0x0C;
  output[10] = output[21] = 0x05;
  if (!std::isfinite(gain)) return output;
  gain = std::clamp(gain, 0.f, 1.f);
  auto effect = [&](size_t offset, uint16_t motor, uint8_t travel) {
    if (!motor || travel < 12 || gain == 0) return;
    output[offset] = 0x01;
    output[offset + 1] = 24;
    output[offset + 2] = uint8_t(std::lround(110.f * gain * motor / 65535.f));
    if (!output[offset + 2]) output[offset] = 0x05;
  };
  effect(10, high, throttle); effect(21, low, brake);
  return output;
}
inline std::array<uint8_t, 47> FeedbackEffect(uint16_t low, uint16_t high,
    uint8_t brake, uint8_t throttle, float rumble_gain, float adaptive_gain) {
  auto output = TriggerEffect(low, high, brake, throttle, adaptive_gain);
  if (!std::isfinite(rumble_gain)) rumble_gain = 0;
  rumble_gain = std::clamp(rumble_gain, 0.f, 1.f);
  const auto left = uint16_t(low * rumble_gain);
  const auto right = uint16_t(high * rumble_gain);
  // Match SDL's compatible rumble-emulation path. Omitting these bits in a
  // raw trigger packet restores audio haptics and cancels emulated vibration.
  if (left || right) output[0] |= 0x03;
  output[2] = uint8_t(right >> 9);
  output[3] = uint8_t(left >> 9);
  return output;
}
}
