// Runtime graphics settings. GPL-3.0-only.
#pragma once
#include <cstdint>
namespace rex::ui { class Window; class ImGuiDrawer; class Presenter; }
namespace fm4::settings {
struct Request {
  uint64_t id = 0;
  int scale = 1;
  int width = 1280, height = 720;
  int effect = 0, post = 0, anisotropy = 3, fps = 0;
  double mip_bias = 0;
  bool fullscreen = false, persist = true;
};
#ifdef FM4_LIVE_SETTINGS_EXPORT
#define FM4_SETTINGS_API __declspec(dllexport)
#else
#define FM4_SETTINGS_API __declspec(dllimport)
#endif
FM4_SETTINGS_API bool Take(Request& request);
FM4_SETTINGS_API void Complete(const Request& request, bool success);
FM4_SETTINGS_API void Pace();
FM4_SETTINGS_API bool Capturing();
void Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer, rex::ui::Presenter* presenter);
void Detach();
}
