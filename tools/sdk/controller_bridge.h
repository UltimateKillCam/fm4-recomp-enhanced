// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#pragma once
#include <cstdint>
#if defined(FM4_CONTROLLER_STATIC_TEST)
#define FM4_CONTROLLER_API
#elif defined(FM4_CONTROLLER_BRIDGE_EXPORT)
#define FM4_CONTROLLER_API __declspec(dllexport)
#else
#define FM4_CONTROLLER_API __declspec(dllimport)
#endif
// Runs inside rexruntime so all calls share the SDK's SDL instance and handles.
extern "C" {
FM4_CONTROLLER_API void Fm4ControllerConfigure();
FM4_CONTROLLER_API uint16_t Fm4ControllerExtraButtons(uint32_t user);
// 1 = accepted, 0 = disconnected, -1 = output failed.
FM4_CONTROLLER_API int Fm4ControllerFeedback(uint32_t user, uint16_t low, uint16_t high,
    uint8_t brake, uint8_t throttle, float rumble_gain, float adaptive_gain);
FM4_CONTROLLER_API void Fm4ControllerStop(uint32_t user);
FM4_CONTROLLER_API void Fm4ControllerShutdown();
}
