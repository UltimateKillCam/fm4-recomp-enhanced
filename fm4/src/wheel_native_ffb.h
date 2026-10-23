// DirectInput force-state recovery. Modified 2026-10-17. GPL-3.0-only.
#pragma once
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dinput.h>

namespace fm4::wheel {
enum class ForceDeviceResult { Ready, SwitchOff, PowerOff, DeviceLost, ResumeFailed, EnableFailed, Muted };

// Tests supply an in-memory endpoint. No hardware commands are sent by them.
template <typename Device>
ForceDeviceResult PrepareForceActuators(Device& device, DWORD& flags, bool& known) {
  flags = 0;
  known = SUCCEEDED(device.GetForceFeedbackState(&flags));
  // State reporting is optional on legitimate drivers. Keep effect HRESULTs
  // authoritative if this telemetry is unavailable.
  if (!known) return ForceDeviceResult::Ready;
  auto blocked = [](DWORD value) {
    if (value & (DIGFFS_USERFFSWITCHOFF | DIGFFS_SAFETYSWITCHOFF)) return ForceDeviceResult::SwitchOff;
    if (value & DIGFFS_POWEROFF) return ForceDeviceResult::PowerOff;
    if (value & DIGFFS_DEVICELOST) return ForceDeviceResult::DeviceLost;
    return ForceDeviceResult::Ready;
  };
  auto result = blocked(flags);
  if (result != ForceDeviceResult::Ready) return result;
  bool changed = false;
  if (flags & DIGFFS_PAUSED) {
    if (FAILED(device.SendForceFeedbackCommand(DISFFC_CONTINUE))) return ForceDeviceResult::ResumeFailed;
    changed = true;
  }
  if (flags & DIGFFS_ACTUATORSOFF) {
    if (FAILED(device.SendForceFeedbackCommand(DISFFC_SETACTUATORSON))) return ForceDeviceResult::EnableFailed;
    changed = true;
  }
  if (changed) {
    known = SUCCEEDED(device.GetForceFeedbackState(&flags));
    if (known) {
      result = blocked(flags);
      if (result != ForceDeviceResult::Ready) return result;
      if (flags & (DIGFFS_PAUSED | DIGFFS_ACTUATORSOFF)) return ForceDeviceResult::Muted;
    }
  }
  return ForceDeviceResult::Ready;
}
}  // namespace fm4::wheel
