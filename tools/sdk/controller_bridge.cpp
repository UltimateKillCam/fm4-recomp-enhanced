// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#define FM4_CONTROLLER_BRIDGE_EXPORT
#include "controller_bridge.h"
#include "dualsense_effect.h"
#include <SDL3/SDL.h>
#include <rex/logging.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
struct Feedback {
  SDL_JoystickID id = 0;
  bool active = false, triggers_sent = false;
  bool reported_adaptive = false;
  Clock::time_point time;
  std::array<uint8_t, 47> triggers{};
};
std::mutex mutex;
std::condition_variable wake;
std::array<Feedback, 4> states;
// The pinned SDK hard-exits in its first closing listener. A process-lifetime
// handle avoids a joinable std::thread destructor during Windows DLL detach.
// The app's close-request hook performs the normal explicit stop/join.
std::thread* worker = nullptr;
bool stopping = false;

struct JoystickLock {
  JoystickLock() { SDL_LockJoysticks(); }
  ~JoystickLock() { SDL_UnlockJoysticks(); }
};
SDL_Gamepad* Find(uint32_t user) {
  int count = 0;
  auto* ids = SDL_GetGamepads(&count);
  SDL_Gamepad* result = nullptr;
  for (int i = 0; i < count; ++i) {
    auto* pad = SDL_GetGamepadFromID(ids[i]);
    if (pad && SDL_GetGamepadPlayerIndex(pad) == int(user)) { result = pad; break; }
  }
  SDL_free(ids);
  return result;
}
void Clear(uint32_t user) {
  auto& state = states[user];
  if (!state.active && !state.triggers_sent) return;
  JoystickLock lock;
  auto* pad = Find(user);
  if (pad && SDL_GetGamepadID(pad) == state.id) {
    SDL_RumbleGamepad(pad, 0, 0, 0);
    if (state.triggers_sent) {
      const auto off = fm4::controller::TriggerEffect(0, 0, 0, 0, 0);
      SDL_SendGamepadEffect(pad, off.data(), int(off.size()));
    }
  }
  state = {};
}
}

void Fm4ControllerConfigure() {
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
  std::lock_guard lock(mutex);
  if (worker) return;
  stopping = false;
  worker = new std::thread([] {
    std::unique_lock lock(mutex);
    while (!stopping) {
      wake.wait_for(lock, std::chrono::milliseconds(25));
      for (uint32_t user = 0; user < states.size(); ++user)
        if (states[user].active && Clock::now() - states[user].time > std::chrono::milliseconds(250)) Clear(user);
    }
  });
}
uint16_t Fm4ControllerExtraButtons(uint32_t user) {
  JoystickLock lock;
  auto* pad = Find(user);
  // Create remains Back; the touchpad click provides a second Back binding.
  return pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_TOUCHPAD) ? 0x20 : 0;
}
int Fm4ControllerFeedback(uint32_t user, uint16_t low, uint16_t high,
    uint8_t brake, uint8_t throttle, float rumble_gain, float adaptive_gain) {
  if (user >= states.size()) return 0;
  std::lock_guard lock(mutex);
  if (stopping) { Clear(user); return 1; }
  JoystickLock joystick;
  auto* pad = Find(user);
  if (!pad) { states[user] = {}; return 0; }
  if (!std::isfinite(rumble_gain)) rumble_gain = 0;
  rumble_gain = std::clamp(rumble_gain, 0.f, 1.f);
  auto& state = states[user];
  const auto id = SDL_GetGamepadID(pad);
  if (id != state.id) state = {};
  state.id = id;
  // A later trigger-packet failure must also stop rumble that this call may
  // already have started, including the first update after a handoff.
  state.active = true;
  const bool rumble_ok = SDL_RumbleGamepad(pad, Uint16(low * rumble_gain), Uint16(high * rumble_gain), 250);
  bool trigger_ok = true;
  if (SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5) {
    const auto effect = fm4::controller::FeedbackEffect(low, high, brake, throttle, rumble_gain, adaptive_gain);
    if (!state.triggers_sent || state.triggers != effect) {
      trigger_ok = SDL_SendGamepadEffect(pad, effect.data(), int(effect.size()));
      if (trigger_ok) {
        state.triggers = effect; state.triggers_sent = true;
        if (!state.reported_adaptive && (effect[10] == 1 || effect[21] == 1)) {
          REXLOG_INFO("FM4 DualSense adaptive resistance accepted: brake={}, throttle={}", effect[23], effect[12]);
          state.reported_adaptive = true;
        }
      }
    }
  }
  if (!rumble_ok || !trigger_ok) { Clear(user); return -1; }
  state.active = low || high || state.triggers_sent;
  state.time = Clock::now();
  return 1;
}
void Fm4ControllerStop(uint32_t user) {
  std::lock_guard lock(mutex);
  if (user < states.size()) Clear(user);
}
void Fm4ControllerShutdown() {
  { std::lock_guard lock(mutex); stopping = true; wake.notify_all(); }
  if (worker) { worker->join(); delete worker; worker = nullptr; }
  std::lock_guard lock(mutex);
  for (uint32_t user = 0; user < states.size(); ++user) Clear(user);
}
