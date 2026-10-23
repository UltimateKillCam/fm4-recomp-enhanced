// Virtual SDL output integration. Modified 2026-10-17. GPL-3.0-only.
// Opens only its own virtual device; never sends output to physical hardware.
#include "../sdk/controller_bridge.h"
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct Output {
  std::mutex mutex;
  std::array<uint8_t,47> effect{};
  unsigned effect_count=0;
  uint16_t low=0,high=0;
  bool reject=false;
} output;
void Require(bool condition,const char* message) {
  if(!condition) throw std::runtime_error(std::string(message)+": "+SDL_GetError());
}
bool SDLCALL Rumble(void*,Uint16 low,Uint16 high) {
  std::lock_guard lock(output.mutex); output.low=low; output.high=high; return true;
}
bool SDLCALL Effect(void*,const void* bytes,int size) {
  std::lock_guard lock(output.mutex);
  if(output.reject) return false;
  if(size!=int(output.effect.size())) return false;
  std::memcpy(output.effect.data(),bytes,size); ++output.effect_count; return true;
}
struct Cleanup {
  SDL_Gamepad* pad=nullptr; SDL_JoystickID id=0;
  ~Cleanup() {
    Fm4ControllerShutdown();
    if(pad) SDL_CloseGamepad(pad);
    if(id) SDL_DetachVirtualJoystick(id);
    SDL_Quit();
  }
};
}
int main() try {
  // Exclude all physical Windows input backends in this process. SDL's virtual
  // backend is independent. The runtime feedback bridge only sees our opened pad.
  for(const char* hint:{"SDL_JOYSTICK_HIDAPI","SDL_XINPUT_ENABLED",
      "SDL_JOYSTICK_DIRECTINPUT","SDL_JOYSTICK_RAWINPUT","SDL_JOYSTICK_WGI"})
    SDL_SetHint(hint,"0");
  Require(SDL_Init(SDL_INIT_GAMEPAD),"SDL gamepad init"); Cleanup cleanup;
  SDL_VirtualJoystickDesc desc{}; SDL_INIT_INTERFACE(&desc);
  desc.type=SDL_JOYSTICK_TYPE_GAMEPAD; desc.vendor_id=0x054c; desc.product_id=0x0ce6;
  desc.naxes=6; desc.nbuttons=21; desc.axis_mask=0x3f; desc.button_mask=(1u<<21)-1;
  desc.name="FM4 virtual DualSense regression"; desc.Rumble=Rumble; desc.SendEffect=Effect;
  cleanup.id=SDL_AttachVirtualJoystick(&desc); Require(cleanup.id!=0,"Attach virtual pad");
  char guid[33]{}; SDL_GUIDToString(SDL_GetJoystickGUIDForID(cleanup.id),guid,sizeof(guid));
  std::string mapping=std::string(guid)+",FM4 virtual DualSense,type:ps5,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,touchpad:b20,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
  Require(SDL_AddGamepadMapping(mapping.c_str())>=0,"Virtual mapping");
  cleanup.pad=SDL_OpenGamepad(cleanup.id); Require(cleanup.pad!=nullptr,"Open virtual pad");
  Require(SDL_GetGamepadType(cleanup.pad)==SDL_GAMEPAD_TYPE_PS5,"PS5 mapping type");
  Require(SDL_SetGamepadPlayerIndex(cleanup.pad,0),"Player zero");
  Fm4ControllerConfigure();
  Require(Fm4ControllerFeedback(0,65535,32768,255,255,1,.5f)==1,"Feedback accepted");
  {
    std::lock_guard lock(output.mutex);
    Require(output.low==65535&&output.high==32768,"SDL rumble amplitudes");
    Require(output.effect_count==1&&output.effect[0]==0x0f,"Combined output packet");
    Require(output.effect[10]==1&&output.effect[21]==1,"Both adaptive triggers");
  }
  // Identical packets are suppressed without skipping the feedback watchdog refresh.
  Require(Fm4ControllerFeedback(0,65535,32768,255,255,1,.5f)==1,"Repeated feedback");
  { std::lock_guard lock(output.mutex); Require(output.effect_count==1,"Packet deduplication"); }
  // The real bridge worker must clear abandoned effects without another guest poll.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  {
    std::lock_guard lock(output.mutex);
    Require(output.low==0&&output.high==0,"Watchdog rumble clear");
    Require(output.effect[10]==5&&output.effect[21]==5,"Watchdog trigger clear");
  }
  auto* joystick=SDL_GetGamepadJoystick(cleanup.pad);
  Require(SDL_SetJoystickVirtualButton(joystick,20,true),"Touchpad press"); SDL_UpdateJoysticks();
  Require(Fm4ControllerExtraButtons(0)==0x20,"Touchpad maps to Back");
  Require(SDL_SetJoystickVirtualButton(joystick,20,false),"Touchpad release"); SDL_UpdateJoysticks();
  Require(Fm4ControllerExtraButtons(0)==0,"Back release");
  Require(Fm4ControllerFeedback(0,65535,65535,255,255,1,0)==1,"Adaptive disable");
  { std::lock_guard lock(output.mutex); Require(output.effect[10]==5&&output.effect[21]==5,"Disabled trigger clear"); }
  Fm4ControllerStop(0);
  { std::lock_guard lock(output.mutex); Require(output.low==0&&output.high==0,"Handoff rumble clear"); }
  { std::lock_guard lock(output.mutex); output.reject=true; }
  Require(Fm4ControllerFeedback(0,65535,65535,255,255,1,1)==-1,"Failed output reported");
  { std::lock_guard lock(output.mutex);
    Require(output.low==0&&output.high==0,"Partial-output failure immediately clears accepted rumble");
    output.reject=false;
  }
  SDL_CloseGamepad(cleanup.pad); cleanup.pad=nullptr;
  Require(SDL_DetachVirtualJoystick(cleanup.id),"Virtual disconnect"); cleanup.id=0;
  Require(Fm4ControllerFeedback(0,65535,65535,255,255,1,1)==0,"Disconnected feedback rejected");
  cleanup.id=SDL_AttachVirtualJoystick(&desc); Require(cleanup.id!=0,"Reconnect virtual pad");
  cleanup.pad=SDL_OpenGamepad(cleanup.id); Require(cleanup.pad!=nullptr,"Reopen virtual pad");
  Require(SDL_SetGamepadPlayerIndex(cleanup.pad,0),"Reconnected player zero");
  Require(Fm4ControllerFeedback(0,65535,65535,255,255,1,1)==1,"Reconnected feedback accepted");
  Fm4ControllerShutdown();
  { std::lock_guard lock(output.mutex);
    Require(output.low==0&&output.high==0&&output.effect[10]==5&&output.effect[21]==5,"Shutdown clears reconnected output");
  }
  std::puts("PASS: virtual PS5 packet/rumble/trigger/touchpad, watchdog, handoff, output failure and reconnect/shutdown integration; no physical output");
  return 0;
} catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
