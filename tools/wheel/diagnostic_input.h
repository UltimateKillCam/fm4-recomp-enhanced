#pragma once
// Wheel edition fixture. Modified 2026-10-17. GPL-3.0-only.
// Opt-in controller fixture for hidden integration runs. This communicates with
// the game's input API, never with desktop windows or physical force devices.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <queue>
#include <rex/input/input.h>
#include <rex/logging.h>
// Keep this application fixture separate from the optional graphics SDK's
// legacy fixture, whose inline globals are exported by the Windows DLL.
namespace fm4::wheel_diagnostic {
inline std::mutex fixture_mutex;
inline std::queue<rex::input::X_INPUT_KEYSTROKE> fixture_keys;
inline bool fixture_wheel_mode=false;
inline uint8_t InitialDevices() {
  const char* value=std::getenv("FM4_WHEEL_DIAGNOSTIC_DEVICES");
  if(!value) return 3;
  char* end=nullptr;
  const auto mask=std::strtoul(value,&end,10);
  return end!=value&&!*end&&mask<=3?uint8_t(mask):0;
}
inline uint8_t fixture_devices=0xff;
inline void QueueButtonChanges(uint16_t previous, uint16_t current) {
  constexpr uint16_t masks[]{0x1000,0x2000,0x4000,0x8000,0x0100,0x0200,
                             0x0001,0x0002,0x0004,0x0008,0x0010,0x0020,0x0040,0x0080};
  constexpr uint16_t codes[]{0x5800,0x5801,0x5802,0x5803,0x5805,0x5804,
                             0x5810,0x5811,0x5812,0x5813,0x5814,0x5815,0x5816,0x5817};
  for (size_t i = 0; i < std::size(masks); ++i) {
    if (!((current ^ previous) & masks[i])) continue;
    rex::input::X_INPUT_KEYSTROKE key{};
    key.virtual_key = codes[i]; key.flags = (current & masks[i]) ? 1 : 2;
    // Keep releases even if the game only asks for keystrokes on a pressed
    // state. Otherwise repeated presses of the same button lose their edge.
    if (fixture_keys.size() < 128) fixture_keys.push(key);
  }
}
inline bool Enabled() {
  static const bool enabled = std::getenv("FM4_BACKGROUND") && std::getenv("FM4_WHEEL_DIAGNOSTIC_INPUT");
  return enabled;
}
inline rex::input::X_INPUT_STATE ReadState(bool expose_release=true,bool* wheel_mode=nullptr,uint8_t* devices=nullptr) {
  std::lock_guard lock(fixture_mutex);
  if(fixture_devices==0xff) fixture_devices=InitialDevices();
  static auto last_read = std::chrono::steady_clock::time_point{};
  static rex::input::X_INPUT_STATE cached{};
  static uint32_t state_packet = 0, last_command = 0;
  static uint64_t deadline = 0;
  static bool release_pending = false;
  static uint32_t release_packet = 0;
  const auto now = std::chrono::steady_clock::now();
  const auto wall = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  uint64_t until=0; uint32_t packet=0, buttons=0, lt=0, rt=0; int32_t lx=0, ly=0, rx=0, ry=0;
  bool valid=false; uint32_t mode=0,attached=fixture_devices;
  if (now - last_read >= std::chrono::milliseconds(10)) {
    last_read = now;
    std::ifstream input(std::getenv("FM4_WHEEL_DIAGNOSTIC_INPUT"));
    valid = bool(input >> until >> packet >> buttons >> lt >> rt >> lx >> ly >> rx >> ry) &&
        until >= wall && until <= wall + 2000 && buttons <= 65535 && lt <= 255 && rt <= 255 &&
        lx >= -32768 && lx <= 32767 && ly >= -32768 && ly <= 32767 &&
        rx >= -32768 && rx <= 32767 && ry >= -32768 && ry <= 32767;
    // Optional fields: mode, then connected-device mask (pad=1, wheel=2).
    // Attachment persists after input expires, matching an idle plugged device.
    if(valid) {
      input>>std::ws;
      if(!input.eof()) {
        if(!(input>>mode)) valid=false;
        input>>std::ws;
        if(!input.eof()) { if(!(input>>attached)) valid=false;input>>std::ws; }
      }
      if(!input.eof()||mode>1||attached>3||
          (mode==1&&(rx < -1||rx>8||ly<0||ry<0))) valid=false;
      const bool nonzero=buttons||lt||rt||lx||ly||rx||ry;
      if(nonzero&&!(attached&(mode==1?2:1))) valid=false;
    }
  }
  // A new tap after expiry needs the old release even if no poll observed it.
  // A fresh renewal of the same command remains one continuous held input.
  if (wall > deadline && deadline && !(valid && packet == last_command)) {
    release_pending = release_pending || cached.gamepad.buttons != 0;
    QueueButtonChanges(cached.gamepad.buttons, 0);
    deadline = 0; cached = {}; cached.packet_number = ++state_packet;
    release_packet = state_packet;
  }
  if (valid) {
    fixture_wheel_mode=mode==1;
    fixture_devices=uint8_t(attached);
    const bool changed = packet != last_command || uint16_t(cached.gamepad.buttons) != buttons ||
        cached.gamepad.left_trigger != lt || cached.gamepad.right_trigger != rt ||
        int16_t(cached.gamepad.thumb_lx) != lx || int16_t(cached.gamepad.thumb_ly) != ly ||
        int16_t(cached.gamepad.thumb_rx) != rx || int16_t(cached.gamepad.thumb_ry) != ry;
    if (packet != last_command) {
      last_command = packet;
      REXLOG_INFO("FM4 hidden controller fixture: command={} buttons={:04X} lt={} rt={}",
                  packet, buttons, lt, rt);
    }
    if (changed) ++state_packet;
    QueueButtonChanges(cached.gamepad.buttons, uint16_t(buttons));
    cached = {}; deadline = until; cached.packet_number = state_packet;
    cached.gamepad.buttons = uint16_t(buttons);
    cached.gamepad.left_trigger = uint8_t(lt); cached.gamepad.right_trigger = uint8_t(rt);
    cached.gamepad.thumb_lx = int16_t(lx); cached.gamepad.thumb_ly = int16_t(ly);
    cached.gamepad.thumb_rx = int16_t(rx); cached.gamepad.thumb_ry = int16_t(ry);
  }
  // State-based menus also need to observe the release, even if a keystroke
  // poll already read the new command. Present one neutral sample first.
  if(wheel_mode) *wheel_mode=fixture_wheel_mode;
  if(devices) *devices=fixture_devices;
  if (expose_release && release_pending) {
    release_pending = false;
    rex::input::X_INPUT_STATE released{}; released.packet_number = release_packet;
    return released;
  }
  return cached;
}
inline bool ReadKeystroke(rex::input::X_INPUT_KEYSTROKE* output) {
  ReadState(false);
  std::lock_guard lock(fixture_mutex);
  if (fixture_keys.empty()) return false;
  if (output) *output = fixture_keys.front();
  fixture_keys.pop(); return true;
}
}  // namespace fm4::wheel_diagnostic
