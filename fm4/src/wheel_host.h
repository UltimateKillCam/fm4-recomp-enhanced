// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#pragma once

#include "wheel_math.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fm4::wheel {

struct AxisBinding {
  // Empty device selects the wheel. An instance GUID selects separate USB pedals.
  std::string device;
  std::string axis;
  Calibration calibration;
};

struct Config {
  bool enabled = true;
  std::string device;  // Empty selects a driving / actuator-capable FFB interface.
  uint32_t user = 0;
  float rotation_scale = 1;
  float rotation_degrees = 900;  // The wheelbase's configured lock-to-lock rotation.
  bool full_rotation = true;     // Draw the cockpit wheel 1:1 without FM4's 90-degree clamp.
  float gain = 0.4f;
  bool invert_force = false;
  float detail_gain = 0.5f;  // Rumble detail strength relative to full torque.
  float engine_gain = 0.1f;  // Sustained motor rumble relative to event detail.
  float road_gain = 0.3f;    // Road texture from front tyre loads (telemetry source).
  ForceShape shape;          // Reweights FM4's effects for a modern wheelbase.
  bool telemetry_force = true;  // [force] mode: telemetry (default) or game.
  bool ffb_enabled = true;
  uint32_t timeout_ms = 150;
  float rumble_gain = 1;
  float stick_deadzone = .06f;
  bool adaptive_triggers = true;
  float adaptive_gain = .35f;
  bool touchpad_back = true;
  AxisBinding steering{{}, "Wheel Axis"};
  AxisBinding throttle{{}, "Accelerator"};
  AxisBinding brake{{}, "Brake"};
  AxisBinding clutch{{}, "Clutch"};
  AxisBinding handbrake{{}, "disabled"};
  // One-based button numbers, zero disables a binding. Xbox bit ordering.
  // Fanatec McLaren PC mapping: P/N, LSB/RSB, paddles, A/B/X/Y.
  std::array<int, 16> buttons{0, 0, 0, 0, 10, 9, 12, 11, 6, 5, 0, 0, 2, 3, 1, 4};
  std::array<std::string, 16> button_devices;
  bool shifter_enabled = false;
  std::string shifter_device;
  unsigned shifter_gears = 7;
  std::array<int, 9> gear_buttons{};  // Reverse, then gears 1..8; zero disables.
  std::string force_backend = "auto"; // auto, native or software constant torque.
  float velocity_scale = .1f, acceleration_scale = .01f;
};

struct State {
  bool connected = false;
  bool ffb_capable = false;
  bool output_active = false;
  uint64_t force_age_ms = 0;
  std::string force_stop_reason;
  uint32_t native_force_flags = 0;
  bool native_force_flags_known = false;
  unsigned downloaded_effects = 0;
  uint32_t packet = 0;
  uint16_t buttons = 0;
  int16_t gear = 0;
  float steering = 0, throttle = 0, brake = 0, clutch = 0, handbrake = 0;
  std::string device;
  std::string error;
  std::string force_backend;
};

Config LoadConfig(const std::filesystem::path& path);
void WriteDefaultConfig(const std::filesystem::path& path);

// DirectInput is accessed only on the worker, including effect teardown.
// The worker owns the watchdog independently of guest rendering and input polling.
class Host {
 public:
  explicit Host(Config config);
  ~Host();
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  void SetWindow(void* window);
  void SetActive(bool active);
  void Submit(EffectFrame forces);
  void SubmitTelemetry(const Telemetry& telemetry);
  void Stop();
  State Snapshot() const;
  // Latest calibrated steering from the 250 Hz poll, without copying State.
  float LiveSteering() const;
  const Config& config() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fm4::wheel
