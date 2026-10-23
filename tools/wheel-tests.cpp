// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#include "../fm4/src/wheel_math.h"
#include "../fm4/src/wheel_host.h"
#include "../fm4/src/wheel_native_ffb.h"
#include "../fm4/src/wheel_protocol.h"
#include "../fm4/src/input_policy.h"
#include "sdk/dualsense_effect.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <cstdlib>
#include <windows.h>
#include <vector>

struct FakeForceDevice {
  DWORD flags = DIGFFS_ACTUATORSON;
  HRESULT query_result = S_OK, command_result = S_OK;
  bool commands_change_state = true;
  std::vector<DWORD> commands;
  HRESULT GetForceFeedbackState(DWORD* output) { *output = flags; return query_result; }
  HRESULT SendForceFeedbackCommand(DWORD command) {
    commands.push_back(command);
    if (FAILED(command_result)) return command_result;
    if (commands_change_state) {
      if (command == DISFFC_CONTINUE) flags &= ~DIGFFS_PAUSED;
      if (command == DISFFC_SETACTUATORSON) { flags &= ~DIGFFS_ACTUATORSOFF; flags |= DIGFFS_ACTUATORSON; }
    }
    return S_OK;
  }
};

int main() {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
  using namespace fm4::wheel;
  {
    DWORD flags; bool known;
    FakeForceDevice device;
    assert(PrepareForceActuators(device, flags, known) == ForceDeviceResult::Ready && known);
    assert(device.commands.empty());
    device.flags = DIGFFS_PAUSED | DIGFFS_ACTUATORSOFF;
    assert(PrepareForceActuators(device, flags, known) == ForceDeviceResult::Ready);
    assert((device.commands == std::vector<DWORD>{DISFFC_CONTINUE, DISFFC_SETACTUATORSON}));
    for (DWORD blocked : std::array<DWORD, 4>{DIGFFS_USERFFSWITCHOFF, DIGFFS_SAFETYSWITCHOFF, DIGFFS_POWEROFF, DIGFFS_DEVICELOST}) {
      device = {}; device.flags = blocked | DIGFFS_ACTUATORSOFF;
      assert(PrepareForceActuators(device, flags, known) != ForceDeviceResult::Ready);
      assert(device.commands.empty());
    }
    device = {}; device.query_result = E_NOTIMPL;
    assert(PrepareForceActuators(device, flags, known) == ForceDeviceResult::Ready && !known);
    device = {}; device.flags = DIGFFS_PAUSED; device.command_result = E_FAIL;
    assert(PrepareForceActuators(device, flags, known) == ForceDeviceResult::ResumeFailed);
    device = {}; device.flags = DIGFFS_ACTUATORSOFF; device.command_result = E_FAIL;
    assert(PrepareForceActuators(device, flags, known) == ForceDeviceResult::EnableFailed);
    device = {}; device.flags = DIGFFS_ACTUATORSOFF; device.commands_change_state = false;
    assert(PrepareForceActuators(device, flags, known) == ForceDeviceResult::Muted);
  }
  assert(AutoSelectRank(false, true, true, true) == 0);
  assert(AutoSelectRank(true, false, true, true) == 1);
  assert(AutoSelectRank(true, true, true, true) == 2);
  assert(AutoSelectRank(true, true, false, true) == 2);
  assert(AutoSelectRank(true, true, true, false) == 0);
  assert(ControllerAxis(-512, .06f) == 0 && ControllerAxis(512, .06f) == 0);
  assert(ControllerAxis(-32768, .06f) == -32768 && ControllerAxis(32767, .06f) == 32767);
  assert(ControllerAxis(8192, .06f) > 0 && ControllerAxis(8192, .06f) < 8192);
  assert(ControllerAxis(-8192, .06f) < 0 && ControllerAxis(-8192, .06f) > -8192);
  assert(ControllerAxis(1234, 0) == 1234 && ControllerAxis(-1234, 0) == -1234);
  assert(ControllerAxis(32767, std::nanf("")) == 0);
  InputSelection selection;
  Activity w{true}, p{true};
  assert(selection.Update(w, p) == InputSource::Controller);
  w.axes[0] = .01f;
  assert(selection.Update(w, p) == InputSource::Controller);
  w.axes[0] = .05f;
  assert(selection.Update(w, p) == InputSource::Wheel);
  p.axes[0] = .03f;
  assert(selection.Update(w, p) == InputSource::Wheel);
  p.buttons = 0x1000;
  assert(selection.Update(w, p) == InputSource::Controller);
  assert(selection.Update(w, p) == InputSource::Controller);
  // An already held inactive pedal cannot repeatedly steal control.
  w.axes[1] = 1;
  assert(selection.Update(w, p) == InputSource::Wheel);
  p.buttons = 0x2000;
  assert(selection.Update(w, p) == InputSource::Controller);
  assert(selection.Update(w, p) == InputSource::Controller);
  p.connected = false;
  assert(selection.Update(w, p) == InputSource::Wheel);
  p.connected = true;
  assert(selection.Update(w, p) == InputSource::Wheel);
  p.buttons = 0x10;
  assert(selection.Update(w, p) == InputSource::Controller);
  w.connected = p.connected = false;
  assert(selection.Update(w, p) == InputSource::None);
  // Holding a pedal during handoff must not steal control. Releasing and
  // pressing it to exactly the old position must still select the wheel.
  InputSelection rearm;
  Activity rw{true},rp{true};
  assert(rearm.Update(rw,rp)==InputSource::Controller);
  rw.axes[1]=1; assert(rearm.Update(rw,rp)==InputSource::Wheel);
  rp.buttons=0x1000; assert(rearm.Update(rw,rp)==InputSource::Controller);
  assert(rearm.Update(rw,rp)==InputSource::Controller);
  rw.axes[1]=0; assert(rearm.Update(rw,rp)==InputSource::Controller);
  rw.axes[1]=1; assert(rearm.Update(rw,rp)==InputSource::Wheel);
  // The same release/repress sequence applies to a controller trigger.
  rp.axes[4]=1; assert(rearm.Update(rw,rp)==InputSource::Controller);
  rw.axes[0]=.5f; assert(rearm.Update(rw,rp)==InputSource::Wheel);
  rp.axes[4]=0; assert(rearm.Update(rw,rp)==InputSource::Wheel);
  rp.axes[4]=1; assert(rearm.Update(rw,rp)==InputSource::Controller);
  auto effect = fm4::controller::TriggerEffect(65535, 65535, 255, 255, 1);
  assert(effect.size() == 47 && effect[0] == 0x0C);
  assert(effect[10] == 1 && effect[12] == 110 && effect[21] == 1 && effect[23] == 110);
  effect = fm4::controller::TriggerEffect(65535, 65535, 0, 0, 1);
  assert(effect[10] == 5 && effect[21] == 5);
  effect = fm4::controller::TriggerEffect(65535, 65535, 255, 255, 0);
  assert(effect[10] == 5 && effect[21] == 5);
  effect = fm4::controller::TriggerEffect(65535, 65535, 255, 255, std::nanf(""));
  assert(effect[10] == 5 && effect[21] == 5);
  effect = fm4::controller::TriggerEffect(0, 0, 255, 255, 1);
  assert(effect[10] == 5 && effect[21] == 5);
  effect = fm4::controller::FeedbackEffect(65535, 65535, 255, 255, 1, 1);
  assert(effect[0] == 0x0F && effect[2] == 127 && effect[3] == 127);
  assert(effect[10] == 1 && effect[21] == 1);
  effect = fm4::controller::FeedbackEffect(65535, 65535, 255, 255, 0, 1);
  assert(effect[0] == 0x0C && effect[2] == 0 && effect[3] == 0 && effect[10] == 1);
  effect = fm4::controller::FeedbackEffect(65535, 65535, 255, 255, 1, 0);
  assert(effect[0] == 0x0F && effect[2] == 127 && effect[10] == 5 && effect[21] == 5);
  effect = fm4::controller::FeedbackEffect(65535, 65535, 255, 255, std::nanf(""), 0);
  assert(effect[0] == 0x0C && effect[2] == 0 && effect[3] == 0);
  Calibration axis;
  assert(axis.Apply(0, true) == -1);
  assert(axis.Apply(32767, true) == 0);
  assert(axis.Apply(65535, true) == 1);
  assert(axis.Apply(-999, true) == -1);
  axis.inverted = true;
  assert(axis.Apply(0, false) == 1);
  assert(axis.Apply(65535, false) == 0);
  axis.deadzone = 0.1f; axis.saturation = 0.9f;
  assert(axis.Apply(32767, true) == 0);
  axis.center = axis.minimum;
  assert(!axis.Valid(true)); assert(axis.Apply(12000, true) == 0);
  axis.gamma = std::numeric_limits<float>::quiet_NaN();
  assert(!axis.Valid(false));
  assert(Steering(-1) == -32768 && Steering(1) == 32767);
  assert(Pedal(0) == 0 && Pedal(1) == 255);
  assert(Steering(std::numeric_limits<float>::infinity()) == 0);
  std::array<bool, 9> contacts{};
  assert(SelectGear(contacts, 7) == 0);
  contacts[0] = true; assert(SelectGear(contacts, 7) == -1);
  contacts[0] = false; contacts[7] = true; assert(SelectGear(contacts, 7) == 7);
  contacts[3] = true; assert(SelectGear(contacts, 7) == 0);
  contacts[7] = false; assert(SelectGear(contacts, 7) == 3);
  contacts[3] = false; contacts[8] = true; assert(SelectGear(contacts, 7) == 0);
  ButtonPulses pulses;
  pulses.Press(0x1001, 1000);
  assert(pulses.Read(1050) == 0x1001);
  pulses.Press(0x2000, 1050);
  assert(pulses.Read(1100) == 0x2000);
  pulses.Clear(); assert(pulses.Read(1101) == 0);
  assert(Forces{}.Valid());
  assert(!(Forces{1.1f, 0, 0, 0}.Valid()));
  assert(!(Forces{0, 0, -0.1f, 0}.Valid()));
  assert(CanApplyForces(true, true, true, 1000, 990, 150));
  assert(!CanApplyForces(true, true, true, 1140, 990, 150));
  assert(!CanApplyForces(false, true, true, 1000, 990, 150));
  assert(!CanApplyForces(true, false, true, 1000, 990, 150));
  assert(!CanApplyForces(true, true, false, 1000, 990, 150));
  assert(!CanApplyForces(true, true, true, 1000, 0, 150));
  assert(!CanApplyForces(true, true, true, 900, 990, 150));
  // Device enumeration started at 1000; a guest update arrived at 1020.
  // The worker must sample its watchdog clock after the request snapshot.
  assert(!CanApplyForces(true, true, true, 1000, 1020, 150));
  assert(CanApplyForces(true, true, true, 1021, 1020, 150));
  assert(!CanApplyForces(true, true, true, 1170, 1020, 150));
  Motion motion;
  motion.Update(.8f, 1000, .1f, .01f);
  assert(motion.velocity == 0 && motion.acceleration == 0);
  motion.Update(-.8f, 1004, .1f, .01f);
  assert(motion.velocity < 0 && motion.velocity >= -1 && motion.acceleration >= -1);
  motion.Update(.5f, 1500, .1f, .01f);
  assert(motion.velocity == 0 && motion.acceleration == 0);
  motion.Update(std::nanf(""), 1504, .1f, .01f);
  assert(motion.previous_ms == 0);
  EffectFrame fallback;
  for (auto& slot : fallback.slots) {
    slot.type = EffectType::Constant; slot.gain = 1; slot.magnitude = 1; slot.playing = true;
  }
  assert(MixSoftwareForces(fallback, motion, .15f) == .15f);
  for (auto& slot : fallback.slots) slot.magnitude = -1;
  assert(MixSoftwareForces(fallback, motion, .15f) == -.15f);
  fallback.slots[0].gain = std::nanf("");
  assert(MixSoftwareForces(fallback, motion, 1) == 0);
  Condition condition{.1f, -.5f, -.25f, .2f, .1f, .05f};
  assert(ConditionForce(condition, .1f) == 0);
  assert(ConditionForce(condition, 1) == -.2f);
  assert(ConditionForce(condition, -1) == .1f);
  assert(ConditionForce(condition, std::nanf("")) == 0);
  assert(ConditionForce(condition, .5f, true) == -.2f);
  // FM4 torque is positive toward right steer; DirectInput positive pushes left.
  assert(HostTorque(.5f, false) == -.5f && HostTorque(.5f, true) == .5f);
  {
    // Weight 1 is stock; higher weight raises small torque, never clips, keeps sign.
    assert(SteeringWeightCurve(.2f, 1) == .2f && SteeringWeightCurve(2, 1) == 1);
    assert(std::abs(SteeringWeightCurve(.4f, 2.5f) - .625f) < 1e-6f);
    assert(SteeringWeightCurve(.8f, 2.5f) - SteeringWeightCurve(.6f, 2.5f) > .11f);
    assert(SteeringWeightCurve(NAN, 2.5f) == 0 && SteeringWeightCurve(.5f, NAN) == 0);
    for (float weight : {1.0f, 1.2f, 2.5f, 5.0f}) {
      assert(SteeringWeightCurve(1, weight) == 1 && SteeringWeightCurve(-1, weight) == -1);
      assert(std::abs(SteeringWeightCurve(.00001f, weight) / .00001f - weight) < .001f);
      for (int i = 0; i < 1000; ++i) {
        const float x = float(i) / 1000, a = SteeringWeightCurve(x, weight), b = SteeringWeightCurve(x + .001f, weight);
        assert(a >= 0 && b <= 1 && b > a);
        assert(std::abs(SteeringWeightCurve(-x, weight) + a) < 1e-6f);
        assert(b - a >= .001f / weight - 1e-6f);
      }
    }
    EffectFrame shaped;
    shaped.slots[3].type = EffectType::Spring; shaped.slots[3].condition.positive = .5f; shaped.slots[3].condition.negative = .5f;
    shaped.slots[2].type = EffectType::Damper; shaped.slots[2].condition.positive = .8f; shaped.slots[2].condition.negative = .8f;
    shaped.slots[4].type = EffectType::Constant; shaped.slots[4].magnitude = -.2f;
    ShapeForces(shaped, {2.5f, .4f, 2});
    assert(std::abs(shaped.slots[3].condition.positive - .2f) < 1e-6f && shaped.slots[2].condition.negative == 1);
    assert(std::abs(shaped.slots[4].magnitude + .3846154f) < 1e-6f);
    EffectFrame damper; damper.slots[2].type = EffectType::Damper;
    damper.slots[2].condition.positive = damper.slots[2].condition.negative = .1f;
    ShapeForces(damper, ForceShape{});
    assert(std::abs(damper.slots[2].condition.positive - .025f) < 1e-6f);
    // Telemetry torque: invalid samples are rejected, and the smoother converges
    // without overshoot and restarts cleanly.
    Telemetry bad; bad.torque = NAN; assert(!bad.Valid());
    Telemetry good; good.torque = .3f; good.speed = 30; assert(good.Valid());
    TorqueSmoother smooth;
    assert(smooth.Update(.2f, 1000) == 0);
    assert(smooth.Update(.6f, 1000) == 0);
    float previous = 0;
    for (uint64_t t = 1004; t < 1100; t += 4) {
      const float v = smooth.Update(.6f, t);
      assert(v >= previous && v <= .6f + 1e-6f); previous = v;
    }
    assert(std::abs(previous - .6f) < .01f);
    smooth.Reset(); assert(smooth.Update(0, 2000) == 0);
    const float first_tick = smooth.Update(.6f, 2004);
    assert(first_tick > .37f && first_tick < .39f);
    assert(smooth.Update(.6f, 2008) > .51f);
    assert(smooth.Update(.6f, 2016) > .58f);
    // Switching to game fallback keeps the current output at the switch instant.
    const float before_switch = smooth.value;
    assert(smooth.Update(-.6f, 2016, 16) == before_switch);
    const float fallback_tick = smooth.Update(-.6f, 2020, 16);
    assert(fallback_tick < before_switch && fallback_tick > -.6f);
    assert(smooth.Update(-1, 2024, 4) > -1);
    assert(smooth.Update(.5f, 3000) == 0 && smooth.Update(.5f, 2999) == 0);
    assert(std::isfinite(smooth.Update(NAN, 3003, 0)));
    assert(std::isfinite(smooth.Update(5, 3007, NAN)) && smooth.value <= 1);
    // Road texture reacts to changes in front load difference, not to steady load transfer.
    RoadTexture road; Telemetry level; level.load = {40, 40, 40, 40};
    assert(road.Update(level, 1000) == 0 && road.Update(level, 1016) == 0);
    Telemetry bump = level; bump.load = {50, 40, 40, 40};
    const float kick = road.Update(bump, 1032); assert(kick > .05f && kick <= 1);
    float settled = kick;
    for (uint64_t t = 1048; t < 1400; t += 16) settled = road.Update(bump, t);
    assert(std::abs(settled) < .01f);
    Telemetry mirrored = level; mirrored.load = {40, 50, 40, 40};
    RoadTexture other; other.Update(level, 1000); other.Update(level, 1016);
    assert(other.Update(mirrored, 1032) < -.05f);
    // Road detail ramps over 8 ms; retargeting preserves continuity, even within a tick.
    FrameInterpolator frames;
    frames.Push(0, 1000);
    frames.Push(.6f, 1016);
    assert(frames.Sample(1016) == 0 && std::abs(frames.Sample(1020) - .3f) < 1e-6f && std::abs(frames.Sample(1024) - .6f) < 1e-6f);
    frames.Push(.9f, 1020);
    assert(std::abs(frames.Sample(1020) - .3f) < 1e-6f);
    frames.Push(.2f, 1020);
    assert(std::abs(frames.Sample(1020) - .3f) < 1e-6f);
    float last = frames.Sample(1020);
    for (uint64_t t = 1020; t <= 1028; t += 2) {
      const float v = frames.Sample(t); assert(v <= last + 1e-5f && v >= .2f - 1e-5f); last = v;
    }
    frames.Push(.9f, 2000);
    assert(frames.Sample(2000) == 0 && frames.Sample(2008) == .9f);
    frames.Push(.4f, 1999); assert(frames.Sample(1999) == 0);
    frames.Reset(); assert(frames.Sample(3000) == 0);
    frames.Push(NAN, 3000); assert(frames.Sample(3008) == 0);
    // Shift and impact thumps on the low motor are weighted down.
    EffectFrame thump; thump.rumble_low = 1; thump.rumble_high = 0;
    float peak = 0; for (double s = 0; s < .2; s += .001) peak = std::max(peak, std::abs(RumbleForce(thump, s, 1)));
    assert(peak <= kRumbleLowWeight + 1e-4f);
    EffectFrame stock = shaped; ShapeForces(stock, {1, 1, 1});
    assert(stock.slots[3].condition.positive == shaped.slots[3].condition.positive && stock.slots[4].magnitude == shaped.slots[4].magnitude);
  }
  // Cockpit wheel: FM4's own angle tracks input x 450 degrees, clamped at 90 degrees.
  assert(TracksSteering(.1 * kGuestWheelRadiansPerInput, .1) && TracksSteering(kGuestWheelClamp, .5));
  assert(TracksSteering(.05 * kGuestWheelRadiansPerInput * 2.25, .05));    // speed-scaled
  assert(!TracksSteering(-.5, .2) && !TracksSteering(.05, .3) && TracksSteering(0, 0) && !TracksSteering(.5, 0));
  // Filter lag and speed scaling never release a lock; opposite steering or a replay does.
  assert(!ContradictsSteering(.05, .3) && !ContradictsSteering(1.5, .1) && !ContradictsSteering(.2, 0));
  assert(ContradictsSteering(-.5, .3) && ContradictsSteering(.8, 0) && ContradictsSteering(NAN, .2));
  // 1:1 drawing: a 900-degree base at half lock is 225 degrees; the renderer draws 0.75x.
  assert(std::abs(OneToOneWheelAngle(.5, 1, 900, .75) * .75 - 3.926990817) < 1e-6);
  assert(std::abs(OneToOneWheelAngle(.6, 1.2, 1080, .75) * .75 - 4.71238898) < 1e-6);  // 1080 base via rotation_scale
  assert(OneToOneWheelAngle(.5, 1, 900, 0) == 0);
  // Rumble detail: bounded, scaled by the detail setting, silent at zero.
  {
    RumbleDetail detail;
    auto motors = detail.Update(.6f, .4f, 1000);
    assert(std::abs(motors[0] - .06f) < 1e-6f && std::abs(motors[1] - .04f) < 1e-6f);
    for (uint64_t t = 1004; t <= 2000; t += 4) motors = detail.Update(.6f, .4f, t);
    assert(std::abs(motors[0] - .06f) < 1e-6f);
    motors = detail.Update(.9f, .4f, 2004);
    assert(motors[0] > .35f && std::abs(motors[1] - .04f) < 1e-6f);
    // Falling amplitude and silence must not turn into extra vibration.
    motors = detail.Update(.3f, 0, 2008);
    assert(std::abs(motors[0] - .03f) < 1e-6f && motors[1] == 0);
    motors = detail.Update(0, 0, 2012); assert(motors[0] == 0 && motors[1] == 0);
    detail.Reset(); detail.Update(0, 0, 3000);
    motors = detail.Update(.8f, 1, 3004, 0);
    assert(motors[0] > .78f && motors[1] > .98f);
    for (uint64_t t = 3008; t < 6500; t += 4) motors = detail.Update(.8f, 1, t, 0);
    assert(motors[0] < .001f && motors[1] < .001f);
    motors = detail.Update(.4f, .7f, 6500, 1);
    assert(motors[0] == .4f && motors[1] == .7f);
    motors = detail.Update(.8f, .6f, 8000);
    assert(std::abs(motors[0] - .08f) < 1e-6f);
    motors = detail.Update(.4f, .2f, 7999);
    assert(std::abs(motors[0] - .04f) < 1e-6f);
    motors = detail.Update(NAN, 0, 8003); assert(motors[0] == 0 && motors[1] == 0);
    for (uint64_t t = 8004; t < 9000; t += 4) {
      const float input = float(t % 37) / 36;
      motors = detail.Update(input, 1 - input, t);
      assert(motors[0] >= 0 && motors[0] <= input && motors[1] >= 0 && motors[1] <= 1 - input);
    }
    EffectFrame rumble;
    assert(rumble.Valid() && RumbleForce(rumble, .3, 1) == 0);
    rumble.rumble_low = 1; rumble.rumble_high = 1;
    for (double t = 0; t < 1; t += .001) assert(std::abs(RumbleForce(rumble, t, .25f)) <= .5f + 1e-6f);
    assert(std::abs(RumbleForce(rumble, 1.0 / 48, .25f)) > 0);
    rumble.rumble_high = 1.5f;
    assert(!rumble.Valid());
  }
  std::array<uint8_t, 0x74> bytes{};
  auto write = [&](size_t offset, uint32_t value) {
    bytes[offset] = uint8_t(value >> 24); bytes[offset + 1] = uint8_t(value >> 16);
    bytes[offset + 2] = uint8_t(value >> 8); bytes[offset + 3] = uint8_t(value);
  };
  write(0, 5); write(4, 1); write(0xC, 255); write(0x2C, uint32_t(-127));
  auto decoded = DecodeEffect(bytes);
  assert(decoded && decoded->effect.magnitude == -1 && decoded->effect.gain == 1);
  write(0x2C, 128); assert(!DecodeEffect(bytes));
  write(0x2C, 0); write(4, 999); assert(!DecodeEffect(bytes));
  assert(!DecodeEffect(std::span(bytes).first(0x73)));
  constexpr uint32_t types[] = {10, 11, 9, 8};
  for (uint32_t i = 0; i < 4; ++i) {
    write(0, i + 1); write(4, types[i]); write(0x30, 127); write(0x34, uint32_t(-127));
    write(0x38, 255); write(0x3C, 128); write(0x40, 0);
    decoded = DecodeEffect(bytes);
    assert(decoded && size_t(decoded->effect.type) == i);
    assert(decoded->effect.condition.positive == 1 && decoded->effect.condition.negative == -1);
    assert(decoded->effect.condition.positive_limit == 1);
  }
  write(0x38, 256); assert(!DecodeEffect(bytes));
  const auto profile = std::filesystem::temp_directory_path() / "fm4-wheel-test.ini";
  std::filesystem::remove(profile);
  WriteDefaultConfig(profile);
  const auto config = LoadConfig(profile);
  assert(config.enabled && config.ffb_enabled && config.gain == 0.4f);
  assert(config.engine_gain == .1f && config.shape.damping == .25f);
  assert(Config{}.engine_gain == config.engine_gain && Config{}.shape.damping == config.shape.damping);
  assert(config.steering.axis == "Wheel Axis" && config.brake.axis == "Brake");
  assert(config.buttons[12] == 2 && config.buttons[0] == 0);
  assert(config.buttons[4] == 10 && config.buttons[5] == 9);
  assert(!config.shifter_enabled && config.shifter_gears == 7);
  assert(config.stick_deadzone == .06f);
  assert(config.force_backend == "auto" && config.velocity_scale == .1f && config.acceleration_scale == .01f);
  // Existing user settings take precedence, including an explicit force-off
  // choice. Re-running first-launch setup must never replace that choice.
  { std::ofstream disabled(profile); disabled << "[force]\nenabled=0\ngain=0.32\n"; }
  WriteDefaultConfig(profile);
  const auto disabled = LoadConfig(profile);
  assert(!disabled.ffb_enabled && disabled.gain == .32f);
  assert(!disabled.invert_force && disabled.detail_gain == .5f);
  assert(disabled.shape.steering_weight == 3.5f && disabled.shape.centering == 0 && disabled.shape.damping == .25f);
  assert(disabled.engine_gain == .1f);
  assert(disabled.road_gain == .3f);
  assert(disabled.telemetry_force);
  assert(disabled.full_rotation && disabled.rotation_degrees == 900);
  { std::ofstream tuned(profile); tuned << "[force]\ninvert=1\ndetail=0.6\nengine=0.25\nsteering_weight=1\ncentering=1\ndamping=0.5\n"; }
  const auto tuned = LoadConfig(profile);
  assert(tuned.invert_force && tuned.detail_gain == .6f);
  assert(tuned.engine_gain == .25f);
  assert(tuned.shape.steering_weight == 1 && tuned.shape.centering == 1 && tuned.shape.damping == .5f);
  { std::ofstream mode(profile); mode << "[force]\nmode=game\n"; }
  assert(!LoadConfig(profile).telemetry_force);
  for (float engine : {0.0f, 1.0f}) {
    { std::ofstream out(profile); out << "[force]\nengine=" << engine << "\ndamping=1\n"; }
    const auto legacy = LoadConfig(profile);
    assert(legacy.engine_gain == engine && legacy.shape.damping == 1);
  }
  for (const char* bad : {"[force]\nmode=fast\n", "[force]\nsteering_weight=0.5\n", "[force]\ncentering=1.5\n", "[force]\ndamping=3\n",
                         "[force]\nengine=-0.1\n", "[force]\nengine=1.1\n", "[force]\nengine=nan\n", "[force]\nengine=inf\n"}) {
    { std::ofstream out(profile); out << bad; }
    bool refused = false;
    try { LoadConfig(profile); } catch (const std::runtime_error&) { refused = true; }
    assert(refused);
  }
  { std::ofstream bad(profile); bad << "[force]\ndetail=2\n"; }
  bool rejected = false;
  try { LoadConfig(profile); } catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
  std::filesystem::remove(profile);
  std::puts("Wheel calibration, report limits, invalid force, watchdog and profile tests passed.");
}
