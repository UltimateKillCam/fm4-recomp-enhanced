// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fm4::wheel {

inline unsigned AutoSelectRank(bool force_feedback, bool driving, bool actuator, bool steering) {
  return force_feedback && steering ? (driving ? 2u : actuator ? 1u : 0u) : 0u;
}

struct Calibration {
  int minimum = 0;
  int center = 32767;
  int maximum = 65535;
  float deadzone = 0.0f;
  float saturation = 1.0f;
  float gamma = 1.0f;
  bool inverted = false;

  bool Valid(bool centered) const {
    return minimum < maximum && (!centered || (minimum < center && center < maximum)) &&
           std::isfinite(deadzone) && std::isfinite(saturation) && std::isfinite(gamma) &&
           deadzone >= 0 && saturation <= 1 && deadzone < saturation && gamma > 0;
  }

  float Apply(int raw, bool centered) const {
    if (!Valid(centered)) return 0;
    raw = std::clamp(raw, minimum, maximum);
    float value = centered
        ? (raw < center ? float(raw - center) / float(center - minimum)
                        : float(raw - center) / float(maximum - center))
        : float(raw - minimum) / float(maximum - minimum);
    if (inverted) value = centered ? -value : 1.0f - value;
    const float magnitude = std::clamp((std::abs(value) - deadzone) /
                                      (saturation - deadzone), 0.0f, 1.0f);
    return std::copysign(std::pow(magnitude, gamma), value);
  }
};

inline int16_t Steering(float value) {
  if (!std::isfinite(value)) return 0;
  value = std::clamp(value, -1.0f, 1.0f);
  return static_cast<int16_t>(std::lround(value * (value < 0 ? 32768.0f : 32767.0f)));
}
inline uint8_t Pedal(float value) {
  return std::isfinite(value) ? static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255)) : 0;
}

// FM4's wheel report uses -1 for reverse, 0 for neutral, and 1..8 for gears.
// Conflicting contacts fail to neutral rather than choosing an arbitrary gear.
inline int16_t SelectGear(const std::array<bool, 9>& contacts, unsigned gears) {
  int16_t result = 0;
  unsigned pressed = 0;
  for (unsigned i = 0; i <= std::min(gears, 8u); ++i) {
    if (!contacts[i]) continue;
    ++pressed; result = i == 0 ? int16_t(-1) : int16_t(i);
  }
  return pressed == 1 ? result : 0;
}

struct ButtonPulses {
  std::array<uint64_t, 16> until{};
  void Press(uint16_t buttons, uint64_t now) {
    for (unsigned bit = 0; bit < until.size(); ++bit)
      if (buttons & (1u << bit)) until[bit] = now + 100;
  }
  uint16_t Read(uint64_t now) const {
    uint16_t buttons = 0;
    for (unsigned bit = 0; bit < until.size(); ++bit)
      if (now < until[bit]) buttons |= uint16_t(1u << bit);
    return buttons;
  }
  void Clear() { until = {}; }
};

// All values are normalized host values, never controller rumble magnitudes.
struct Forces {
  float torque = 0;
  float spring = 0;
  float damper = 0;
  float friction = 0;
  bool Valid() const {
    return std::isfinite(torque) && std::isfinite(spring) &&
           std::isfinite(damper) && std::isfinite(friction) &&
           std::abs(torque) <= 1 && spring >= 0 && spring <= 1 &&
           damper >= 0 && damper <= 1 && friction >= 0 && friction <= 1;
  }
};

enum class EffectType { Inertia, Friction, Damper, Spring, Constant };
struct Condition {
  float center = 0, positive = 0, negative = 0;
  float positive_limit = 0, negative_limit = 0, deadband = 0;
  bool Valid() const {
    return std::isfinite(center) && std::isfinite(positive) && std::isfinite(negative) &&
        std::isfinite(positive_limit) && std::isfinite(negative_limit) && std::isfinite(deadband) &&
        std::abs(center) <= 1 && std::abs(positive) <= 1 && std::abs(negative) <= 1 &&
        positive_limit >= 0 && positive_limit <= 1 && negative_limit >= 0 &&
        negative_limit <= 1 && deadband >= 0 && deadband <= 1;
  }
};
struct Effect {
  EffectType type = EffectType::Constant;
  float gain = 0;
  float magnitude = 0;
  Condition condition;
  bool playing = false;
  bool Valid() const {
    return std::isfinite(gain) && gain >= 0 && gain <= 1 &&
        std::isfinite(magnitude) && std::abs(magnitude) <= 1 && condition.Valid();
  }
};
struct EffectFrame {
  std::array<Effect, 5> slots;
  // The Xbox wheel's two rumble motors (low and high frequency), 0..1. FM4
  // drives them with engine, kerb and surface detail.
  float rumble_low = 0, rumble_high = 0;
  bool Valid() const {
    for (const auto& effect : slots) if (!effect.Valid()) return false;
    return std::isfinite(rumble_low) && rumble_low >= 0 && rumble_low <= 1 &&
        std::isfinite(rumble_high) && rumble_high >= 0 && rumble_high <= 1;
  }
};

// FM4's constant torque is positive toward positive steering (right). A
// DirectInput force is described by the direction it comes from, so on the
// steering axis positive pushes left. Some drivers differ; invert flips only
// this torque, never the spring/damper conditions.
inline float HostTorque(float guest, bool invert) { return invert ? guest : -guest; }

// FM4 has one force tuning for every wheel (Media/ControllerFFB.ini): physics
// self-aligning torque at scale 1, a 0.5 artificial centring spring and a 0.1
// damper, sized for the Xbox 360 wheel's small motor. ShapeForces reweights the
// game's own effects for a modern wheelbase: the physics torque is raised on a
// bounded curve, and the centring spring and damper are scaled.
// steering_weight=1, centering=1, damping=1 is stock FM4.
struct ForceShape { float steering_weight = 3.5f, centering = 0, damping = .25f; };
inline float SteeringWeightCurve(float torque, float weight) {
  if (!std::isfinite(torque)) return 0;
  torque = std::clamp(torque, -1.0f, 1.0f);
  if (!std::isfinite(weight)) return 0;
  weight = std::clamp(weight, 1.0f, 5.0f);
  // Keep the requested slope at zero without flattening the upper range.
  return weight * torque / (1 + (weight - 1) * std::abs(torque));
}
inline void ShapeForces(EffectFrame& frame, const ForceShape& shape) {
  for (auto& effect : frame.slots) {
    if (effect.type == EffectType::Constant) {
      effect.magnitude = SteeringWeightCurve(effect.magnitude, shape.steering_weight);
    } else if (effect.type == EffectType::Spring || effect.type == EffectType::Damper) {
      const float scale = effect.type == EffectType::Spring ? shape.centering : shape.damping;
      auto& condition = effect.condition;
      condition.positive = std::clamp(condition.positive * scale, -1.0f, 1.0f);
      condition.negative = std::clamp(condition.negative * scale, -1.0f, 1.0f);
    }
  }
}

// Rumble detail as steering-axis vibration: low motor at 12 Hz, high at 40 Hz. FM4's
// wheel table drives the low motor with gear shifts (0.6) and impacts, which a
// direct-drive base turns into thumps, so it is weighted down; kerb, surface and
// engine detail stay on the high motor.
constexpr float kRumbleLowHz = 12, kRumbleHighHz = 40, kRumbleLowWeight = .35f;
// The two motor signals mix engine and events. Suppress their sustained floor,
// preserving positive excursions without generating a buzz when a motor stops.
struct RumbleDetail {
  std::array<float, 2> baseline{};
  uint64_t last_ms = 0;
  bool primed = false;
  std::array<float, 2> Update(float low, float high, uint64_t now_ms, float engine = .1f) {
    if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(engine)) {
      Reset(); return {};
    }
    const std::array<float, 2> input{std::clamp(low, 0.0f, 1.0f), std::clamp(high, 0.0f, 1.0f)};
    engine = std::clamp(engine, 0.0f, 1.0f);
    if (!primed || now_ms < last_ms || now_ms - last_ms > 200) {
      baseline = input; primed = true;
    } else {
      const float alpha = float(1 - std::exp(-double(now_ms - last_ms) / 350.0));
      for (size_t i = 0; i < input.size(); ++i) baseline[i] += alpha * (input[i] - baseline[i]);
    }
    last_ms = now_ms;
    std::array<float, 2> output{};
    for (size_t i = 0; i < input.size(); ++i)
      output[i] = engine * input[i] + (1 - engine) * std::max(0.0f, input[i] - baseline[i]);
    return output;
  }
  void Reset() { *this = {}; }
};

inline float RumbleForce(const EffectFrame& frame, double seconds, float detail) {
  constexpr double kTwoPi = 6.283185307179586;
  return detail * float(frame.rumble_low * kRumbleLowWeight * std::sin(kTwoPi * kRumbleLowHz * seconds) +
      frame.rumble_high * std::sin(kTwoPi * kRumbleHighHz * seconds));
}

// Fallback for drivers that expose constant torque but not all five conditions.
// Derivatives are bounded and filtered; a reacquisition never differentiates
// against a stale sample. Scale parameters are explicit because DirectInput
// does not specify a common physical velocity/acceleration unit for wheels.
struct Motion {
  float position = 0, velocity = 0, acceleration = 0;
  uint64_t previous_ms = 0;
  void Reset() { *this = {}; }
  void Update(float value, uint64_t now, float velocity_scale, float acceleration_scale) {
    if (!std::isfinite(value)) { Reset(); return; }
    value = std::clamp(value, -1.0f, 1.0f);
    if (!previous_ms || now <= previous_ms || now - previous_ms > 100) {
      position = value; velocity = acceleration = 0; previous_ms = now; return;
    }
    const float dt = float(now - previous_ms) * .001f;
    const float alpha = dt / (.020f + dt);
    const float previous_velocity = velocity;
    const float target_velocity = std::clamp((value - position) / dt * velocity_scale, -1.0f, 1.0f);
    velocity += alpha * (target_velocity - velocity);
    const float target_acceleration = std::clamp((velocity - previous_velocity) / dt * acceleration_scale, -1.0f, 1.0f);
    acceleration += alpha * (target_acceleration - acceleration);
    position = value; previous_ms = now;
  }
};

inline float ConditionForce(const Condition& condition, float metric, bool friction = false) {
  if (!condition.Valid() || !std::isfinite(metric)) return 0;
  const float delta = metric - condition.center;
  if (std::abs(delta) <= condition.deadband) return 0;
  const float displacement = friction ? std::copysign(1.0f, delta)
      : delta - std::copysign(condition.deadband, delta);
  const float coefficient = delta > 0 ? condition.positive : condition.negative;
  const float saturation = delta > 0 ? condition.positive_limit : condition.negative_limit;
  return std::clamp(coefficient * displacement, -saturation, saturation);
}

inline float MixSoftwareForces(const EffectFrame& frame, const Motion& motion, float gain) {
  if (!frame.Valid() || !std::isfinite(gain) || gain < 0 || gain > 1) return 0;
  float torque = 0;
  for (const auto& effect : frame.slots) {
    if (!effect.playing || effect.gain == 0) continue;
    float value = 0;
    switch (effect.type) {
      case EffectType::Constant: value = effect.magnitude; break;
      case EffectType::Spring: value = ConditionForce(effect.condition, motion.position); break;
      case EffectType::Damper: value = ConditionForce(effect.condition, motion.velocity); break;
      case EffectType::Inertia: value = ConditionForce(effect.condition, motion.acceleration); break;
      case EffectType::Friction: value = ConditionForce(effect.condition, motion.velocity, true); break;
    }
    torque += value * effect.gain;
  }
  // Limit the combined output before applying the user's total force limit.
  return std::clamp(torque, -1.0f, 1.0f) * gain;
}

// Cockpit steering wheel. FM4 reports a 900-degree wheel, so its cockpit angle is
// steering input x 450 degrees (radians), raised by up to 2.25x at speed by the
// GraphicsSteeringFilter tunables, and clamped to +/-90 degrees; the renderer then
// draws a fraction of it (a live tunable, 0.75 in stock data). Remove the clamp and
// draw the physical wheel angle instead.
constexpr double kGuestWheelRadiansPerInput = 7.853981633974483;  // 450 degrees
constexpr double kGuestWheelClamp = 1.5707963267948966;           // 90 degrees
// True when a cockpit angle is the game's own mapping of this steering input.
inline bool TracksSteering(double game, double input) {
  if (!std::isfinite(game) || !std::isfinite(input)) return false;
  if (std::abs(input) < 0.03) return std::abs(game) < 0.1;
  if ((game > 0) != (input > 0)) return false;
  const double low = std::min(std::abs(input) * kGuestWheelRadiansPerInput * 0.8, kGuestWheelClamp);
  const double high = std::min(std::abs(input) * kGuestWheelRadiansPerInput * 2.4, kGuestWheelClamp);
  return std::abs(game) >= low - 0.05 && std::abs(game) <= high + 0.05;
}
// Telemetry force mode. FM4 builds its wheel force once per frame from the player
// car's steering torque (its own aligning-torque model, clamped to +/-1), speed (m/s)
// and rear slip, then scales it by DynFFBScale and sends it as a +/-127 integer. The
// host samples the torque before that quantization and per-wheel physics state (front
// wheels 0-1, rear 2-3). Read-only: car physics is never changed.
struct Telemetry {
  float torque = 0, speed = 0, rear_slip = 0;
  std::array<float, 4> slip{}, load{};
  uint8_t contact = 0;  // Bit i: wheel i touches a surface.
  std::array<uint8_t, 4> surface{};
  bool Valid() const {
    return std::isfinite(torque) && std::isfinite(speed) && std::isfinite(rear_slip) &&
        std::abs(torque) <= 1.5f && std::abs(speed) < 1000;
  }
};
// Road texture: bumps under one front tyre change its load relative to the other and
// kick the steering. The normalized front load difference is high-passed (40 ms), so
// steady cornering load transfer adds nothing; only changes are felt. Telemetry only.
struct RoadTexture {
  float previous = 0, value = 0;
  uint64_t last_ms = 0;
  bool primed = false;
  float Update(const Telemetry& t, uint64_t now_ms) {
    const float total = t.load[0] + t.load[1];
    const float x = std::isfinite(total) && total > 1 ? (t.load[0] - t.load[1]) / total : 0;
    if (!primed || now_ms <= last_ms || now_ms - last_ms > 200) {
      previous = x; value = 0; primed = true; last_ms = now_ms; return 0;
    }
    const double dt = double(now_ms - last_ms) / 1000.0, tau = .04;
    last_ms = now_ms;
    value = float(tau / (tau + dt) * (value + x - previous));
    previous = x;
    value = std::clamp(value, -1.0f, 1.0f);
    return value;
  }
  void Reset() { primed = false; value = 0; }
};

// Telemetry arrives once per game frame; stepping the output at 60 Hz is felt as fine
// clicks on a direct-drive base. Ramp from the current output over two worker
// ticks instead of waiting a full frame; a retarget never jumps the output.
struct FrameInterpolator {
  float previous = 0, current = 0;
  uint64_t current_ms = 0;
  bool primed = false;
  void Push(float value, uint64_t now_ms) {
    if (!std::isfinite(value)) value = 0;
    previous = !primed || now_ms < current_ms || now_ms - current_ms > 200 ? 0 : Sample(now_ms);
    current = value; current_ms = now_ms;
    primed = true;
  }
  float Sample(uint64_t now_ms) const {
    if (!primed) return 0;
    if (now_ms <= current_ms) return previous;
    const float progress = std::min(1.0f, float(now_ms - current_ms) / 8.0f);
    return previous + (current - previous) * progress;
  }
  void Reset() { *this = {}; }
};

// Upsamples the once-per-frame torque into the 250 Hz output loop with a short
// one-pole filter, so the wheel sees a continuous force instead of frame steps.
struct TorqueSmoother {
  float value = 0;
  uint64_t last_ms = 0;
  bool primed = false;
  float Update(float target, uint64_t now_ms, float tau_ms = 4) {
    target = std::isfinite(target) ? std::clamp(target, -1.0f, 1.0f) : 0;
    if (!primed || now_ms < last_ms || now_ms - last_ms > 200) {
      value = 0; primed = true; last_ms = now_ms; return value;
    }
    if (!std::isfinite(tau_ms) || tau_ms <= 0) tau_ms = 4;
    const double dt = double(std::min<uint64_t>(now_ms - last_ms, 100));
    last_ms = now_ms;
    value += float((target - value) * (1 - std::exp(-dt / double(tau_ms))));
    return value;
  }
  void Reset() { *this = {}; }
};

// A presentation locked to the live wheel is released only on clear disagreement:
// steering the other way, or the game steering hard while the wheel is centred
// (a replay). FM4's steering filter lag and speed scaling are not disagreement,
// so the drawn wheel never snaps back to the game's clamped angle mid-corner.
inline bool ContradictsSteering(double game, double input) {
  if (!std::isfinite(game) || !std::isfinite(input)) return true;
  if (std::abs(input) < 0.03) return std::abs(game) > 0.35;
  return std::abs(input) > 0.1 && std::abs(game) > 0.15 && (game > 0) != (input > 0);
}
// Angle to pass to the cockpit so the drawn wheel matches the physical wheel.
inline double OneToOneWheelAngle(double input, float rotation_scale, float rotation_degrees, double draw_scale) {
  if (!std::isfinite(input) || rotation_scale <= 0 || !(draw_scale > 0.05 && draw_scale < 10)) return 0;
  const double physical = input / rotation_scale * rotation_degrees * 0.5 * 3.141592653589793 / 180.0;
  return physical / draw_scale;
}

// Pure policy, shared by the host worker and regression tests.
inline bool CanApplyForces(bool focused, bool active, bool connected,
                           uint64_t now, uint64_t last, uint32_t timeout) {
  return focused && active && connected && last != 0 && now >= last &&
         now - last < timeout;
}

}  // namespace fm4::wheel
