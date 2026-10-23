// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#include "wheel_host.h"

#define DIRECTINPUT_VERSION 0x0800
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dinput.h>
#include "wheel_native_ffb.h"
#include "wheel_wait.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fm4::wheel {
namespace {

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
bool Equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}
uint64_t Now() { return GetTickCount64(); }

struct RawState {
  LONG axes[32]{};
  DWORD pov[4]{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  BYTE buttons[128]{};
};
struct Axis { GUID guid; DWORD id; DWORD offset; std::string name; bool actuator; };

struct Device {
  IDirectInputDevice8W* handle = nullptr;
  std::string guid, name;
  uint16_t vendor = 0;
  bool driving = false, ffb = false, readable = false, received_report = false;
  uint64_t acquisition = 0;
  RawState raw;
  std::array<bool, 5> effect_support{};
  std::vector<Axis> axes;
  std::vector<DIOBJECTDATAFORMAT> objects;
  unsigned buttons = 0, povs = 0;
  ~Device() { if (handle) { handle->Unacquire(); handle->Release(); } }
  static BOOL CALLBACK EffectInfo(const DIEFFECTINFOW* info, void* context) {
    auto& self = *static_cast<Device*>(context);
    const GUID* types[] = {&GUID_Inertia, &GUID_Friction, &GUID_Damper, &GUID_Spring, &GUID_ConstantForce};
    for (size_t i = 0; i < self.effect_support.size(); ++i)
      if (IsEqualGUID(info->guid, *types[i])) self.effect_support[i] = true;
    return DIENUM_CONTINUE;
  }
  static BOOL CALLBACK Object(const DIDEVICEOBJECTINSTANCEW* object, void* context) {
    auto& self = *static_cast<Device*>(context);
    if ((object->dwType & DIDFT_AXIS) && self.axes.size() < 32) {
      const DWORD offset = DWORD(self.axes.size() * sizeof(LONG));
      self.axes.push_back({object->guidType, object->dwType, offset,
                           Utf8(object->tszName), bool(object->dwFlags & DIDOI_FFACTUATOR)});
    } else if ((object->dwType & DIDFT_BUTTON) && self.buttons < 128) {
      self.objects.push_back({nullptr, DWORD(offsetof(RawState, buttons) + self.buttons++), object->dwType, 0});
    } else if ((object->dwType & DIDFT_POV) && self.povs < 4) {
      self.objects.push_back({nullptr, DWORD(offsetof(RawState, pov) + self.povs++ * sizeof(DWORD)), object->dwType, 0});
    }
    return DIENUM_CONTINUE;
  }
  bool Open(IDirectInput8W* input, const DIDEVICEINSTANCEW& info) {
    if (FAILED(input->CreateDevice(info.guidInstance, &handle, nullptr))) return false;
    guid = Guid(info.guidInstance); name = Utf8(info.tszProductName);
    vendor = LOWORD(info.guidProduct.Data1);
    driving = GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_DRIVING;
    DIDEVCAPS caps{}; caps.dwSize = sizeof(caps);
    if (FAILED(handle->GetCapabilities(&caps))) return false;
    ffb = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
    handle->EnumObjects(Object, this, DIDFT_ALL);
    if (ffb) handle->EnumEffects(EffectInfo, this, DIEFT_ALL);
    // Explicit object IDs support Fanatec's extra collections and >8 axes.
    // Native driver offsets are never assumed to be DIJOYSTATE2 offsets.
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
    return SUCCEEDED(handle->SetDataFormat(&format));
  }
  int AxisIndex(std::string_view name) const {
    if (Equal(name, "disabled")) return -1;
    for (size_t i = 0; i < axes.size(); ++i) if (Equal(axes[i].name, name)) return int(i);
    if (Equal(name, "Wheel Axis"))
      for (size_t i = 0; i < axes.size(); ++i) if (axes[i].actuator) return int(i);
    // Stable HID semantics work with localized Logitech, Thrustmaster, MOZA,
    // Simucube, Simagic and other Windows DirectInput drivers. Driver names and
    // explicit numeric indexes still take precedence above / below.
    const GUID* semantic = nullptr;
    if (Equal(name, "X") || Equal(name, "Wheel Axis")) semantic = &GUID_XAxis;
    else if (Equal(name, "Y")) semantic = &GUID_YAxis;
    else if (Equal(name, "Z")) semantic = &GUID_ZAxis;
    else if (Equal(name, "RX")) semantic = &GUID_RxAxis;
    else if (Equal(name, "RY")) semantic = &GUID_RyAxis;
    else if (Equal(name, "RZ")) semantic = &GUID_RzAxis;
    if (semantic)
      for (size_t i = 0; i < axes.size(); ++i) if (IsEqualGUID(axes[i].guid, *semantic)) return int(i);
    if (Equal(name, "Slider0") || Equal(name, "Slider1")) {
      unsigned slider = Equal(name, "Slider1") ? 1 : 0;
      for (size_t i = 0; i < axes.size(); ++i)
        if (IsEqualGUID(axes[i].guid, GUID_Slider) && slider-- == 0) return int(i);
    }
    // Numeric index is explicit and is useful for localized driver axis names.
    if (!name.empty() && name.front() == '#') {
      try { const int i = std::stoi(std::string(name.substr(1))); return i >= 0 && i < int(axes.size()) ? i : -1; }
      catch (...) { return -1; }
    }
    return -1;
  }
};

}  // namespace

Config LoadConfig(const std::filesystem::path& path) {
  Config config;
  if (!std::filesystem::exists(path)) return config;
  auto read = [&](const std::string& section, const std::string& key, const std::string& fallback) {
    wchar_t value[1024]{};
    auto wide = [](const std::string& text) { return std::wstring(text.begin(), text.end()); };
    GetPrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(fallback).c_str(),
                              value, 1024, std::filesystem::absolute(path).c_str());
    return Utf8(value);
  };
  auto number = [&](const std::string& section, const std::string& key, float fallback) {
    const auto text = read(section, key, std::to_string(fallback));
    size_t consumed = 0;
    const float result = std::stof(text, &consumed);
    if (consumed != text.size() || !std::isfinite(result)) throw std::runtime_error("Invalid wheel setting: " + section + "." + key);
    return result;
  };
  config.enabled = number("wheel", "enabled", 1) != 0;
  config.device = read("wheel", "device", "");
  const float user = number("wheel", "user", 0);
  if (user < 0 || user > 3 || user != std::floor(user)) throw std::runtime_error("wheel.user must be 0..3");
  config.user = uint32_t(user);
  config.rotation_scale = number("wheel", "rotation_scale", 1);
  config.rotation_degrees = number("wheel", "rotation_degrees", 900);
  config.full_rotation = number("wheel", "full_rotation", 1) != 0;
  if (config.rotation_degrees < 90 || config.rotation_degrees > 3600)
    throw std::runtime_error("wheel.rotation_degrees must be in 90..3600");
  config.gain = number("force", "gain", 0.4f);
  config.invert_force = number("force", "invert", 0) != 0;
  config.detail_gain = number("force", "detail", 0.5f);
  if (config.detail_gain < 0 || config.detail_gain > 1) throw std::runtime_error("force.detail must be in 0..1");
  config.engine_gain = number("force", "engine", .1f);
  if (config.engine_gain < 0 || config.engine_gain > 1) throw std::runtime_error("force.engine must be in 0..1");
  config.road_gain = number("force", "road", .3f);
  if (config.road_gain < 0 || config.road_gain > 1) throw std::runtime_error("force.road must be in 0..1");
  const auto mode = read("force", "mode", "telemetry");
  if (mode != "game" && mode != "telemetry") throw std::runtime_error("force.mode must be game or telemetry");
  config.telemetry_force = mode == "telemetry";
  config.shape.steering_weight = number("force", "steering_weight", 3.5f);
  config.shape.centering = number("force", "centering", 0);
  config.shape.damping = number("force", "damping", .25f);
  if (config.shape.steering_weight < 1 || config.shape.steering_weight > 5)
    throw std::runtime_error("force.steering_weight must be in 1..5");
  if (config.shape.centering < 0 || config.shape.centering > 1 || config.shape.damping < 0 || config.shape.damping > 2)
    throw std::runtime_error("force.centering must be in 0..1 and force.damping in 0..2");
  config.ffb_enabled = number("force", "enabled", 1) != 0;
  config.force_backend = read("force", "backend", "auto");
  if (config.force_backend != "auto" && config.force_backend != "native" && config.force_backend != "software")
    throw std::runtime_error("force.backend must be auto, native or software");
  config.velocity_scale = number("force", "velocity_scale", .1f);
  config.acceleration_scale = number("force", "acceleration_scale", .01f);
  if (config.velocity_scale <= 0 || config.velocity_scale > 10 ||
      config.acceleration_scale <= 0 || config.acceleration_scale > 10)
    throw std::runtime_error("Software condition scales must be in (0,10]");
  const float timeout = number("force", "timeout_ms", 150);
  if (config.gain < 0 || config.gain > 1 || config.rotation_scale <= 0 ||
      config.rotation_scale > 10 || timeout < 20 || timeout > 500)
    throw std::runtime_error("Wheel gain, rotation scale, or watchdog is outside its supported range");
  config.timeout_ms = uint32_t(timeout);
  config.rumble_gain = number("controller", "rumble_gain", 1);
  config.stick_deadzone = number("controller", "stick_deadzone", .06f);
  config.adaptive_triggers = number("controller", "adaptive_triggers", 1) != 0;
  config.adaptive_gain = number("controller", "adaptive_gain", .35f);
  config.touchpad_back = number("controller", "touchpad_back", 1) != 0;
  if (config.rumble_gain < 0 || config.rumble_gain > 1 || config.adaptive_gain < 0 || config.adaptive_gain > 1)
    throw std::runtime_error("Controller feedback gain must be in 0..1");
  if (config.stick_deadzone < 0 || config.stick_deadzone > .5f)
    throw std::runtime_error("Controller stick deadzone must be in 0..0.5");
  std::array<std::pair<const char*, AxisBinding*>, 5> bindings{{
      {"steering", &config.steering}, {"throttle", &config.throttle},
      {"brake", &config.brake}, {"clutch", &config.clutch}, {"handbrake", &config.handbrake}}};
  for (const auto& [name, binding] : bindings) {
    binding->device = read(name, "device", "");
    binding->axis = read(name, "axis", binding->axis);
    auto& calibration = binding->calibration;
    auto axis_integer = [&](const char* key, int fallback) {
      const float value = number(name, key, float(fallback));
      if (value < 0 || value > 65535 || value != std::floor(value))
        throw std::runtime_error("Wheel calibration endpoints must be integers in 0..65535");
      return int(value);
    };
    calibration.minimum = axis_integer("minimum", 0);
    calibration.center = axis_integer("center", 32767);
    calibration.maximum = axis_integer("maximum", 65535);
    calibration.deadzone = number(name, "deadzone", 0);
    calibration.saturation = number(name, "saturation", 1);
    calibration.gamma = number(name, "gamma", 1);
    calibration.inverted = number(name, "invert", 0) != 0;
    if (!calibration.Valid(binding == &config.steering)) throw std::runtime_error("Invalid calibration: " + std::string(name));
  }
  for (size_t bit = 0; bit < config.buttons.size(); ++bit) {
    const float button = number("buttons", "bit" + std::to_string(bit), float(config.buttons[bit]));
    if (button < 0 || button > 128 || button != std::floor(button)) throw std::runtime_error("Invalid wheel button binding");
    config.buttons[bit] = int(button);
    config.button_devices[bit] = read("buttons", "device" + std::to_string(bit), "");
  }
  config.shifter_enabled = number("shifter", "enabled", 0) != 0;
  config.shifter_device = read("shifter", "device", "");
  const float gears = number("shifter", "gears", 7);
  if (gears < 1 || gears > 8 || gears != std::floor(gears))
    throw std::runtime_error("shifter.gears must be 1..8");
  config.shifter_gears = unsigned(gears);
  for (size_t gear = 0; gear < config.gear_buttons.size(); ++gear) {
    const auto key = gear == 0 ? "reverse" : "gear" + std::to_string(gear);
    const float button = number("shifter", key, 0);
    if (button < 0 || button > 128 || button != std::floor(button))
      throw std::runtime_error("Invalid shifter button binding");
    config.gear_buttons[gear] = int(button);
  }
  return config;
}

void WriteDefaultConfig(const std::filesystem::path& path) {
  if (std::filesystem::exists(path)) return;
  std::ofstream out(path);
  out << "; FM4 wheel profile. Button numbers are one-based, zero disables.\n"
         "[wheel]\nenabled=1\ndevice=\nuser=0\nrotation_scale=1\nrotation_degrees=900\nfull_rotation=1\n\n"
         "[force]\nenabled=1\ngain=0.4\ninvert=0\ndetail=0.5\nengine=0.1\nroad=0.3\nmode=telemetry\nsteering_weight=2.5\ncentering=0\ndamping=0.25\ntimeout_ms=150\nbackend=auto\nvelocity_scale=0.1\nacceleration_scale=0.01\n\n"
         "[controller]\nrumble_gain=1\nstick_deadzone=0.06\nadaptive_triggers=1\nadaptive_gain=0.35\ntouchpad_back=1\n\n";
  for (const auto& [name, axis] : std::array<std::pair<const char*, const char*>, 5>{{
      {"steering", "Wheel Axis"}, {"throttle", "Accelerator"}, {"brake", "Brake"},
      {"clutch", "Clutch"}, {"handbrake", "disabled"}}}) {
    out << '[' << name << "]\ndevice=\naxis=" << axis <<
        "\nminimum=0\ncenter=32767\nmaximum=65535\ndeadzone=0\nsaturation=1\ngamma=1\ninvert=0\n\n";
  }
  out << "[buttons]\n";
  const Config defaults;
  for (size_t bit = 0; bit < defaults.buttons.size(); ++bit)
    out << "bit" << bit << '=' << defaults.buttons[bit] << '\n';
  out << "\n[shifter]\nenabled=0\ndevice=\ngears=7\nreverse=0\n";
  for (unsigned gear = 1; gear <= 8; ++gear) out << "gear" << gear << "=0\n";
  if (!out) throw std::runtime_error("Could not write wheel profile");
}

struct Host::Impl {
  Config config;
  mutable std::mutex mutex;
  State state;
  EffectFrame forces;
  uint64_t last_force = 0;
  std::atomic<HWND> window{nullptr};
  std::atomic<bool> active{false};
  std::atomic<float> live_steering{0};
  Telemetry telemetry;
  uint64_t last_telemetry = 0;
  TorqueSmoother telemetry_torque;
  FrameInterpolator road_frames;
  RoadTexture road_texture;
  RumbleDetail rumble_detail;
  uint64_t stopped_at = 0;  // When output stopped while effects were still downloaded.
  std::array<bool, 7> started{};
  std::jthread worker;
  IDirectInput8W* input = nullptr;
  std::vector<std::unique_ptr<Device>> devices;
  std::vector<std::string> discovered_ids;
  Device* wheel = nullptr;
  bool gain_set = false;
  uint64_t next_gain_retry = 0;
  // Five guest effects, then the low/high rumble vibrations.
  std::array<IDirectInputEffect*, 7> effects{};
  bool applying = false;
  bool actuators_ready = false;
  uint64_t force_acquisition = 0;
  uint64_t next_force_state_check = 0;
  bool software_forces = false;
  Motion motion;

  void ResetForceHistory() {
    telemetry_torque.Reset(); road_frames.Reset(); road_texture.Reset(); rumble_detail.Reset();
    last_telemetry = 0;
  }

  explicit Impl(Config value) : config(std::move(value)) {
    worker = std::jthread([this](std::stop_token stop) { Run(stop); });
  }
  ~Impl() { worker.request_stop(); if (worker.joinable()) worker.join(); }
  static BOOL CALLBACK Enumerate(const DIDEVICEINSTANCEW* info, void* context) {
    auto& self = *static_cast<Impl*>(context);
    self.discovered_ids.push_back(Guid(info->guidInstance));
    auto device = std::make_unique<Device>();
    if (device->Open(self.input, *info)) self.devices.push_back(std::move(device));
    return DIENUM_CONTINUE;
  }
  static BOOL CALLBACK Inventory(const DIDEVICEINSTANCEW* info, void* context) {
    static_cast<std::vector<std::string>*>(context)->push_back(Guid(info->guidInstance));
    return DIENUM_CONTINUE;
  }
  bool InventoryChanged() {
    std::vector<std::string> attached;
    if (FAILED(input->EnumDevices(DI8DEVCLASS_GAMECTRL, Inventory, &attached, DIEDFL_ATTACHEDONLY))) return false;
    std::sort(attached.begin(), attached.end());
    return attached != discovered_ids;
  }
  void ReleaseEffects() {
    for (auto*& effect : effects) if (effect) { effect->Stop(); effect->Release(); effect = nullptr; }
    started.fill(false); stopped_at = 0;
    applying = false; actuators_ready = false; next_force_state_check = 0;
    gain_set = false; next_gain_retry = 0;
    std::lock_guard lock(mutex); state.output_active = false; state.downloaded_effects = 0;
  }
  void Reset() {
    ReleaseEffects(); force_acquisition = 0;
    wheel = nullptr; devices.clear(); discovered_ids.clear(); motion.Reset();
    std::lock_guard lock(mutex);
    const auto packet = state.packet + 1;
    state = {}; state.packet = packet; last_force = 0; ResetForceHistory();
  }
  bool Discover(HWND hwnd) {
    Reset();
    input->EnumDevices(DI8DEVCLASS_GAMECTRL, Enumerate, this, DIEDFL_ATTACHEDONLY);
    std::sort(discovered_ids.begin(), discovered_ids.end());
    unsigned selected_rank = 0;
    for (auto& device : devices) {
      if (device->AxisIndex(config.steering.axis) < 0) continue;
      if (!config.device.empty()) {
        if (Equal(device->guid, config.device) || Equal(device->name, config.device)) { wheel = device.get(); break; }
        continue;
      }
      const unsigned rank = AutoSelectRank(device->ffb, device->driving,
          std::any_of(device->axes.begin(), device->axes.end(), [](const Axis& a) { return a.actuator; }), true);
      if (rank > selected_rank) { wheel = device.get(); selected_rank = rank; }
    }
    if (!wheel) return false;
    software_forces = config.force_backend == "software" || (config.force_backend == "auto" &&
        !std::all_of(wheel->effect_support.begin(), wheel->effect_support.end(), [](bool v) { return v; }));
    for (auto& device : devices) {
      const bool exclusive = device.get() == wheel && config.ffb_enabled;
      if (FAILED(device->handle->SetCooperativeLevel(hwnd, DISCL_FOREGROUND |
          (exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE)))) {
        if (device.get() == wheel) { Reset(); return false; }
        continue;
      }
      if (exclusive) {
        DIPROPDWORD autocenter{};
        autocenter.diph = {sizeof(autocenter), sizeof(DIPROPHEADER), 0, DIPH_DEVICE};
        autocenter.dwData = DIPROPAUTOCENTER_OFF;
        device->handle->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);
      }
    }
    return true;
  }
  bool Poll(Device& device) {
    device.handle->Poll();
    if (FAILED(device.handle->GetDeviceState(sizeof(RawState), &device.raw))) {
      // Some HID drivers return DI_NOEFFECT from Poll even while unacquired.
      // Reacquire based on the state read as well as the polling operation.
      device.received_report = false;
      if (FAILED(device.handle->Acquire())) return false;
      ++device.acquisition;
      device.handle->Poll();
      if (FAILED(device.handle->GetDeviceState(sizeof(RawState), &device.raw))) return false;
    }
    // DirectInput returns midpoints until the first HID report after acquisition.
    // Do not turn those placeholder pedal values into half throttle and brake.
    if (!device.received_report) {
      for (size_t i = 0; i < device.axes.size(); ++i)
        if (device.raw.axes[i] != 32768) device.received_report = true;
      for (const auto button : device.raw.buttons) if (button & 0x80) device.received_report = true;
    }
    return true;
  }
  Device* FindDevice(const std::string& selector) {
    Device* device = wheel;
    if (!selector.empty()) {
      device = nullptr;
      for (auto& candidate : devices)
        if (Equal(candidate->guid, selector) || Equal(candidate->name, selector)) { device = candidate.get(); break; }
    }
    return device;
  }
  bool Button(const std::string& selector, int button) {
    const auto* device = FindDevice(selector);
    return device && device->readable && button > 0 && button <= int(device->buttons) &&
        (device->raw.buttons[button - 1] & 0x80);
  }
  float AxisValue(const AxisBinding& binding, bool centered) {
    Device* device = FindDevice(binding.device);
    if (!device || !device->readable) return 0;
    const int index = device->AxisIndex(binding.axis);
    return index < 0 ? 0 : binding.calibration.Apply(device->raw.axes[index], centered);
  }
  bool ApplyEffect(unsigned index, const Effect& value) {
    auto& effect = effects[index];
    const GUID* type[] = {&GUID_Inertia, &GUID_Friction, &GUID_Damper, &GUID_Spring, &GUID_ConstantForce};
    DWORD axis = 0;
    const int steering = wheel->AxisIndex(config.steering.axis);
    if (steering < 0) return false;
    axis = wheel->axes[size_t(steering)].id;
    LONG direction = 10000;
    const auto scaled = [](float number) { return LONG(std::lround(number * 10000)); };
    DICONSTANTFORCE constant{scaled(value.magnitude)};
    DIPERIODIC periodic{};
    periodic.dwMagnitude = DWORD(scaled(value.magnitude));
    periodic.dwPeriod = DWORD(std::lround(1e6 / (index == 5 ? kRumbleLowHz : kRumbleHighHz)));
    DICONDITION condition{};
    condition.lOffset = scaled(value.condition.center);
    condition.lPositiveCoefficient = scaled(value.condition.positive);
    condition.lNegativeCoefficient = scaled(value.condition.negative);
    condition.dwPositiveSaturation = DWORD(scaled(value.condition.positive_limit));
    condition.dwNegativeSaturation = DWORD(scaled(value.condition.negative_limit));
    condition.lDeadBand = scaled(value.condition.deadband);
    DIEFFECT parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTIDS;
    // Finite device-side duration provides a second watchdog if the process stalls.
    parameters.dwDuration = INFINITE;
    parameters.dwGain = DWORD(std::lround(value.gain * 10000));
    parameters.dwTriggerButton = DIEB_NOTRIGGER;
    parameters.cAxes = 1; parameters.rgdwAxes = &axis; parameters.rglDirection = &direction;
    const bool vibration = index >= 5;
    parameters.cbTypeSpecificParams = vibration ? sizeof(periodic) : index == 4 ? sizeof(constant) : sizeof(condition);
    parameters.lpvTypeSpecificParams = vibration ? static_cast<void*>(&periodic) :
        index == 4 ? static_cast<void*>(&constant) : static_cast<void*>(&condition);
    const bool condition_force =
        (value.condition.positive != 0 && value.condition.positive_limit != 0) ||
        (value.condition.negative != 0 && value.condition.negative_limit != 0);
    // Constant torque and vibrations keep running through zero: stopping and restarting
    // them each time the force crosses zero (or a vibration fades) is felt as a click.
    const bool running = value.playing && value.gain != 0 && (index >= 4 || condition_force);
    if (!effect) {
      if (!running) return true;
      if (FAILED(wheel->handle->CreateEffect(vibration ? GUID_Sine : *type[index], &parameters, &effect, nullptr)))
        return vibration;  // Detail is optional; never drop steering forces for it.
      started[index] = false;
    }
    if (!running) { started[index] = false; return SUCCEEDED(effect->Stop()); }
    // Update a playing effect in place; restarting it every update causes dropouts on
    // some drivers and resets the vibration phase.
    const DWORD update = DIEP_TYPESPECIFICPARAMS | DIEP_DURATION | DIEP_GAIN | DIEP_DIRECTION;
    if (started[index] && SUCCEEDED(effect->SetParameters(&parameters, update | DIEP_NORESTART))) return true;
    if (SUCCEEDED(effect->SetParameters(&parameters, update | DIEP_START))) { started[index] = true; return true; }
    // Some drivers cannot update direction/gain on a downloaded effect.
    // Recreate it with the complete parameters rather than dropping feedback.
    effect->Stop(); effect->Release(); effect = nullptr;
    started[index] = SUCCEEDED(wheel->handle->CreateEffect(vibration ? GUID_Sine : *type[index], &parameters, &effect, nullptr)) &&
        SUCCEEDED(effect->Start(1, 0));
    return started[index] || vibration;
  }
  void Run(std::stop_token stop) {
    const HRESULT created = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
        IID_IDirectInput8W, reinterpret_cast<void**>(&input), nullptr);
    if (FAILED(created)) {
      std::lock_guard lock(mutex); state.error = "DirectInput8Create failed"; return;
    }
    PollTimer poll_timer;
    uint64_t next_discovery = 0;
    HWND bound = nullptr;
    while (!stop.stop_requested()) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      const auto now = Now(); const auto hwnd = window.load();
      const bool focused = hwnd && GetForegroundWindow() == hwnd;
      if (config.enabled && hwnd && (hwnd != bound || (!wheel && now >= next_discovery))) {
        bound = hwnd; Discover(hwnd); next_discovery = now + 2000;
      }
      // Discover separate USB peripherals even while the wheel stays attached.
      // Stable inventories do not tear down active force effects.
      if (config.enabled && hwnd && wheel && now >= next_discovery) {
        if (InventoryChanged()) Discover(hwnd);
        next_discovery = now + 2000;
      }
      bool connected = false;
      if (wheel && focused) {
        connected = Poll(*wheel);
        wheel->readable = connected && wheel->received_report;
        for (auto& device : devices) if (device.get() != wheel)
          device->readable = Poll(*device) && device->received_report;
      }
      if (wheel && connected) {
        if (force_acquisition != wheel->acquisition) {
          // Drivers may discard downloaded effects or reset device gain on
          // reacquisition even when the next input read succeeds immediately.
          // Rebuild every output setting before accepting fresh game forces.
          ReleaseEffects(); motion.Reset();
          { std::lock_guard lock(mutex); ResetForceHistory(); }
          force_acquisition = wheel->acquisition;
        }
        State next;
        next.connected = true; next.device = wheel->name + " " + wheel->guid;
        next.ffb_capable = wheel->ffb;
        next.force_backend = software_forces ? "software" : "native";
        next.steering = std::clamp(AxisValue(config.steering, true) * config.rotation_scale, -1.0f, 1.0f);
        live_steering.store(next.steering);
        next.throttle = AxisValue(config.throttle, false); next.brake = AxisValue(config.brake, false);
        next.clutch = AxisValue(config.clutch, false); next.handbrake = AxisValue(config.handbrake, false);
        auto physical_axis = config.steering;
        physical_axis.calibration.inverted = false;
        motion.Update(AxisValue(physical_axis, true), now, config.velocity_scale, config.acceleration_scale);
        for (size_t bit = 0; bit < config.buttons.size(); ++bit) {
          const int button = config.buttons[bit];
          if (Button(config.button_devices[bit], button)) next.buttons |= uint16_t(1u << bit);
        }
        if (config.shifter_enabled) {
          std::array<bool, 9> contacts{};
          for (size_t i = 0; i < contacts.size(); ++i)
            contacts[i] = Button(config.shifter_device, config.gear_buttons[i]);
          next.gear = SelectGear(contacts, config.shifter_gears);
        }
        if (wheel->povs && LOWORD(wheel->raw.pov[0]) != 0xFFFF) {
          const auto angle = wheel->raw.pov[0];
          if (angle >= 31500 || angle <= 4500) next.buttons |= 1;
          if (angle >= 13500 && angle <= 22500) next.buttons |= 2;
          if (angle >= 22500 && angle <= 31500) next.buttons |= 4;
          if (angle >= 4500 && angle <= 13500) next.buttons |= 8;
        }
        std::lock_guard lock(mutex);
        next.packet = state.packet + 1; next.error = state.error;
        next.output_active = state.output_active;
        next.force_age_ms = state.force_age_ms;
        next.force_stop_reason = state.force_stop_reason;
        next.native_force_flags = state.native_force_flags;
        next.native_force_flags_known = state.native_force_flags_known;
        next.downloaded_effects = state.downloaded_effects;
        state = std::move(next);
      } else {
        motion.Reset();
        std::lock_guard lock(mutex);
        // Keep a previously readable wheel attached while focus is elsewhere.
        // A foreground polling failure is a real disconnect immediately; do
        // not leave the input selector on a stale handle until the next scan.
        const bool keep_connected = wheel && !focused && state.connected;
        const auto packet = state.packet + 1;
        auto error = std::move(state.error);
        state = {}; state.packet = packet; state.error = std::move(error);
        state.connected = keep_connected;
        if (keep_connected) { state.ffb_capable = wheel->ffb; state.device = wheel->name + " " + wheel->guid; }
      }
      EffectFrame requested; uint64_t last, force_now;
      {
        std::lock_guard lock(mutex);
        requested = forces; last = last_force;
        {
          // Telemetry mode replaces FM4's per-frame torque, quantized to +/-127 levels,
          // with the full-precision physics torque. Either source is smoothed into the
          // 250 Hz loop; the game's steps need longer smoothing. Sharing the filter
          // history avoids a jump when stale telemetry falls back to game torque.
          const auto now = Now();
          const bool fresh = config.telemetry_force && last_telemetry && now >= last_telemetry &&
              now - last_telemetry <= config.timeout_ms;
          requested.slots[4].magnitude = telemetry_torque.Update(
              fresh ? telemetry.torque : requested.slots[4].magnitude, now, fresh ? 4 : 16);
          // Convert the game's torque sign to DirectInput's (see HostTorque).
          requested.slots[4].magnitude = HostTorque(requested.slots[4].magnitude, config.invert_force);
          ShapeForces(requested, config.shape);
          // Road texture is added after the weight curve so small bumps are not amplified.
          const float texture = fresh ? road_frames.Sample(now) * config.road_gain : 0;
          requested.slots[4].magnitude = std::clamp(requested.slots[4].magnitude + texture, -1.0f, 1.0f);
          if (requested.Valid()) {
            const auto detail = rumble_detail.Update(requested.rumble_low, requested.rumble_high, now, config.engine_gain);
            requested.rumble_low = detail[0]; requested.rumble_high = detail[1];
          }
        }
        // Discovery and HID polling can block. A guest force published while
        // they run is newer than the iteration's original timestamp. Sample
        // after taking the request snapshot so a fresh update is never
        // mistaken for a future timestamp and stopped by the watchdog.
        force_now = Now();
      }
      const bool allowed = config.ffb_enabled && wheel && wheel->readable && requested.Valid() &&
          CanApplyForces(focused, active.load(), connected, force_now, last, config.timeout_ms);
      {
        std::lock_guard lock(mutex);
        if (!allowed) ResetForceHistory();
        state.force_age_ms = last && force_now >= last ? force_now - last : 0;
        state.force_stop_reason = !config.ffb_enabled ? "disabled in profile" :
            !focused ? "window not focused" : !active.load() ? "wheel input inactive" :
            !connected || !wheel || !wheel->readable ? "wheel input unavailable" :
            !requested.Valid() ? "invalid game effects" : !last ? "game/input stopped output" :
            force_now < last ? "force timestamp ahead of watchdog" :
            force_now - last >= config.timeout_ms ? "force update watchdog expired" : "";
      }
      if (allowed) stopped_at = 0;
      // The game briefly stops its forces at lap changes and similar moments. Releasing
      // and recreating every effect for that is a clunk, so fade the torque and vibrations
      // to zero and keep the effects for a short grace period.
      const bool transient = applying && !allowed && config.ffb_enabled && focused && active.load() &&
          connected && wheel && wheel->readable;
      if (transient && !stopped_at) stopped_at = force_now ? force_now : 1;
      if (transient && force_now - stopped_at < 400) {
        Effect quiet; quiet.type = EffectType::Constant; quiet.gain = 1; quiet.playing = true;
        for (unsigned i = 4; i < effects.size(); ++i) if (effects[i]) ApplyEffect(i, quiet);
      } else if (allowed) {
        bool success = true;
        if (!actuators_ready || force_now >= next_force_state_check) {
          DWORD flags = 0; bool known = false;
          const auto result = PrepareForceActuators(*wheel->handle, flags, known);
          actuators_ready = result == ForceDeviceResult::Ready;
          next_force_state_check = force_now + 1000;
          {
            std::lock_guard lock(mutex);
            state.native_force_flags = flags; state.native_force_flags_known = known;
            if (actuators_ready) state.error.clear();
            else if (result == ForceDeviceResult::Muted) {
              // DirectInput commands may still be pending. Do not describe
              // asynchronous actuator state as a failed command.
              state.force_stop_reason = "waiting for driver force-state change";
              state.error.clear();
            } else {
              state.error = result == ForceDeviceResult::SwitchOff ? "Wheel force/safety switch is off" :
                  result == ForceDeviceResult::PowerOff ? "Wheel force power is off" :
                  result == ForceDeviceResult::DeviceLost ? "Wheel force device lost; reconnect it" :
                  "Wheel force actuator resume/enable failed";
            }
          }
          if (!actuators_ready) {
            ReleaseEffects();
            // A device may reset its global gain while its actuators are off,
            // paused or being recovered. Reapply it after state recovery.
            poll_timer.Wait(std::chrono::milliseconds(20));
            continue;
          }
        }
        if (!software_forces && !gain_set) {
          // Device gain scales the mixed result, bounding total output even
          // when the game's spring, damper and torque overlap.
          DIPROPDWORD gain{};
          gain.diph = {sizeof(gain), sizeof(DIPROPHEADER), 0, DIPH_DEVICE};
          gain.dwData = DWORD(std::lround(config.gain * 10000));
          success = gain_set = SUCCEEDED(wheel->handle->SetProperty(DIPROP_FFGAIN, &gain.diph));
        }
        if (!software_forces && !success && config.force_backend == "auto") {
          ReleaseEffects(); software_forces = true; success = true;
        }
        if (software_forces) {
          // Set device gain to full if supported. The mixed constant magnitude
          // already contains the user's limit; never multiply it twice.
          if (!gain_set) {
            if (force_now >= next_gain_retry) {
              DIPROPDWORD gain{};
              gain.diph = {sizeof(gain), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}; gain.dwData = 10000;
              const HRESULT gain_result = wheel->handle->SetProperty(DIPROP_FFGAIN, &gain.diph);
              if (SUCCEEDED(gain_result)) {
                gain_set = true; next_gain_retry = 0;
                std::lock_guard lock(mutex); state.error.clear();
              } else if (gain_result == DIERR_UNSUPPORTED) {
                // Keep the historical fallback for drivers without this
                // property; their default gain remains in effect.
                gain_set = true;
                std::lock_guard lock(mutex);
                state.error = "DirectInput device gain is unsupported; software forces use the driver default";
              } else {
                next_gain_retry = force_now + 250;
                std::lock_guard lock(mutex);
                state.error = "DirectInput device gain update failed; software forces will retry";
              }
            }
            if (!gain_set) {
              // Do not apply the already user-scaled software force while an
              // old device gain may still multiply it. Retain the fresh guest
              // frame and retry at a bounded rate.
              {
                std::lock_guard lock(mutex);
                state.output_active = false;
                state.force_stop_reason = "waiting to set software force device gain";
              }
              poll_timer.Wait(std::chrono::milliseconds(20));
              continue;
            }
          }
          Effect constant; constant.type = EffectType::Constant; constant.gain = 1; constant.playing = true;
          const double seconds = double(force_now) / 1000.0;
          constant.magnitude = std::clamp(MixSoftwareForces(requested, motion, config.gain) +
              RumbleForce(requested, seconds, config.detail_gain) * config.gain, -config.gain, config.gain);
          success = ApplyEffect(4, constant);
        } else {
          for (unsigned i = 0; i < requested.slots.size(); ++i)
            if (success) success = ApplyEffect(i, requested.slots[i]);
          for (unsigned i = 0; i < 2 && success; ++i) {
            Effect rumble; rumble.gain = 1; rumble.playing = true;
            rumble.magnitude = std::clamp((i ? requested.rumble_high : requested.rumble_low * kRumbleLowWeight) * config.detail_gain, 0.f, 1.f);
            success = ApplyEffect(5 + i, rumble);
          }
        }
        applying = true;
        { std::lock_guard lock(mutex); state.output_active = success;
          state.downloaded_effects = unsigned(std::count_if(effects.begin(), effects.end(), [](auto* effect) { return effect != nullptr; })); }
        if (!success) {
          ReleaseEffects(); std::lock_guard lock(mutex); last_force = 0;
          if (!software_forces && config.force_backend == "auto") {
            software_forces = true;
            state.error = "Native effects rejected; switched to software constant torque";
          } else state.error = "Wheel rejected a force effect; output stopped";
        }
      } else if (applying) {
        ReleaseEffects();
        std::lock_guard lock(mutex); last_force = 0;
      }
      if (wheel && focused && !connected && now >= next_discovery) {
        Reset(); next_discovery = now + 500;
      }
      poll_timer.Wait(std::chrono::milliseconds(4));
    }
    Reset(); input->Release(); input = nullptr;
  }
};

Host::Host(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Host::~Host() = default;
void Host::SetWindow(void* window) { impl_->window.store(static_cast<HWND>(window)); }
void Host::SetActive(bool active) {
  impl_->active.store(active);
  if (!active) Stop();
}
void Host::Submit(EffectFrame forces) {
  std::lock_guard lock(impl_->mutex);
  impl_->forces = forces;
  impl_->last_force = forces.Valid() ? Now() : 0;
}
void Host::SubmitTelemetry(const Telemetry& telemetry) {
  if (!telemetry.Valid()) return;
  std::lock_guard lock(impl_->mutex);
  impl_->telemetry = telemetry;
  impl_->last_telemetry = Now();
  impl_->road_frames.Push(impl_->road_texture.Update(telemetry, impl_->last_telemetry), impl_->last_telemetry);
}
void Host::Stop() {
  std::lock_guard lock(impl_->mutex);
  impl_->last_force = 0; impl_->forces = {}; impl_->ResetForceHistory();
}
State Host::Snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
float Host::LiveSteering() const { return impl_->live_steering.load(); }
const Config& Host::config() const { return impl_->config; }

}  // namespace fm4::wheel
