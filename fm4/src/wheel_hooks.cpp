// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
// Native implementations of FM4's extended wheel APIs. Addresses are for the
// executable enforced by tools/Build-Local.ps1, not other FM4 revisions.
#include "wheel_driver.h"
#include "wheel_protocol.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/runtime.h>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <map>
#include <chrono>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(fm4_time_based_wheel_visuals, false, "FM4",
                   "Experimental: use elapsed time for cockpit wheel matching and easing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
std::mutex effects_mutex;
std::array<fm4::wheel::EffectFrame, 4> banks;
std::array<bool, 4> enabled{};
uint32_t calls = 0;
float peak_torque = 0;
thread_local uint8_t* guest_controllers = nullptr;
std::array<std::atomic<bool>,4> pending_binding_refresh{};

bool Ready(uint32_t user) { return user < 4 && fm4::wheel::HasWheel(user); }
void Complete(uint32_t overlapped) {
  if (overlapped) rex::system::kernel_state()->CompleteOverlappedImmediate(overlapped, 0);
}
void Publish(uint32_t user) {
  if (enabled[user]) fm4::wheel::SubmitGuestEffects(user, banks[user]);
  else fm4::wheel::StopGuestForces();
}
void WriteBE(uint8_t* bytes, uint32_t value) {
  bytes[0] = uint8_t(value >> 24); bytes[1] = uint8_t(value >> 16);
  bytes[2] = uint8_t(value >> 8); bytes[3] = uint8_t(value);
}
bool GuestRange(uint32_t address, size_t length) {
  // Guest buffers come from FM4. Reject null and overflow before constructing a
  // bounded span, then let the runtime's mapped guest address space handle it.
  return address >= 0x10000 && uint64_t(address) + length <= 0x100000000ull;
}
}

namespace fm4::wheel {
void RefreshGuestCapabilities(uint32_t user) {
  // FM4 checks this flag immediately after GetState. Recapture capabilities
  // in that same frame without manufacturing a disconnected XInput device.
  if(user>=4) return;
  pending_binding_refresh[user]=true;
  if(guest_controllers) guest_controllers[0x10 + user * 0x1600 + 4] = 0;
}
}

// Trace the game's own per-device binding reloads. Capability refresh alone
// does not establish that the higher-level wheel layout has been reapplied.
extern "C" REX_FUNC(__imp__sub_824347D0);
REX_HOOK_RAW(sub_824347D0) {
  static const bool trace_input=std::getenv("FM4_TRACE_INPUT")!=nullptr;
  if(trace_input&&GuestRange(ctx.r3.u32,0x20)) {
    const auto user=fm4::wheel::ReadBE({base+ctx.r3.u32,0x20},0x1c);
    const auto manager=fm4::wheel::ReadBE({base+0x82DC3818,4},0);
    if(user<4&&GuestRange(manager,0x10+4*0x1600))
      REXLOG_INFO("FM4 game binding reload user {}: device type {}",user,
          unsigned(base[manager+0x16+user*0x1600]));
  }
  __imp__sub_824347D0(ctx,base);
}

extern "C" REX_FUNC(__imp__sub_822B58E8);
REX_HOOK_RAW(sub_822B58E8) {
  auto* previous = guest_controllers;
  const auto address = ctx.r3.u32;
  guest_controllers = GuestRange(address, 0x10 + 4 * 0x1600) ? base + address : nullptr;
  // GetState is also queried outside this poll by FM4's profile code. Repair
  // a subtype mismatch here even if the source changed in one of those calls.
  if(guest_controllers) for(uint32_t user=0;user<4;++user) {
    if((guest_controllers[0x16+user*0x1600]==1)!=fm4::wheel::HasWheel(user)) {
      guest_controllers[0x14+user*0x1600]=0;
      pending_binding_refresh[user]=true;
    }
  }
  __imp__sub_822B58E8(ctx, base);
  // FM4 also caches its profile's button/analog layout above XInput. Let the
  // game's normal notification handler reload that layout after capabilities
  // have been refreshed, preserving the user's selected layout/calibration.
  for(uint32_t user=0;guest_controllers&&user<4;++user) if(pending_binding_refresh[user]&&
      (guest_controllers[0x16+user*0x1600]==1)==fm4::wheel::HasWheel(user)&&
      pending_binding_refresh[user].exchange(false)) {
    rex::system::kernel_state()->BroadcastNotification(0x00000012,0);
    rex::system::kernel_state()->BroadcastNotification(0x00000013,user);
    REXLOG_INFO("FM4 input configuration changed for user {}",user);
  }
  static const bool trace_input = std::getenv("FM4_TRACE_INPUT") != nullptr;
  static thread_local uint32_t input_frames = 0;
  if (trace_input && ++input_frames % 300 == 0 && GuestRange(address, 0x58AC)) {
    for (const auto offset : {0x9C, 0x589C}) {
      const std::span<const uint8_t> report(base + address + offset, 16);
      const auto buttons = fm4::wheel::ReadBE(report, 4);
      const auto left = fm4::wheel::ReadBE(report, 8), right = fm4::wheel::ReadBE(report, 12);
      REXLOG_INFO("FM4 input {}: buttons={:04X}, LT={}, RT={}, LX={}, LY={}, RX={}, RY={}",
          offset == 0x9C ? "user0" : "combined", buttons >> 16, (buttons >> 8) & 255, buttons & 255,
          int16_t(left >> 16), int16_t(left), int16_t(right >> 16), int16_t(right));
    }
  }
  guest_controllers = previous;
}

// XInputFFGetDeviceCapabilities(user, caps): FM4 requires five effect slots.
REX_HOOK_RAW(sub_82CA9C08) {
  const uint32_t user = ctx.r3.u32, output = ctx.r4.u32;
  if (!Ready(user)) { ctx.r3.u64 = 0x48F; return; }
  if (!GuestRange(output, 8)) { ctx.r3.u64 = 0x57; return; }
  WriteBE(base + output, 1); WriteBE(base + output + 4, 5);
  ctx.r3.u64 = 0;
}

// Download the game's five original effect descriptions.
REX_HOOK_RAW(sub_82CA9C68) {
  const uint32_t user = ctx.r3.u32, address = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32, overlapped = ctx.r6.u32;
  if (!Ready(user)) { ctx.r3.u64 = 0x48F; return; }
  if (!count || count > 5 || !GuestRange(address, size_t(count) * 0x74)) {
    ctx.r3.u64 = 0x57; return;
  }
  std::lock_guard lock(effects_mutex);
  auto next = banks[user];
  for (uint32_t i = 0; i < count; ++i) {
    const auto decoded = fm4::wheel::DecodeEffect({base + address + i * 0x74, 0x74});
    if (!decoded) { ctx.r3.u64 = 0x57; return; }
    next.slots[decoded->id - 1] = decoded->effect;
  }
  banks[user] = next;
  Complete(overlapped); ctx.r3.u64 = 0;
  REXLOG_INFO("FM4 downloaded {} original wheel effects for user {}", count, user);
}

// Update fields selected by the original Xbox effect flags.
REX_HOOK_RAW(sub_82CA9CC8) {
  const uint32_t user = ctx.r3.u32, address = ctx.r4.u32;
  const uint32_t flags = ctx.r5.u32, overlapped = ctx.r6.u32;
  if (!Ready(user)) { ctx.r3.u64 = 0x48F; return; }
  if (!GuestRange(address, 0x74) || (flags & ~0x500u)) { ctx.r3.u64 = 0x57; return; }
  const auto decoded = fm4::wheel::DecodeEffect({base + address, 0x74});
  if (!decoded) { fm4::wheel::StopGuestForces(); ctx.r3.u64 = 0x57; return; }
  std::lock_guard lock(effects_mutex);
  auto& effect = banks[user].slots[decoded->id - 1];
  if (flags & 0x100) effect.gain = decoded->effect.gain;
  if (flags & 0x400) {
    effect.magnitude = decoded->effect.magnitude;
    effect.condition = decoded->effect.condition;
  }
  Publish(user); Complete(overlapped); ctx.r3.u64 = 0;
  if (decoded->id == 5) peak_torque = std::max(peak_torque, std::abs(effect.magnitude));
  if (++calls <= 3 || calls % 1000 == 0) {
    REXLOG_INFO("FM4 original wheel effect update: id={}, flags={:X}, torque={}, peak={}, updates={}",
        decoded->id, flags, effect.magnitude, peak_torque, calls);
    if (calls % 1000 == 0) peak_torque = 0;
  }
}

// FM4 submits pairs of effect ID and command; command 2 starts an effect.
REX_HOOK_RAW(sub_82CA9D28) {
  const uint32_t user = ctx.r3.u32, address = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32, overlapped = ctx.r7.u32;
  if (!Ready(user)) { ctx.r3.u64 = 0x48F; return; }
  if (!count || count > 5 || !GuestRange(address, size_t(count) * 8)) { ctx.r3.u64 = 0x57; return; }
  std::lock_guard lock(effects_mutex);
  auto next = banks[user];
  for (uint32_t i = 0; i < count; ++i) {
    std::span<const uint8_t> pair(base + address + i * 8, 8);
    const auto id = fm4::wheel::ReadBE(pair, 0), command = fm4::wheel::ReadBE(pair, 4);
    if (id < 1 || id > 5 || command != 2) { ctx.r3.u64 = 0x57; return; }
    next.slots[id - 1].playing = true;
  }
  banks[user] = next; Publish(user); Complete(overlapped); ctx.r3.u64 = 0;
}

// Reset/free all effects. The game reloads its descriptors after this call.
REX_HOOK_RAW(sub_82CA9D90) {
  const uint32_t user = ctx.r3.u32, overlapped = ctx.r4.u32;
  if (!Ready(user)) { ctx.r3.u64 = 0x48F; return; }
  std::lock_guard lock(effects_mutex);
  banks[user] = {}; fm4::wheel::StopGuestForces();
  Complete(overlapped); ctx.r3.u64 = 0;
}

REX_HOOK_RAW(sub_82CA9DF8) {
  const uint32_t user = ctx.r3.u32, command = ctx.r4.u32, overlapped = ctx.r5.u32;
  if (!Ready(user)) { ctx.r3.u64 = 0x48F; return; }
  if (command != 0) { ctx.r3.u64 = 0x57; return; }
  fm4::wheel::StopGuestForces(); Complete(overlapped); ctx.r3.u64 = 0;
}

// The wheel's two rumble motors carry FM4's engine, kerb and surface detail.
// A force-feedback wheel has no motors, so the host plays them as small
// steering-axis vibrations (force.detail). This also avoids the runtime's
// unimplemented Xbox motor-rumble path every input frame.
REX_HOOK_RAW(sub_82CA9E68) {
  const uint32_t user = ctx.r3.u32, address = ctx.r4.u32, overlapped = ctx.r5.u32;
  if (!Ready(user)) { ctx.r3.u64 = 0x48F; return; }
  if (!GuestRange(address, 4)) { ctx.r3.u64 = 0x57; return; }
  const auto motors = fm4::wheel::ReadBE({base + address, 4}, 0);
  static const bool trace = std::getenv("FM4_TRACE_INPUT") != nullptr;
  static uint32_t traced = 0;
  {
    std::lock_guard lock(effects_mutex);
    auto& bank = banks[user];
    const float low = float(motors >> 16) / 65535.0f, high = float(motors & 0xFFFF) / 65535.0f;
    if (low != bank.rumble_low || high != bank.rumble_high) {
      bank.rumble_low = low; bank.rumble_high = high;
      if (trace && traced < 4000) {
        ++traced;
        REXLOG_INFO("FM4 wheel rumble request: left={}, right={}", motors >> 16, motors & 0xFFFF);
      }
      Publish(user);
    }
  }
  Complete(overlapped); ctx.r3.u64 = 0;
}

// CCarPresentationDeferred::SetSteeringWheelAngle(angle, axis, f2). FM4 clamps the
// cockpit angle to +/-90 degrees and draws only a fraction of it, so the in-game wheel
// stops long before a 900-degree wheel does. For presentations that are following the
// live wheel (both views of the player's car), pass the physical angle instead. A
// presentation is locked once it has tracked the input for a while and released only
// when it clearly contradicts it (replays); everything else keeps the game's own angle.
extern "C" REX_FUNC(__imp__sub_822E6590);
REX_HOOK_RAW(sub_822E6590) {
  using Clock = std::chrono::steady_clock;
  struct Track {
    int votes = 0, misses = 0, blend = 0;
    bool locked = false; uint32_t seen = 0;
    double vote_seconds = 0, miss_seconds = 0, blend_seconds = 0;
    Clock::time_point last{};
  };
  static std::map<uint32_t, Track> tracks;
  static uint32_t clock = 0;
  float steering = 0, rotation_scale = 1, rotation_degrees = 900;
  if (fm4::wheel::CockpitSteering(steering, rotation_scale, rotation_degrees)) {
    const uint32_t self = ctx.r3.u32;
    const double game = ctx.f1.f64;
    const bool timed = REXCVAR_GET(fm4_time_based_wheel_visuals);
    const auto now = timed ? Clock::now() : Clock::time_point{};
    ++clock;
    if (tracks.size() > 64)
      std::erase_if(tracks, [&](const auto& entry) {
        return timed ? now - entry.second.last > std::chrono::seconds(10) :
                       clock - entry.second.seen > 600;
      });
    auto& track = tracks[self];
    const double elapsed = !timed || track.last == Clock::time_point{} ? 0.0 :
        std::clamp(std::chrono::duration<double>(now - track.last).count(), 0.0, 0.1);
    if (timed) track.last = now;
    track.seen = clock;
    const bool follows = fm4::wheel::TracksSteering(game, steering);
    if (track.locked) {
      track.misses = fm4::wheel::ContradictsSteering(game, steering) ? track.misses + 1 : 0;
      track.miss_seconds = track.misses ? track.miss_seconds + elapsed : 0;
      if (timed ? track.miss_seconds > 2.0 : track.misses > 120) {
        REXLOG_INFO("FM4 cockpit wheel released presentation {:08X}", self);
        track = {}; track.seen = clock;
      }
    } else {
      track.votes = follows && std::abs(steering) > 0.05f ? track.votes + 1 : 0;
      track.vote_seconds = track.votes ? track.vote_seconds + elapsed : 0;
      if (timed ? track.vote_seconds >= 20.0 / 60.0 : track.votes >= 20) {
        track.locked = true;
        REXLOG_INFO("FM4 cockpit wheel drawn 1:1 ({} degrees) for presentation {:08X}", rotation_degrees, self);
      }
    }
    if (track.locked) {
      // Drawing scale tunable in FM4's .data (0.75 in stock data).
      const float draw_scale = std::bit_cast<float>(fm4::wheel::ReadBE({base + 0x82D92C94u, 4}, 0));
      const double target = fm4::wheel::OneToOneWheelAngle(steering, rotation_scale, rotation_degrees,
          draw_scale > 0.05f && draw_scale < 10.0f ? draw_scale : 0.75f);
      // Preserve the legacy 12-frame ease, or use its 0.2-second duration.
      track.blend = std::min(track.blend + 1, 12);
      track.blend_seconds = std::min(track.blend_seconds + elapsed, 0.2);
      const double blend = timed ? track.blend_seconds / 0.2 : track.blend / 12.0;
      ctx.f1.f64 = game + (target - game) * blend;
    }
    static const bool trace = std::getenv("FM4_TRACE_INPUT") != nullptr;
    static uint32_t traced = 0;
    if (trace && traced < 20000) {
      ++traced;
      REXLOG_INFO("FM4 cockpit angle: clock={}, presentation={:08X}, input={:.4f}, game={:.4f}, drawn={:.4f}, locked={}",
          clock, self, steering, game, ctx.f1.f64, track.locked ? 1 : 0);
    }
  }
  __imp__sub_822E6590(ctx, base);
}

// FM4 shapes its wheel force here once per frame: r4 = {steering torque, speed m/s,
// rear slip} (before DynFFBScale and the +/-127 device level), r25 = the car's
// physics object, whose four wheel states start at +0xEB0 (0x420 apart). Sample them
// for the telemetry force mode; the game's own force path is left unchanged.
extern "C" REX_FUNC(__imp__sub_8239E1D0);
REX_HOOK_RAW(sub_8239E1D0) {
  const uint32_t input = ctx.r4.u32, physics = ctx.r25.u32;
  if (GuestRange(input, 12)) {
    const auto real = [&](uint32_t address) {
      return std::bit_cast<float>(fm4::wheel::ReadBE({base + address, 4}, 0));
    };
    fm4::wheel::Telemetry telemetry;
    telemetry.torque = real(input); telemetry.speed = real(input + 4); telemetry.rear_slip = real(input + 8);
    if (GuestRange(physics, 0xEB0 + 4 * 0x420)) {
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t wheel = physics + 0xEB0 + i * 0x420;
        telemetry.slip[i] = real(wheel + 0x228); telemetry.load[i] = real(wheel + 0x2C8);
        if (fm4::wheel::ReadBE({base + wheel + 0x33C, 4}, 0)) telemetry.contact |= uint8_t(1u << i);
        telemetry.surface[i] = base[wheel + 0x141];
      }
    }
    fm4::wheel::SubmitTelemetry(telemetry);
    static const bool trace = std::getenv("FM4_TRACE_INPUT") != nullptr;
    static uint32_t calls = 0;
    if (trace && ++calls % 2 == 0 && calls < 60000)
      REXLOG_INFO("FM4 telemetry: car={:08X}, controller={:08X}, torque={:.4f}, speed={:.2f}, rear_slip={:.3f}, "
          "slip={:.3f}/{:.3f}/{:.3f}/{:.3f}, load={:.0f}/{:.0f}/{:.0f}/{:.0f}, contact={:X}, surface={}/{}/{}/{}",
          physics, ctx.r3.u32, telemetry.torque, telemetry.speed, telemetry.rear_slip,
          telemetry.slip[0], telemetry.slip[1], telemetry.slip[2], telemetry.slip[3],
          telemetry.load[0], telemetry.load[1], telemetry.load[2], telemetry.load[3], telemetry.contact,
          telemetry.surface[0], telemetry.surface[1], telemetry.surface[2], telemetry.surface[3]);
  }
  __imp__sub_8239E1D0(ctx, base);
}

// Runs in FM4's wheel update, before it submits force/gain updates. Pausing or
// disabling the game's force output suppresses every effect, including springs.
void Fm4WheelActive(PPCRegister& r3) {
  auto* memory = rex::system::kernel_state()->memory();
  const auto address = r3.u32;
  if (!GuestRange(address, 6)) return;
  auto* object = memory->TranslateVirtual<uint8_t*>(address);
  const uint32_t user = fm4::wheel::ReadBE({object, 6}, 0);
  if (user >= 4) return;
  std::lock_guard lock(effects_mutex);
  enabled[user] = object[5] != 0;
  if (!enabled[user]) fm4::wheel::StopGuestForces();
}
