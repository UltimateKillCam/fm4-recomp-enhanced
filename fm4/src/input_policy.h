// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fm4::wheel {
// Suppress idle stick drift before it reaches FM4's menu/camera handling.
// Rescale the remaining travel while preserving both signed endpoints.
inline int16_t ControllerAxis(int16_t raw, float deadzone) {
  if (!std::isfinite(deadzone) || deadzone < 0 || deadzone >= 1) return 0;
  const float endpoint = raw < 0 ? 32768.f : 32767.f;
  const float magnitude = std::abs(float(raw)) / endpoint;
  if (magnitude <= deadzone) return 0;
  const float value = (magnitude - deadzone) / (1 - deadzone) * endpoint;
  return int16_t(std::lround(raw < 0 ? -value : value));
}
enum class InputSource { None, Wheel, Controller };
struct Activity {
  bool connected = false;
  uint16_t buttons = 0;
  std::array<float, 6> axes{};
};

// Keep the inactive device's position as an anchor. Slow deliberate movement
// accumulates; noise and an already held pedal cannot steal the selected input.
class InputSelection {
 public:
  InputSource Update(const Activity& wheel, const Activity& pad) {
    RestedAxes(wheel,wheel_anchor_,true);
    RestedAxes(pad,pad_anchor_,false);
    auto next = selected_;
    if (next == InputSource::None || (next == InputSource::Wheel && !wheel.connected) ||
        (next == InputSource::Controller && !pad.connected))
      next = pad.connected ? InputSource::Controller : wheel.connected ? InputSource::Wheel : InputSource::None;
    else if (next == InputSource::Controller && Changed(wheel, wheel_anchor_, wheel_previous_, true))
      next = InputSource::Wheel;
    else if (next == InputSource::Wheel && Changed(pad, pad_anchor_, pad_previous_, false))
      next = InputSource::Controller;
    // New devices start at their current position and require subsequent input.
    if (next != selected_ || wheel.connected != wheel_previous_.connected || pad.connected != pad_previous_.connected) {
      wheel_anchor_ = wheel; pad_anchor_ = pad;
    }
    selected_ = next; wheel_previous_ = wheel; pad_previous_ = pad;
    return selected_;
  }
  InputSource selected() const { return selected_; }
 private:
  static void RestedAxes(const Activity& value,Activity& anchor,bool wheel) {
    if(!value.connected) return;
    // Releasing an inactive trigger/pedal to rest must rearm its next press.
    // A held input keeps its handoff anchor and cannot steal control repeatedly.
    for(size_t i=0;i<value.axes.size();++i)
      if(std::isfinite(value.axes[i])&&std::abs(value.axes[i])<=(wheel?.02f:.12f)&&
         std::abs(anchor.axes[i])>(wheel?.02f:.12f))
        anchor.axes[i]=value.axes[i];
  }
  static bool Changed(const Activity& value, const Activity& anchor, const Activity& previous, bool wheel) {
    if (!value.connected || !previous.connected) return false;
    if (value.buttons & ~previous.buttons) return true;
    for (size_t i = 0; i < value.axes.size(); ++i) {
      const float threshold = wheel ? (i == 0 ? .04f : .08f) : (i < 4 ? .20f : .10f);
      if (std::isfinite(value.axes[i]) && std::abs(value.axes[i] - anchor.axes[i]) > threshold &&
          std::abs(value.axes[i]) > (wheel ? .02f : .12f)) return true;
    }
    return false;
  }
  InputSource selected_ = InputSource::None;
  Activity wheel_anchor_, pad_anchor_, wheel_previous_, pad_previous_;
};
}
