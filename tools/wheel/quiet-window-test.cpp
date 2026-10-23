// Hidden-window regression. Modified 2026-10-17. GPL-3.0-only.
// SDL calls are test doubles: this executable never opens or captures a window.
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
std::array<unsigned,5> calls{};
SDL_Window* last_window=nullptr;
bool last_enabled=false, result=true;
void Require(bool value) { if(!value) throw std::runtime_error("Quiet-window regression failed"); }
}
extern "C" bool SDLCALL SDL_ShowWindow(SDL_Window* window) { ++calls[0];last_window=window;return result; }
extern "C" bool SDLCALL SDL_ShowCursor() { ++calls[1];return result; }
extern "C" bool SDLCALL SDL_HideCursor() { ++calls[2];return result; }
extern "C" bool SDLCALL SDL_SetWindowRelativeMouseMode(SDL_Window* window,bool enabled) {
  ++calls[3];last_window=window;last_enabled=enabled;return result;
}
extern "C" bool SDLCALL SDL_CaptureMouse(bool enabled) { ++calls[4];last_enabled=enabled;return result; }
extern "C" bool SDLCALL Fm4QuietShowWindow(SDL_Window*);
extern "C" bool SDLCALL Fm4QuietShowCursor();
extern "C" bool SDLCALL Fm4QuietHideCursor();
extern "C" bool SDLCALL Fm4QuietRelativeMouse(SDL_Window*,bool);
extern "C" bool SDLCALL Fm4QuietCaptureMouse(bool);

int main() try {
  const char* previous=std::getenv("FM4_BACKGROUND");
  const std::string saved=previous?previous:"";
  const bool had_previous=previous!=nullptr;
  struct Restore { bool present;std::string value;
    ~Restore(){_putenv_s("FM4_BACKGROUND",present?value.c_str():"");}
  } restore{had_previous,saved};
  auto* window=reinterpret_cast<SDL_Window*>(uintptr_t(1));
  _putenv_s("FM4_BACKGROUND","1");
  result=false;
  Require(Fm4QuietShowWindow(window));
  Require(Fm4QuietShowCursor()&&Fm4QuietHideCursor());
  Require(Fm4QuietRelativeMouse(window,true)&&Fm4QuietRelativeMouse(window,false));
  Require(Fm4QuietCaptureMouse(true)&&Fm4QuietCaptureMouse(false));
  Require(calls==std::array<unsigned,5>{});
  _putenv_s("FM4_BACKGROUND","");
  for(bool value:{false,true}) {
    result=value;
    Require(Fm4QuietShowWindow(window)==value&&last_window==window);
    Require(Fm4QuietShowCursor()==value&&Fm4QuietHideCursor()==value);
    Require(Fm4QuietRelativeMouse(window,value)==value&&last_window==window&&last_enabled==value);
    Require(Fm4QuietCaptureMouse(value)==value&&last_enabled==value);
  }
  Require(calls==std::array<unsigned,5>{2,2,2,2,2});
  std::puts("Quiet window suppresses visibility, cursor and capture calls; normal forwarding preserved.");
  return 0;
} catch(const std::exception& error) { std::fprintf(stderr,"%s\n",error.what());return 1; }
