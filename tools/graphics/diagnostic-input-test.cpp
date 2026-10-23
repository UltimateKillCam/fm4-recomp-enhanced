#include "diagnostic_input.h"
#include <rex/input/input_system.h>
#include <filesystem>
#include <thread>
#include <cstdio>
#include <stdexcept>
#include <windows.h>
int main() {
  auto file = std::filesystem::temp_directory_path() /
      ("fm4-input-" + std::to_string(GetCurrentProcessId()) + ".txt");
  _putenv_s("FM4_BACKGROUND", "1");
  _putenv_s("FM4_DIAGNOSTIC_INPUT", file.string().c_str());
  auto command = [&](uint32_t id) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ofstream(file) << now + 60 << ' ' << id << " 4096 0 0 0 0 0 0\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  };
  command(1);
  if (fm4::diagnostic::ReadState().gamepad.buttons != 4096) return 1;
  // The game may poll state through a transition without reading keystrokes.
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  if (fm4::diagnostic::ReadState().gamepad.buttons != 0) return 2;
  command(2);
  if (fm4::diagnostic::ReadState().gamepad.buttons != 4096) return 3;
  for (uint16_t expected : {1, 2, 1}) {
    rex::input::X_INPUT_KEYSTROKE key{};
    if (!fm4::diagnostic::ReadKeystroke(&key) || key.virtual_key != 0x5800 ||
        key.flags != expected) return 4;
  }
  rex::input::X_INPUT_KEYSTROKE key{};
  if (fm4::diagnostic::ReadKeystroke(&key)) return 5;
  // A sparse-polling menu sees only the new command: it must still receive
  // the expired tap's release before the next down event.
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  fm4::diagnostic::ReadState();
  if (!fm4::diagnostic::ReadKeystroke(&key) || key.flags != 2) return 10;
  command(3); fm4::diagnostic::ReadState();
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  command(4);
  if (fm4::diagnostic::ReadState().gamepad.buttons != 0) return 12;
  if (fm4::diagnostic::ReadState().gamepad.buttons != 4096) return 13;
  for (uint16_t expected : {1, 2, 1}) {
    if (!fm4::diagnostic::ReadKeystroke(&key) || key.virtual_key != 0x5800 || key.flags != expected) return 8;
  }
  if (fm4::diagnostic::ReadKeystroke(&key)) return 9;
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  command(4); // renew the same continuous command after a sparse poll
  if (fm4::diagnostic::ReadState().gamepad.buttons != 4096 || fm4::diagnostic::ReadKeystroke(&key)) return 11;
  command(5); // give the DLL entry point a fresh bounded command
  // Exercise the real DLL entry point, including XUSER_INDEX_ANY used by menus.
  // This has separate fixture storage from this executable's header-only test.
  rex::input::InputSystem input(nullptr);
  if (input.GetKeystroke(0xFF, 0, &key) != 0 ||
      key.virtual_key != 0x5800 || key.flags != 1 || key.user_index != 0) return 6;
  if (input.GetKeystroke(1, 0, &key) == 0) return 7;
  // Exercise sparse state polling through the actual runtime DLL as well.
  rex::input::X_INPUT_STATE state{};
  if (input.GetState(0,&state) != 0 || state.gamepad.buttons != 4096) return 14;
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  command(6);
  if (input.GetKeystroke(0xFF,0,&key) != 0 || key.flags != 2) return 15;
  if (input.GetState(0,&state) != 0 || state.gamepad.buttons != 0) return 16;
  if (input.GetState(0,&state) != 0 || state.gamepad.buttons != 4096) return 17;
  std::filesystem::remove(file);
  std::puts("PASS: repeated presses retain down/up/down while keystroke polling is delayed");
  return 0;
}
