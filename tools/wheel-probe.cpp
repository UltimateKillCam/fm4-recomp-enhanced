// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
// Read-only DirectInput inventory. This program never creates or plays effects.
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dinput.h>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>

static unsigned watch_seconds = 0;
static HWND probe_window = nullptr;
static void Pump() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message); DispatchMessageW(&message);
  }
}

struct ProbeAxis { GUID guid; DWORD id, offset; std::wstring name; bool actuator; };
struct ProbeState {
  LONG axes[32]{};
  DWORD pov[4]{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  BYTE buttons[128]{};
};
struct ProbeObjects {
  std::vector<ProbeAxis> axes;
  std::vector<DIOBJECTDATAFORMAT> other;
  unsigned buttons = 0, povs = 0;
};
struct WatchedDevice { IDirectInputDevice8W* handle; ProbeObjects objects; ProbeState previous; };
static std::vector<WatchedDevice> watched;

static BOOL CALLBACK Effect(const DIEFFECTINFOW* effect, void*) {
  std::wprintf(L"    effect: %ls\n", effect->tszName);
  return DIENUM_CONTINUE;
}
static BOOL CALLBACK Object(const DIDEVICEOBJECTINSTANCEW* object, void* context) {
  auto& objects = *static_cast<ProbeObjects*>(context);
  if ((object->dwType & DIDFT_AXIS) && objects.axes.size() < 32)
    objects.axes.push_back({object->guidType, object->dwType,
      DWORD(objects.axes.size() * sizeof(LONG)), object->tszName,
      bool(object->dwFlags & DIDOI_FFACTUATOR)});
  else if ((object->dwType & DIDFT_BUTTON) && objects.buttons < 128)
    objects.other.push_back({nullptr, DWORD(offsetof(ProbeState, buttons) + objects.buttons++), object->dwType, 0});
  else if ((object->dwType & DIDFT_POV) && objects.povs < 4)
    objects.other.push_back({nullptr, DWORD(offsetof(ProbeState, pov) + objects.povs++ * sizeof(DWORD)), object->dwType, 0});
  return DIENUM_CONTINUE;
}
static BOOL CALLBACK Device(const DIDEVICEINSTANCEW* info, void* context) {
  auto* input = static_cast<IDirectInput8W*>(context);
  wchar_t guid[40]{};
  StringFromGUID2(info->guidInstance, guid, 40);
  std::wprintf(L"device: %ls\n  instance: %ls\n  product: %08lx\n  type: %08lx\n",
               info->tszProductName, guid, info->guidProduct.Data1, info->dwDevType);
  IDirectInputDevice8W* device = nullptr;
  if (FAILED(input->CreateDevice(info->guidInstance, &device, nullptr))) return DIENUM_CONTINUE;
  ProbeObjects objects;
  device->EnumObjects(Object, &objects, DIDFT_ALL);
  auto format_objects = objects.other;
  for (auto& axis : objects.axes) {
    format_objects.push_back({&axis.guid, axis.offset, axis.id, DIDOI_ASPECTPOSITION});
    DIPROPRANGE range{};
    range.diph = {sizeof(range), sizeof(DIPROPHEADER), axis.id, DIPH_BYID};
    range.lMin = 0; range.lMax = 65535;
    device->SetProperty(DIPROP_RANGE, &range.diph);
  }
  DIDATAFORMAT format{sizeof(format), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS,
    sizeof(ProbeState), DWORD(format_objects.size()), format_objects.data()};
  device->SetDataFormat(&format);
  const auto cooperative = device->SetCooperativeLevel(probe_window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
  if (FAILED(cooperative)) {
    std::wprintf(L"  cooperative level failed: %08lx\n", cooperative);
    device->Release(); return DIENUM_CONTINUE;
  }
  DIDEVCAPS caps{};
  caps.dwSize = sizeof(caps);
  if (SUCCEEDED(device->GetCapabilities(&caps)))
    std::wprintf(L"  axes=%lu buttons=%lu povs=%lu ffb=%ls\n", caps.dwAxes, caps.dwButtons,
                 caps.dwPOVs, (caps.dwFlags & DIDC_FORCEFEEDBACK) ? L"yes" : L"no");
  device->EnumEffects(Effect, nullptr, DIEFT_ALL);
  if (SUCCEEDED(device->Acquire())) {
    for (int i = 0; i < 20; ++i) { Pump(); Sleep(10); }
    device->Poll();
    ProbeState state;
    if (SUCCEEDED(device->GetDeviceState(sizeof(state), &state)))
      for (size_t i = 0; i < objects.axes.size(); ++i)
        std::wprintf(L"    #%zu %ls = %ld (actuator=%ls)\n", i, objects.axes[i].name.c_str(),
                     state.axes[i], objects.axes[i].actuator ? L"yes" : L"no");
    if (watch_seconds) {
      watched.push_back({device, std::move(objects)});
      return DIENUM_CONTINUE;
    }
    device->Unacquire();
    auto formatResult = device->SetDataFormat(&c_dfDIJoystick2);
    if (SUCCEEDED(formatResult) && SUCCEEDED(device->Acquire())) {
      Sleep(100); device->Poll(); DIJOYSTATE2 standard{};
      auto readResult = device->GetDeviceState(sizeof(standard), &standard);
      std::wprintf(L"  standard hr=%08lx: %ld %ld %ld %ld %ld %ld %ld %ld\n", readResult,
        standard.lX, standard.lY, standard.lZ, standard.lRx, standard.lRy, standard.lRz,
        standard.rglSlider[0], standard.rglSlider[1]);
      device->Unacquire();
    }
  }
  device->Release();
  return DIENUM_CONTINUE;
}
int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--watch") {
    char* end = nullptr;
    const auto seconds = std::strtol(argv[2], &end, 10);
    if (!end || *end || seconds < 1 || seconds > 600) {
      std::fputs("Watch duration must be 1..600 seconds.\n", stderr); return 2;
    }
    watch_seconds = unsigned(seconds);
  } else if (argc != 1) {
    std::fputs("Usage: wheel-probe [--watch seconds]\n", stderr); return 2;
  }
  IDirectInput8W* input = nullptr;
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"Fm4ReadOnlyWheelProbe";
  RegisterClassW(&window_class);
  probe_window = CreateWindowExW(0, window_class.lpszClassName, L"FM4 wheel probe", 0,
      0, 0, 0, 0, nullptr, nullptr, window_class.hInstance, nullptr);
  auto result = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
      IID_IDirectInput8W, reinterpret_cast<void**>(&input), nullptr);
  if (FAILED(result)) return 1;
  input->EnumDevices(DI8DEVCLASS_GAMECTRL, Device, input, DIEDFL_ATTACHEDONLY);
  if (watch_seconds) {
    std::wprintf(L"CAPTURE READY: %zu Fanatec interfaces, %u seconds\n", watched.size(), watch_seconds);
    std::fflush(stdout);
    const auto end = GetTickCount64() + watch_seconds * 1000;
    while (GetTickCount64() < end) {
      Pump();
      for (size_t index = 0; index < watched.size(); ++index) {
        auto& item = watched[index]; ProbeState state;
        item.handle->Poll();
        if (SUCCEEDED(item.handle->GetDeviceState(sizeof(state), &state))) {
          bool changed = false;
          for (size_t i = 0; i < item.objects.axes.size(); ++i) if (state.axes[i] != item.previous.axes[i]) changed = true;
          if (std::memcmp(state.buttons, item.previous.buttons, sizeof(state.buttons)) ||
              std::memcmp(state.pov, item.previous.pov, sizeof(state.pov))) changed = true;
          if (changed) {
            std::wprintf(L"live device=%zu:", index);
            for (size_t i = 0; i < item.objects.axes.size(); ++i) std::wprintf(L" %zu=%ld", i, state.axes[i]);
            std::wprintf(L" buttons=");
            for (unsigned i = 0; i < item.objects.buttons; ++i)
              if (state.buttons[i] & 0x80) std::wprintf(L"%u,", i + 1);
            for (unsigned i = 0; i < item.objects.povs; ++i)
              if (LOWORD(state.pov[i]) != 0xFFFF) std::wprintf(L" pov%u=%lu", i, state.pov[i]);
            std::wprintf(L"\n"); std::fflush(stdout);
            item.previous = state;
          }
        }
      }
      Sleep(40);
    }
  }
  for (auto& item : watched) { item.handle->Unacquire(); item.handle->Release(); }
  input->Release();
  DestroyWindow(probe_window);
}
