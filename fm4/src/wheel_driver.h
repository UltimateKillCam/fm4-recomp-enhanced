// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#pragma once
#include <filesystem>
#include <memory>
#include <rex/system/interfaces/input.h>
#include "wheel_math.h"

namespace fm4::wheel {
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(
    bool tool_mode, const std::filesystem::path& profile);
bool HasWheel(uint32_t user);
// Latest wheel steering sent to the game (-1..1) and the 1:1 cockpit settings.
// Returns false while the wheel is not the active input or 1:1 drawing is off.
bool CockpitSteering(float& steering, float& rotation_scale, float& rotation_degrees);
void SubmitGuestEffects(uint32_t user, EffectFrame frame);
// Player-car physics sampled where FM4 builds its wheel force (telemetry mode).
void SubmitTelemetry(const Telemetry& telemetry);
void StopGuestForces();
// Called from GetState while FM4 is about to check its cached device layout.
void RefreshGuestCapabilities(uint32_t user);
void ShutdownFeedback();
}
