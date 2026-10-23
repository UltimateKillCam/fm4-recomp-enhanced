// Optional hidden integration tests. Modified 2026-10-17. GPL-3.0-only.
// Only window_sdl.cpp redirects these SDL calls; ordinary launches call SDL.
#include <SDL3/SDL.h>
#include <cstdlib>
namespace {
bool Quiet() { return std::getenv("FM4_BACKGROUND")!=nullptr; }
}
extern "C" bool SDLCALL Fm4QuietShowWindow(SDL_Window* window) {
  return Quiet()||SDL_ShowWindow(window);
}
extern "C" bool SDLCALL Fm4QuietShowCursor() { return Quiet()||SDL_ShowCursor(); }
extern "C" bool SDLCALL Fm4QuietHideCursor() { return Quiet()||SDL_HideCursor(); }
extern "C" bool SDLCALL Fm4QuietRelativeMouse(SDL_Window* window,bool enabled) {
  return Quiet()||SDL_SetWindowRelativeMouseMode(window,enabled);
}
extern "C" bool SDLCALL Fm4QuietCaptureMouse(bool enabled) {
  return Quiet()||SDL_CaptureMouse(enabled);
}
