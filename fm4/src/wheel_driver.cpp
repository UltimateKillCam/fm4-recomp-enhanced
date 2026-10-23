// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#include "wheel_driver.h"
#include "wheel_host.h"
#include "input_policy.h"
#include "../../tools/sdk/controller_bridge.h"
#include "../../tools/wheel/diagnostic_input.h"

#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window_listener.h>

#include <mutex>
#include <chrono>
#include <atomic>
#include <cstring>
#include <deque>
#include <cstdlib>

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse_to_right_stick);
REXCVAR_DECLARE(int32_t, mnk_user_index);
REXCVAR_DECLARE(std::string, keybind_a);
REXCVAR_DECLARE(std::string, keybind_b);
REXCVAR_DECLARE(std::string, keybind_x);
REXCVAR_DECLARE(std::string, keybind_y);
REXCVAR_DECLARE(std::string, keybind_left_shoulder);
REXCVAR_DECLARE(std::string, keybind_right_shoulder);
REXCVAR_DECLARE(std::string, keybind_lstick_press);
REXCVAR_DECLARE(std::string, keybind_rstick_press);
REXCVAR_DECLARE(std::string, keybind_dpad_up);
REXCVAR_DECLARE(std::string, keybind_dpad_down);
REXCVAR_DECLARE(std::string, keybind_dpad_left);
REXCVAR_DECLARE(std::string, keybind_dpad_right);
REXCVAR_DECLARE(std::string, keybind_back);
REXCVAR_DECLARE(std::string, keybind_start);
REXCVAR_DECLARE(std::string, keybind_lstick_up);
REXCVAR_DECLARE(std::string, keybind_lstick_down);
REXCVAR_DECLARE(std::string, keybind_lstick_left);
REXCVAR_DECLARE(std::string, keybind_lstick_right);

namespace fm4::wheel {
using rex::X_STATUS;
using rex::X_RESULT;
namespace {
std::mutex host_mutex;
std::weak_ptr<Host> force_host;
std::array<std::atomic<bool>, 4> wheel_selected{};
// Steering most recently reported to the game while the wheel is the active input.
std::atomic<float> guest_steering{0};
std::atomic<bool> guest_steering_valid{false};
// Latest virtual-wheel steering, for FM4_TRACE_INPUT force traces only.
std::atomic<float> trace_steering{0};

// Preserve taps that begin and end between two guest input polls. Keep the
// SDK's configurable keyboard/mouse driver for held keys, sticks and triggers.
class KeyboardDriver final : public rex::input::InputDriver,
    public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  KeyboardDriver() : InputDriver(nullptr, 0), keyboard_(nullptr, 0) {
    keyboard_.set_is_active_callback([this] { return is_active(); });
  }
  ~KeyboardDriver() override { Detach(); }
  X_STATUS Setup() override { return keyboard_.Setup(); }
  void OnWindowAvailable(rex::ui::Window* window) override {
    Detach();
    // The pinned MnK driver captures and recentres the cursor whenever enabled,
    // even with mouse-to-stick disabled. Attach it only for requested mouse
    // driving; our own listener supplies held keyboard buttons and menu taps.
    if(REXCVAR_GET(mnk_mouse_to_right_stick)) keyboard_.OnWindowAvailable(window);
    window_ = window;
    if (window_) { window_->AddListener(this); window_->AddInputListener(this, 0); }
  }
  X_RESULT GetCapabilities(uint32_t user, uint32_t flags, rex::input::X_INPUT_CAPABILITIES* caps) override {
    return keyboard_.GetCapabilities(user, flags, caps);
  }
  X_RESULT GetState(uint32_t user, rex::input::X_INPUT_STATE* output) override {
    const auto result = keyboard_.GetState(user, output);
    std::lock_guard lock(mutex_);
    if (result == X_ERROR_SUCCESS && output && focused_ && is_active()) {
      output->gamepad.buttons = uint16_t(output->gamepad.buttons) | held_ | pulses_.Read(Now());
      const auto stick = stick_held_ | stick_pulses_.Read(Now());
      if (stick & 3) output->gamepad.thumb_ly = int16_t((stick & 1 ? 32767 : 0) - (stick & 2 ? 32767 : 0));
      if (stick & 12) output->gamepad.thumb_lx = int16_t((stick & 8 ? 32767 : 0) - (stick & 4 ? 32767 : 0));
    }
    if(result==X_ERROR_SUCCESS&&output) {
      if(std::memcmp(&previous_gamepad_,&output->gamepad,sizeof(previous_gamepad_))!=0) {
        ++packet_;previous_gamepad_=output->gamepad;
      }
      output->packet_number=packet_;
    }
    return result;
  }
  X_RESULT SetState(uint32_t user, rex::input::X_INPUT_VIBRATION* value) override {
    return keyboard_.SetState(user, value);
  }
  X_RESULT GetKeystroke(uint32_t user, uint32_t flags, rex::input::X_INPUT_KEYSTROKE* value) override {
    if (!REXCVAR_GET(mnk_mode) || (user != uint32_t(REXCVAR_GET(mnk_user_index)) && user != 0xFF))
      return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(mutex_);
    if (keys_.empty() || !focused_ || !is_active()) return X_ERROR_EMPTY;
    if (value) *value = keys_.front();
    keys_.pop_front();
    return X_ERROR_SUCCESS;
  }
  bool ConsumeRawMouseDelta(int32_t& x, int32_t& y) override { return keyboard_.ConsumeRawMouseDelta(x, y); }
  void OnKeyDown(rex::ui::KeyEvent& event) override {
    if (!REXCVAR_GET(mnk_mode) || !is_active()) return;
    const auto buttons = BoundButtons(event.virtual_key());
    const auto stick = BoundStick(event.virtual_key());
    std::lock_guard lock(mutex_);
    if (focused_) {
      pulses_.Press(buttons, Now());
      stick_pulses_.Press(stick, Now());
      stick_held_ |= stick;
      Enqueue(buttons, uint16_t(1 | ((held_ & buttons) ? 4 : 0)));
      held_ |= buttons;
    }
  }
  void OnKeyUp(rex::ui::KeyEvent& event) override {
    const auto buttons = BoundButtons(event.virtual_key());
    std::lock_guard lock(mutex_);
    Enqueue(buttons & held_, 2); held_ &= ~buttons;
    stick_held_ &= ~BoundStick(event.virtual_key());
  }
  static uint16_t BoundStick(rex::ui::VirtualKey key) {
    const std::array<std::string,4> keys{REXCVAR_GET(keybind_lstick_up),REXCVAR_GET(keybind_lstick_down),
        REXCVAR_GET(keybind_lstick_left),REXCVAR_GET(keybind_lstick_right)};
    uint16_t result=0;
    for(unsigned bit=0;bit<keys.size();++bit)
      if(!keys[bit].empty()&&key==rex::ui::ParseVirtualKey(keys[bit])) result|=uint16_t(1u<<bit);
    return result;
  }
  static uint16_t BoundButtons(rex::ui::VirtualKey key) {
    const std::array<std::string, 16> keys{
        REXCVAR_GET(keybind_dpad_up), REXCVAR_GET(keybind_dpad_down),
        REXCVAR_GET(keybind_dpad_left), REXCVAR_GET(keybind_dpad_right),
        REXCVAR_GET(keybind_start), REXCVAR_GET(keybind_back),
        REXCVAR_GET(keybind_lstick_press), REXCVAR_GET(keybind_rstick_press),
        REXCVAR_GET(keybind_left_shoulder), REXCVAR_GET(keybind_right_shoulder), "", "",
        REXCVAR_GET(keybind_a), REXCVAR_GET(keybind_b), REXCVAR_GET(keybind_x), REXCVAR_GET(keybind_y)};
    uint16_t buttons = 0;
    for (unsigned bit = 0; bit < keys.size(); ++bit)
      if (!keys[bit].empty() && key == rex::ui::ParseVirtualKey(keys[bit])) buttons |= uint16_t(1u << bit);
    return buttons;
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    std::lock_guard lock(mutex_); focused_ = false; pulses_.Clear(); stick_pulses_.Clear(); keys_.clear(); held_ = stick_held_ = 0;
  }
  void OnGotFocus(rex::ui::UISetupEvent&) override { std::lock_guard lock(mutex_); focused_ = true; }
  void OnClosing(rex::ui::UIEvent&) override { Detach(); }
 private:
  void Enqueue(uint16_t buttons, uint16_t flags) {
    using V = rex::ui::VirtualKey;
    const std::array<V, 16> lookup{V::kXInputPadDpadUp, V::kXInputPadDpadDown,
        V::kXInputPadDpadLeft, V::kXInputPadDpadRight, V::kXInputPadStart,
        V::kXInputPadBack, V::kXInputPadLThumbPress, V::kXInputPadRThumbPress,
        V::kXInputPadLShoulder, V::kXInputPadRShoulder, V::kNone, V::kNone,
        V::kXInputPadA, V::kXInputPadB, V::kXInputPadX, V::kXInputPadY};
    for (unsigned bit = 0; bit < lookup.size(); ++bit) if (buttons & (1u << bit)) {
      rex::input::X_INPUT_KEYSTROKE key{};
      key.virtual_key = uint16_t(lookup[bit]); key.flags = flags;
      key.user_index = uint8_t(REXCVAR_GET(mnk_user_index));
      if (keys_.size() < 128) keys_.push_back(key);
    }
  }
  static uint64_t Now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  void Detach() {
    if (window_) { window_->RemoveInputListener(this); window_->RemoveListener(this); window_ = nullptr; }
  }
  rex::input::mnk::MnkInputDriver keyboard_;
  rex::ui::Window* window_ = nullptr;
  std::mutex mutex_;
  ButtonPulses pulses_;
  ButtonPulses stick_pulses_;
  std::deque<rex::input::X_INPUT_KEYSTROKE> keys_;
  uint16_t held_ = 0, stick_held_ = 0;
  rex::input::X_INPUT_GAMEPAD previous_gamepad_{};
  uint32_t packet_=1;
  bool focused_ = true;
};

class Driver final : public rex::input::InputDriver, public rex::ui::WindowListener {
 public:
  explicit Driver(Config config)
      : InputDriver(nullptr, 0), host_(std::make_shared<Host>(std::move(config))), pad_(nullptr, 0) {
    std::lock_guard lock(host_mutex); force_host = host_;
    pad_.set_is_active_callback([this] { return focused_ && is_active(); });
  }
  ~Driver() override {
    Fm4ControllerShutdown();
    host_->Stop();
    if (attached_) attached_->RemoveListener(this);
  }
  X_STATUS Setup() override { Fm4ControllerConfigure(); return pad_.Setup(); }
  void OnWindowAvailable(rex::ui::Window* window) override {
    if (attached_) attached_->RemoveListener(this);
    attached_ = window;
    if (window) { window->AddListener(this); host_->SetWindow(window->GetNativeWindowHandle()); }
    pad_.OnWindowAvailable(window);
  }
  X_RESULT GetCapabilities(uint32_t user, uint32_t flags,
      rex::input::X_INPUT_CAPABILITIES* caps) override {
    auto state = host_->Snapshot();
    if(fm4::wheel_diagnostic::Enabled()) {
      if(user!=host_->config().user) return X_ERROR_DEVICE_NOT_CONNECTED;
      bool pad_connected;
      { std::lock_guard lock(selection_mutex_); state=virtual_wheel_;pad_connected=virtual_pad_connected_; }
      if(!wheel_selected[user]) {
        if(!pad_connected) return X_ERROR_DEVICE_NOT_CONNECTED;
        if(caps) { *caps={}; caps->type=1; caps->sub_type=1;
          caps->gamepad.buttons=0xf3ff;
          caps->gamepad.left_trigger=caps->gamepad.right_trigger=255;
          caps->gamepad.thumb_lx=caps->gamepad.thumb_ly=caps->gamepad.thumb_rx=caps->gamepad.thumb_ry=32767;
          caps->vibration.left_motor_speed=caps->vibration.right_motor_speed=65535;
        }
        return X_ERROR_SUCCESS;
      }
    }
    if (user != host_->config().user || !state.connected || !wheel_selected[user])
      return pad_.GetCapabilities(user, flags, caps);
    if (caps) {
      *caps = {}; caps->type = 1; caps->sub_type = 2;  // Xbox 360 wheel
      caps->flags = state.ffb_capable ? 1 : 0;
      caps->gamepad.buttons = uint16_t(0xF3FF);
      // FM4 reads 900 here as the supported steering rotation.
      caps->gamepad.thumb_lx = int16_t(900);
      // FM4's WheelLayouts uses left-stick Y for handbrake, right-stick Y
      // for clutch. Capabilities and state must advertise the same channels.
      caps->gamepad.thumb_ly = host_->config().handbrake.axis == "disabled" ? 0 : int16_t(-1);
      caps->gamepad.thumb_ry = host_->config().clutch.axis == "disabled" ? 0 : int16_t(-1);
      if (host_->config().shifter_enabled)
        caps->gamepad.thumb_rx = int16_t(host_->config().shifter_gears << 8);
      caps->vibration.left_motor_speed = uint16_t(0xFF00);
      caps->vibration.right_motor_speed = uint16_t(0xFF00);
      caps->gamepad.left_trigger = caps->gamepad.right_trigger = 255;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t user, rex::input::X_INPUT_STATE* output) override {
    rex::input::X_INPUT_STATE controller{};
    const bool diagnostic=fm4::wheel_diagnostic::Enabled();
    bool virtual_wheel_mode=false;
    uint8_t virtual_devices=3;
    rex::input::X_INPUT_STATE fixture{};
    X_RESULT pad_result=X_ERROR_DEVICE_NOT_CONNECTED;
    if(diagnostic) {
      if(user!=host_->config().user) return X_ERROR_DEVICE_NOT_CONNECTED;
      fixture=fm4::wheel_diagnostic::ReadState(true,&virtual_wheel_mode,&virtual_devices);
      if(!virtual_wheel_mode) controller=fixture;
      pad_result=(virtual_devices&1)?X_ERROR_SUCCESS:X_ERROR_DEVICE_NOT_CONNECTED;
    } else pad_result=pad_.GetState(user, &controller);
    if (pad_result == X_ERROR_SUCCESS) {
      auto& p = controller.gamepad;
      const auto deadzone = host_->config().stick_deadzone;
      p.thumb_lx = ControllerAxis(int16_t(p.thumb_lx), deadzone);
      p.thumb_ly = ControllerAxis(int16_t(p.thumb_ly), deadzone);
      p.thumb_rx = ControllerAxis(int16_t(p.thumb_rx), deadzone);
      p.thumb_ry = ControllerAxis(int16_t(p.thumb_ry), deadzone);
    }
    if (user != host_->config().user) {
      if (output) *output = controller;
      return pad_result;
    }
    if (!diagnostic && pad_result == X_ERROR_SUCCESS && host_->config().touchpad_back) {
      // SDL maps touchpad to Guide; expose it as the documented View shortcut.
      controller.gamepad.buttons = (uint16_t(controller.gamepad.buttons) & ~uint16_t(0x400)) |
          Fm4ControllerExtraButtons(user);
    }
    auto state = host_->Snapshot();
    if(diagnostic) {
      std::lock_guard lock(selection_mutex_);
      virtual_pad_connected_=(virtual_devices&1)!=0;
      if(!(virtual_devices&2)) virtual_wheel_={};
      virtual_wheel_.connected=(virtual_devices&2)!=0;
      virtual_wheel_.ffb_capable=virtual_wheel_.connected;
      virtual_wheel_.device=virtual_wheel_.connected?"FM4 virtual wheel fixture (physical feedback disabled)":"";
      virtual_wheel_.buttons=virtual_wheel_mode?uint16_t(fixture.gamepad.buttons):0;
      if(virtual_wheel_mode) {
        const auto& p=fixture.gamepad;
        const auto steering=int16_t(p.thumb_lx);
        virtual_wheel_.steering=steering/(steering<0?32768.f:32767.f);
        trace_steering=virtual_wheel_.steering;
        virtual_wheel_.throttle=p.right_trigger/255.f; virtual_wheel_.brake=p.left_trigger/255.f;
        virtual_wheel_.clutch=std::clamp(int16_t(p.thumb_ly)/32767.f,0.f,1.f);
        virtual_wheel_.handbrake=std::clamp(int16_t(p.thumb_ry)/32767.f,0.f,1.f);
        virtual_wheel_.gear=int16_t(p.thumb_rx);
      }
      state=virtual_wheel_;
    }
    if (user == host_->config().user) {
      std::lock_guard lock(status_mutex_);
      if (state.device != reported_device_) {
        reported_device_ = state.device;
        REXLOG_INFO("FM4 wheel device: {}", state.device.empty() ? "disconnected" : state.device);
      }
      if (state.error != reported_error_) {
        reported_error_ = state.error;
        if (!state.error.empty()) REXLOG_ERROR("FM4 wheel: {}", state.error);
      }
      if (state.output_active != reported_output_) {
        reported_output_ = state.output_active;
        REXLOG_INFO("FM4 DirectInput force output {}; device gain {}%",
            reported_output_ ? "active" : "stopped", host_->config().gain * 100);
        if (!reported_output_)
          REXLOG_INFO("FM4 force stop reason: {}; latest game force age {} ms",
              state.force_stop_reason, state.force_age_ms);
      }
      if (!state.force_backend.empty() && state.force_backend != reported_backend_) {
        reported_backend_ = state.force_backend;
        REXLOG_INFO("FM4 DirectInput force backend: {}", reported_backend_);
      }
      if (state.native_force_flags_known &&
          (!reported_force_flags_known_ || reported_force_flags_ != state.native_force_flags)) {
        reported_force_flags_known_ = true; reported_force_flags_ = state.native_force_flags;
        REXLOG_INFO("FM4 DirectInput force device state: 0x{:08X}", reported_force_flags_);
      }
      if (state.downloaded_effects != reported_effects_) {
        reported_effects_ = state.downloaded_effects;
        REXLOG_INFO("FM4 wheel effects accepted by DirectInput: {}", reported_effects_);
      }
    }
    const bool active = diagnostic || (focused_ && is_active());
    InputSource source;
    {
      std::lock_guard lock(selection_mutex_);
      const auto& p = controller.gamepad;
      const Activity wheel{state.connected, state.buttons,
          {state.steering, state.throttle, state.brake, state.clutch, state.handbrake, float(state.gear) / 8}};
      const Activity gamepad{pad_result == X_ERROR_SUCCESS, uint16_t(p.buttons),
          {int16_t(p.thumb_lx) / 32768.f, int16_t(p.thumb_ly) / 32768.f,
           int16_t(p.thumb_rx) / 32768.f, int16_t(p.thumb_ry) / 32768.f,
           p.left_trigger / 255.f, p.right_trigger / 255.f}};
      source = active ? selection_.Update(wheel, gamepad) : selection_.selected();
      const bool use_wheel = source == InputSource::Wheel;
      if (wheel_selected[user].exchange(use_wheel) != use_wheel || source != reported_source_) {
        RefreshGuestCapabilities(user);
        host_->Stop(); Fm4ControllerStop(user);
        reported_source_ = source;
        REXLOG_INFO("FM4 active input user {}: {}", user, use_wheel ? "wheel" :
            source == InputSource::Controller ? "controller" : "disconnected");
      }
      last_brake_ = p.left_trigger; last_throttle_ = p.right_trigger;
    }
    host_->SetActive(!diagnostic && active && source == InputSource::Wheel);
    if (source == InputSource::None) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (output) {
      *output = {};
      if (active && source == InputSource::Controller) *output = controller;
      const bool wheel_output = active && source == InputSource::Wheel;
      guest_steering_valid = wheel_output;
      if (wheel_output) {
        guest_steering = std::clamp(state.steering, -1.0f, 1.0f);
        output->gamepad.buttons = state.buttons;
        output->gamepad.thumb_lx = Steering(state.steering);
        output->gamepad.right_trigger = Pedal(state.throttle);
        output->gamepad.left_trigger = Pedal(state.brake);
        output->gamepad.thumb_ly = Steering(state.handbrake);
        output->gamepad.thumb_ry = Steering(state.clutch);
        output->gamepad.thumb_rx = state.gear;
      }
      std::lock_guard lock(selection_mutex_);
      if (std::memcmp(&previous_gamepad_, &output->gamepad, sizeof(previous_gamepad_)) != 0) {
        ++packet_; previous_gamepad_ = output->gamepad;
      }
      output->packet_number = packet_;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t user, rex::input::X_INPUT_VIBRATION* vibration) override {
    if (!vibration || user >= 4) return X_ERROR_BAD_ARGUMENTS;
    if(std::getenv("FM4_BACKGROUND")) { Fm4ControllerStop(user); return X_ERROR_SUCCESS; }
    if (!focused_ || !is_active() || wheel_selected[user]) {
      Fm4ControllerStop(user);
      return X_ERROR_SUCCESS;
    }
    std::lock_guard lock(selection_mutex_);
    const auto& config = host_->config();
    const int result = Fm4ControllerFeedback(user, vibration->left_motor_speed, vibration->right_motor_speed,
        last_brake_, last_throttle_, config.rumble_gain, config.adaptive_triggers ? config.adaptive_gain : 0);
    if (result < 0 && !reported_feedback_error_) {
      REXLOG_WARN("FM4 controller feedback request failed; check HIDAPI access to the controller");
      reported_feedback_error_ = true;
    }
    if (result > 0 && (uint16_t(vibration->left_motor_speed) || uint16_t(vibration->right_motor_speed)) && !reported_feedback_) {
      REXLOG_INFO("FM4 controller vibration accepted; custom DualSense adaptive triggers {}",
          config.adaptive_triggers ? "enabled" : "disabled");
      reported_feedback_ = true;
    }
    return result > 0 ? X_ERROR_SUCCESS : result == 0 ? X_ERROR_DEVICE_NOT_CONNECTED : X_ERROR_FUNCTION_FAILED;
  }
  X_RESULT GetKeystroke(uint32_t user, uint32_t flags, rex::input::X_INPUT_KEYSTROKE* value) override {
    if(fm4::wheel_diagnostic::Enabled()) {
      if(!value) return X_ERROR_BAD_ARGUMENTS;
      if(user!=0xff&&user!=host_->config().user) return X_ERROR_DEVICE_NOT_CONNECTED;
      if(!fm4::wheel_diagnostic::ReadKeystroke(value)) return X_ERROR_EMPTY;
      value->user_index=uint8_t(host_->config().user); return X_ERROR_SUCCESS;
    }
    const auto result = pad_.GetKeystroke(user, flags, value);
    // InputSystem stops at EMPTY. Let the keyboard driver supply a pending key.
    return result == X_ERROR_EMPTY ? X_ERROR_DEVICE_NOT_CONNECTED : result;
  }
  void OnClosing(rex::ui::UIEvent&) override {
    focused_ = false;
    Fm4ControllerShutdown();
    host_->SetActive(false); host_->SetWindow(nullptr);
    if (attached_) { attached_->RemoveListener(this); attached_ = nullptr; }
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    focused_ = false; host_->SetActive(false);
    for (uint32_t user = 0; user < 4; ++user) Fm4ControllerStop(user);
  }
  void OnGotFocus(rex::ui::UISetupEvent&) override { focused_ = true; }
 private:
  std::shared_ptr<Host> host_;
  rex::input::sdl::SDLInputDriver pad_;
  std::atomic<bool> focused_{true};
  std::mutex selection_mutex_;
  InputSelection selection_;
  State virtual_wheel_;
  bool virtual_pad_connected_=(fm4::wheel_diagnostic::InitialDevices()&1)!=0;
  InputSource reported_source_ = InputSource::None;
  rex::input::X_INPUT_GAMEPAD previous_gamepad_{};
  uint32_t packet_ = 1;
  uint8_t last_brake_ = 0, last_throttle_ = 0;
  bool reported_feedback_error_ = false, reported_feedback_ = false;
  std::mutex status_mutex_;
  std::string reported_device_, reported_error_;
  bool reported_output_ = false;
  unsigned reported_effects_ = 0;
  std::string reported_backend_;
  uint32_t reported_force_flags_ = 0;
  bool reported_force_flags_known_ = false;
  rex::ui::Window* attached_ = nullptr;
};
}

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(
    bool tool_mode, const std::filesystem::path& profile) {
  if (tool_mode) return rex::input::CreateDefaultInputSystem(true);
  auto input = std::make_unique<rex::input::InputSystem>(nullptr);
  try {
    WriteDefaultConfig(profile);
    auto config = LoadConfig(profile);
    if(std::getenv("FM4_BACKGROUND")) { config.enabled=false; config.ffb_enabled=false; }
    auto devices = std::make_unique<Driver>(std::move(config));
    if (devices->Setup() == X_STATUS_SUCCESS) input->AddDriver(std::move(devices));
    REXLOG_INFO("FM4 wheel/controller input backend; profile: {}", profile.string());
  } catch (const std::exception& error) {
    REXLOG_ERROR("Wheel profile rejected: {}", error.what());
    auto pad = std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0);
    if (pad->Setup() == X_STATUS_SUCCESS) input->AddDriver(std::move(pad));
  }
  // Background diagnostics must never attach the SDK keyboard/mouse driver:
  // it may move the system cursor even though its SDL window is hidden.
  if(!std::getenv("FM4_BACKGROUND")) {
    auto keyboard = std::make_unique<KeyboardDriver>();
    if (keyboard->Setup() == X_STATUS_SUCCESS) input->AddDriver(std::move(keyboard));
  } else REXLOG_INFO("FM4 background input: keyboard/mouse driver omitted; no cursor recentering");
  input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
  return input;
}
bool CockpitSteering(float& steering, float& rotation_scale, float& rotation_degrees) {
  std::lock_guard lock(host_mutex);
  auto host = force_host.lock();
  if (!host || !host->config().full_rotation || !guest_steering_valid.load()) return false;
  // Draw from the freshest wheel sample; fixtures drive the virtual wheel instead.
  steering = fm4::wheel_diagnostic::Enabled() ? guest_steering.load() : host->LiveSteering();
  rotation_scale = host->config().rotation_scale;
  rotation_degrees = host->config().rotation_degrees;
  return true;
}
bool HasWheel(uint32_t user) {
  std::lock_guard lock(host_mutex);
  if (auto host = force_host.lock(); host && host->config().user == user)
    return wheel_selected[user] && (fm4::wheel_diagnostic::Enabled()||host->Snapshot().connected);
  return false;
}
void SubmitTelemetry(const Telemetry& telemetry) {
  if (fm4::wheel_diagnostic::Enabled()) return;  // Fixtures never drive physical output.
  std::lock_guard lock(host_mutex);
  if (auto host = force_host.lock(); host && host->config().telemetry_force) host->SubmitTelemetry(telemetry);
}
void SubmitGuestEffects(uint32_t user, EffectFrame frame) {
  if(fm4::wheel_diagnostic::Enabled()) {
    // The guest still creates/updates its genuine five effects. Never submit
    // diagnostic force frames to physical hardware, even after a virtual handoff.
    static std::atomic<uint32_t> count=0;
    const auto sequence=++count;
    static const bool trace=std::getenv("FM4_TRACE_INPUT")!=nullptr;
    if(trace&&sequence%6==0)
      REXLOG_INFO("FM4 force trace: steering={:.3f}, torque={:.3f}, torque_gain={:.2f}, spring_gain={:.2f}, damper={:.3f}, rumble={:.2f}/{:.2f}",
          trace_steering.load(),frame.slots[4].magnitude,frame.slots[4].gain,frame.slots[3].gain,
          frame.slots[2].condition.positive*frame.slots[2].gain,frame.rumble_low,frame.rumble_high);
    if(sequence<=3||sequence%500==0) {
      REXLOG_INFO("FM4 virtual wheel force frame {}: torque={}, gain={}, playing={}",
          sequence,frame.slots[4].magnitude,frame.slots[4].gain,frame.slots[4].playing);
      // Constant torque can legitimately be zero while spring/damper effects
      // still act on a physical wheel. Record all five original effect slots
      // so a zero constant is not mistaken for a disabled feedback bridge.
      for(size_t i=0;i<frame.slots.size();++i) {
        const auto& effect=frame.slots[i];
        const auto& c=effect.condition;
        REXLOG_INFO("FM4 virtual wheel effect: frame={}, slot={}, gain={}, playing={}, torque={}, center={}, positive={}, negative={}, positive_limit={}, negative_limit={}, deadband={}",
            sequence,i+1,effect.gain,effect.playing,effect.magnitude,c.center,c.positive,c.negative,
            c.positive_limit,c.negative_limit,c.deadband);
      }
    }
    return;
  }
  std::lock_guard lock(host_mutex);
  if (auto host = force_host.lock(); host && host->config().user == user && wheel_selected[user])
    host->Submit(frame);
}
void StopGuestForces() {
  std::lock_guard lock(host_mutex);
  if (auto host = force_host.lock()) host->Stop();
}
void ShutdownFeedback() {
  Fm4ControllerShutdown();
  StopGuestForces();
}
}  // namespace fm4::wheel
