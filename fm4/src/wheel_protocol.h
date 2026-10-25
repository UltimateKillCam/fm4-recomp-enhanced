#pragma once
#include "wheel_math.h"
#include <bit>
#include <optional>
#include <span>

namespace fm4::wheel {

// FM4's 0x74-byte Xbox effect record, as constructed by 0x828CE8D0 in
// the validated B34DFB88... executable. Keep wire decoding independent of SDK
// endian wrappers so malformed records can be tested without guest memory.
inline uint32_t ReadBE(std::span<const uint8_t> bytes, size_t offset) {
  return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
      (uint32_t(bytes[offset + 2]) << 8) | uint32_t(bytes[offset + 3]);
}
struct DecodedEffect { uint32_t id; Effect effect; };
inline std::optional<DecodedEffect> DecodeEffect(std::span<const uint8_t> bytes) {
  if (bytes.size() != 0x74) return std::nullopt;
  DecodedEffect decoded{};
  decoded.id = ReadBE(bytes, 0);
  if (decoded.id < 1 || decoded.id > 5) return std::nullopt;
  auto& effect = decoded.effect;
  switch (ReadBE(bytes, 4)) {
    case 10: effect.type = EffectType::Inertia; break;
    case 11: effect.type = EffectType::Friction; break;
    case 9: effect.type = EffectType::Damper; break;
    case 8: effect.type = EffectType::Spring; break;
    case 1: effect.type = EffectType::Constant; break;
    default: return std::nullopt;
  }
  // FM4 uses one fixed effect of each type, in this order.
  if (static_cast<size_t>(effect.type) != decoded.id - 1) return std::nullopt;
  effect.gain = float(ReadBE(bytes, 0x0C)) / 255.0f;
  auto signed_value = [&](size_t offset) {
    return float(std::bit_cast<int32_t>(ReadBE(bytes, offset))) / 127.0f;
  };
  if (effect.type == EffectType::Constant) {
    effect.magnitude = signed_value(0x2C);
  } else {
    auto& condition = effect.condition;
    condition.center = signed_value(0x2C);
    condition.positive = signed_value(0x30);
    condition.negative = signed_value(0x34);
    condition.positive_limit = float(ReadBE(bytes, 0x38)) / 255.0f;
    condition.negative_limit = float(ReadBE(bytes, 0x3C)) / 255.0f;
    condition.deadband = float(ReadBE(bytes, 0x40)) / 255.0f;
  }
  return effect.Valid() ? std::optional(decoded) : std::nullopt;
}

}  // namespace fm4::wheel
