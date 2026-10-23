// Wheel/controller integration. Modified 2026-10-20. GPL-3.0-only.
// fm4-input: read-only DirectInput reader for the launcher's control binding page.
// It enumerates game controllers exactly like wheel_host.cpp (same object order, so
// "#N" axis indexes match the game, and the same 0..65535 axis range) and streams
// their state as JSON lines. It never creates or plays force-feedback effects and
// never acquires a device exclusively.
//
//   fm4-input --list            print the device inventory once
//   fm4-input [--seconds N]     stream inventory and state changes until stdin closes
//
// Output lines:
//   {"type":"devices","devices":[{"guid","name","driving","ffb","axes":[{"name","type","actuator"}],"buttons","povs"}]}
//   {"type":"state","guid":"{...}","axes":[...],"buttons":[1,5],"povs":[...]}
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct RawState {
  LONG axes[32]{};
  DWORD pov[4]{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  BYTE buttons[128]{};
};
struct Axis { GUID guid; DWORD id, offset; std::string name; bool actuator; };

std::string Utf8(const wchar_t* value) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size < 1) return {};
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
  result.pop_back();
  return result;
}
std::string Guid(const GUID& guid) {
  wchar_t text[40]{};
  StringFromGUID2(guid, text, 40);
  return Utf8(text);
}
const char* AxisType(const GUID& type) {
  if (IsEqualGUID(type, GUID_XAxis)) return "X";
  if (IsEqualGUID(type, GUID_YAxis)) return "Y";
  if (IsEqualGUID(type, GUID_ZAxis)) return "Z";
  if (IsEqualGUID(type, GUID_RxAxis)) return "RX";
  if (IsEqualGUID(type, GUID_RyAxis)) return "RY";
  if (IsEqualGUID(type, GUID_RzAxis)) return "RZ";
  if (IsEqualGUID(type, GUID_Slider)) return "Slider";
  return "Other";
}
std::string Json(const std::string& text) {
  std::string out = "\"";
  for (const unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 0x20) { char buffer[8]; std::snprintf(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
    else out += char(c);
  }
  return out + '"';
}

struct Device {
  IDirectInputDevice8W* handle = nullptr;
  std::string guid, name;
  bool driving = false, ffb = false;
  std::vector<Axis> axes;
  std::vector<DIOBJECTDATAFORMAT> objects;
  unsigned buttons = 0, povs = 0;
  RawState previous;
  bool reported = false;
  ~Device() { if (handle) { handle->Unacquire(); handle->Release(); } }
  // Same object walk as wheel_host.cpp Device::Object.
  static BOOL CALLBACK Object(const DIDEVICEOBJECTINSTANCEW* object, void* context) {
    auto& self = *static_cast<Device*>(context);
    if ((object->dwType & DIDFT_AXIS) && self.axes.size() < 32) {
      const DWORD offset = DWORD(self.axes.size() * sizeof(LONG));
      self.axes.push_back({object->guidType, object->dwType, offset, Utf8(object->tszName),
                           bool(object->dwFlags & DIDOI_FFACTUATOR)});
    } else if ((object->dwType & DIDFT_BUTTON) && self.buttons < 128) {
      self.objects.push_back({nullptr, DWORD(offsetof(RawState, buttons) + self.buttons++), object->dwType, 0});
    } else if ((object->dwType & DIDFT_POV) && self.povs < 4) {
      self.objects.push_back({nullptr, DWORD(offsetof(RawState, pov) + self.povs++ * sizeof(DWORD)), object->dwType, 0});
    }
    return DIENUM_CONTINUE;
  }
  bool Open(IDirectInput8W* input, const DIDEVICEINSTANCEW& info, HWND window) {
    if (FAILED(input->CreateDevice(info.guidInstance, &handle, nullptr))) return false;
    guid = Guid(info.guidInstance); name = Utf8(info.tszProductName);
    driving = GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_DRIVING;
    DIDEVCAPS caps{}; caps.dwSize = sizeof(caps);
    if (FAILED(handle->GetCapabilities(&caps))) return false;
    ffb = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
    handle->EnumObjects(Object, this, DIDFT_ALL);
    for (auto& axis : axes) {
      objects.push_back({&axis.guid, axis.offset, axis.id, DIDOI_ASPECTPOSITION});
      DIPROPRANGE range{};
      range.diph = {sizeof(range), sizeof(DIPROPHEADER), axis.id, DIPH_BYID};
      range.lMin = 0; range.lMax = 65535;
      handle->SetProperty(DIPROP_RANGE, &range.diph);
      DIPROPDWORD deadzone{};
      deadzone.diph = {sizeof(deadzone), sizeof(DIPROPHEADER), axis.id, DIPH_BYID};
      deadzone.dwData = 0;
      handle->SetProperty(DIPROP_DEADZONE, &deadzone.diph);
    }
    DIDATAFORMAT format{sizeof(format), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS,
                        sizeof(RawState), DWORD(objects.size()), objects.data()};
    if (FAILED(handle->SetDataFormat(&format))) return false;
    // Background, non-exclusive: never interferes with the game's exclusive
    // force-feedback acquisition and never takes the wheel from another app.
    return SUCCEEDED(handle->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE));
  }
  bool Read(RawState& state) {
    handle->Poll();
    if (FAILED(handle->GetDeviceState(sizeof(state), &state))) {
      if (FAILED(handle->Acquire())) return false;
      handle->Poll();
      if (FAILED(handle->GetDeviceState(sizeof(state), &state))) return false;
    }
    return true;
  }
};

struct Inventory {
  IDirectInput8W* input = nullptr;
  HWND window = nullptr;
  std::vector<std::unique_ptr<Device>> devices;
  std::vector<std::string> ids;
  static BOOL CALLBACK Enumerate(const DIDEVICEINSTANCEW* info, void* context) {
    auto& self = *static_cast<Inventory*>(context);
    auto device = std::make_unique<Device>();
    if (device->Open(self.input, *info, self.window)) self.devices.push_back(std::move(device));
    return DIENUM_CONTINUE;
  }
  static BOOL CALLBACK Ids(const DIDEVICEINSTANCEW* info, void* context) {
    static_cast<std::vector<std::string>*>(context)->push_back(Guid(info->guidInstance));
    return DIENUM_CONTINUE;
  }
  std::vector<std::string> Attached() {
    std::vector<std::string> attached;
    input->EnumDevices(DI8DEVCLASS_GAMECTRL, Ids, &attached, DIEDFL_ATTACHEDONLY);
    std::sort(attached.begin(), attached.end());
    return attached;
  }
  void Discover() {
    devices.clear();
    input->EnumDevices(DI8DEVCLASS_GAMECTRL, Enumerate, this, DIEDFL_ATTACHEDONLY);
    ids = Attached();
  }
  void PrintDevices() {
    std::string line = "{\"type\":\"devices\",\"devices\":[";
    for (size_t d = 0; d < devices.size(); ++d) {
      const auto& device = *devices[d];
      if (d) line += ',';
      line += "{\"guid\":" + Json(device.guid) + ",\"name\":" + Json(device.name) +
          ",\"driving\":" + (device.driving ? "true" : "false") +
          ",\"ffb\":" + (device.ffb ? "true" : "false") + ",\"axes\":[";
      for (size_t a = 0; a < device.axes.size(); ++a) {
        if (a) line += ',';
        line += "{\"name\":" + Json(device.axes[a].name) + ",\"type\":\"" + AxisType(device.axes[a].guid) +
            "\",\"actuator\":" +
            (device.axes[a].actuator ? "true" : "false") + '}';
      }
      line += "],\"buttons\":" + std::to_string(device.buttons) + ",\"povs\":" + std::to_string(device.povs) + '}';
    }
    std::puts((line + "]}").c_str());
    std::fflush(stdout);
  }
  void PrintChanges(bool all) {
    for (auto& item : devices) {
      RawState state;
      if (!item->Read(state)) continue;
      bool changed = all || !item->reported;
      for (size_t i = 0; i < item->axes.size() && !changed; ++i)
        if (std::abs(state.axes[i] - item->previous.axes[i]) > 256) changed = true;
      if (!changed && (std::memcmp(state.buttons, item->previous.buttons, item->buttons) ||
          std::memcmp(state.pov, item->previous.pov, sizeof(state.pov)))) changed = true;
      if (!changed) continue;
      std::string line = "{\"type\":\"state\",\"guid\":" + Json(item->guid) + ",\"axes\":[";
      for (size_t i = 0; i < item->axes.size(); ++i) {
        if (i) line += ',';
        line += std::to_string(state.axes[i]);
      }
      line += "],\"buttons\":[";
      bool first = true;
      for (unsigned i = 0; i < item->buttons; ++i) if (state.buttons[i] & 0x80) {
        if (!first) line += ',';
        line += std::to_string(i + 1); first = false;
      }
      line += "],\"povs\":[";
      for (unsigned i = 0; i < item->povs; ++i) {
        if (i) line += ',';
        line += LOWORD(state.pov[i]) == 0xFFFF ? "-1" : std::to_string(state.pov[i]);
      }
      std::puts((line + "]}").c_str());
      item->previous = state; item->reported = true;
    }
    std::fflush(stdout);
  }
};

void Pump() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}

}  // namespace

int main(int argc, char** argv) {
  bool list = false;
  unsigned seconds = 1800;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--list") list = true;
    else if (argument == "--seconds" && i + 1 < argc) {
      char* end = nullptr;
      const long value = std::strtol(argv[++i], &end, 10);
      if (!end || *end || value < 1 || value > 7200) { std::fputs("--seconds must be 1..7200\n", stderr); return 2; }
      seconds = unsigned(value);
    } else { std::fputs("Usage: fm4-input [--list] [--seconds N]\n", stderr); return 2; }
  }
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"Fm4InputCapture";
  RegisterClassW(&window_class);
  Inventory inventory;
  inventory.window = CreateWindowExW(0, window_class.lpszClassName, L"FM4 input capture", 0, 0, 0, 0, 0,
                                     nullptr, nullptr, window_class.hInstance, nullptr);
  if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                reinterpret_cast<void**>(&inventory.input), nullptr))) {
    std::fputs("DirectInput8Create failed\n", stderr); return 1;
  }
  inventory.Discover();
  inventory.PrintDevices();
  if (list) { inventory.devices.clear(); inventory.input->Release(); return 0; }
  // The launcher owns this process; end when it closes our stdin or exits.
  std::atomic<bool> stop{false};
  std::thread watcher([&] {
    char buffer[64];
    while (std::fgets(buffer, sizeof(buffer), stdin)) {}
    stop = true;
  });
  watcher.detach();
  const auto end = GetTickCount64() + uint64_t(seconds) * 1000;
  uint64_t next_inventory = GetTickCount64() + 2000;
  bool all = true;
  while (!stop && GetTickCount64() < end) {
    Pump();
    if (GetTickCount64() >= next_inventory) {
      next_inventory = GetTickCount64() + 2000;
      if (inventory.Attached() != inventory.ids) { inventory.Discover(); inventory.PrintDevices(); all = true; }
    }
    inventory.PrintChanges(all);
    all = false;
    Sleep(15);
  }
  inventory.devices.clear();
  inventory.input->Release();
  return 0;
}
