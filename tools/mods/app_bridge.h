// Optional game application bridge. GPL-3.0-only.
#pragma once
#include <cstdlib>
#include <string_view>
#ifdef FM4_ENABLE_MODS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdexcept>
namespace fm4::mod_bridge {
inline FARPROC Entry(const char* name) {
  auto* enabled = std::getenv("FM4_MODS");
  if (!enabled || std::string_view(enabled) != "1") return nullptr;
  auto* session = std::getenv("FM4_MOD_SESSION");
  if (!session || !*session) return nullptr;
  auto module = GetModuleHandleW(L"rexruntime.dll");
  auto entry = module ? GetProcAddress(module, name) : nullptr;
  if (!entry) throw std::runtime_error("This runtime was built without FM4 mod support");
  return entry;
}
inline void Configure(rex::PathConfig& paths) { if (auto p = Entry("Fm4ModsConfigure")) reinterpret_cast<void(*)(rex::PathConfig*)>(p)(&paths); }
inline bool Check(const rex::PathConfig& paths) { auto p = Entry("Fm4ModsCheckPaths"); return !p || reinterpret_cast<bool(*)(const rex::PathConfig*)>(p)(&paths); }
inline void Start(rex::Runtime* runtime) { if (auto p = Entry("Fm4ModsStart")) reinterpret_cast<void(*)(rex::Runtime*)>(p)(runtime); }
inline void Attach(rex::ui::ImGuiDrawer* drawer) { if (auto p = Entry("Fm4ModsAttach")) reinterpret_cast<void(*)(rex::ui::ImGuiDrawer*)>(p)(drawer); }
inline void Stop() { if (auto p = Entry("Fm4ModsStop")) reinterpret_cast<void(*)()>(p)(); }
}
#else
namespace fm4::mod_bridge {
inline void Configure(rex::PathConfig&) {}
inline bool Check(const rex::PathConfig&) { return true; }
inline void Start(rex::Runtime*) {}
inline void Attach(rex::ui::ImGuiDrawer*) {}
inline void Stop() {}
}
#endif
