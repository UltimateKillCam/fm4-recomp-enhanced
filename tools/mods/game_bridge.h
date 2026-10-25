// Optional game-thread extension tick. GPL-3.0-only.
#pragma once
#include <string_view>
#if defined(FM4_MOD_GAME_HOOKS) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdlib>
extern "C" __declspec(dllexport) uint32_t Fm4ModGameProtocolV1() { return 1; }
inline void Fm4ModsGuestUpdate(double delta) {
  static const auto tick = []() -> void(*)(double) {
    const char* enabled = std::getenv("FM4_MODS");
    if (!enabled || std::string_view(enabled) != "1") return nullptr;
    auto runtime = GetModuleHandleW(L"rexruntime.dll");
    return runtime ? reinterpret_cast<void(*)(double)>(GetProcAddress(runtime, "Fm4ModsUpdate")) : nullptr;
  }();
  if (tick) tick(delta);
}
#else
inline void Fm4ModsGuestUpdate(double) {}
#endif
