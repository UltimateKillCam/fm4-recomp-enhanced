// Scoped FM4 startup-media hooks.

#include <rex/ppc.h>

void EnableTurn10MovieSkip(PPCRegister& r29) {
  // The following store writes descriptor + 0x28.
  r29.u64 = 1;
}

void RestoreTurn10MovieMode(PPCRegister& r29) {
  // Do not change the mode used by later movie descriptors.
  r29.u64 = 2;
}

void EnableClarksonVoiceoverSkip(PPCRegister& r3) {
  // The previous instruction loaded the allowskipvoiceover byte into r3.
  r3.u64 = 1;
}

// Track streaming memory: FM4 sizes its main asset pool from a settings value (124 MB x 1.08).
// With some random AI fields the race-start streaming exceeds it, the loaders get no memory
// and silently drop track assets (missing Bernese Alps road sections for the whole race).
// The PC build has spare guest physical memory, so give that pool more budget. At higher
// frame rates 132 MB still runs out during track loading and roadside boards keep their
// 32x32 placeholder; 160 MB avoids that. Applies only to the stock value. Added 2026-10-19.
#include <rex/hook.h>
#include <rex/logging.h>

#include <rex/cvar.h>
REXCVAR_DEFINE_INT32(fm4_streaming_asset_pool_mb, 160, "Forza Motorsport 4",
                    "Guest streaming asset pool in MB (132-160)")
    .range(132, 160)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

extern "C" REX_FUNC(__imp__sub_8237CFD8);
REX_HOOK_RAW(sub_8237CFD8) {
  __imp__sub_8237CFD8(ctx, base);
  static bool applied = false;
  if (applied || !ctx.r3.u32) return;
  uint8_t* budget = base + ctx.r3.u32 + 1640;
  const uint32_t value = uint32_t(budget[0]) << 24 | uint32_t(budget[1]) << 16 |
                         uint32_t(budget[2]) << 8 | budget[3];
  if (value != 0x07C00000u) return;  // settings not populated yet, or not the stock budget
  const int requested_mb = REXCVAR_GET(fm4_streaming_asset_pool_mb);
  const uint32_t pool_mb = uint32_t(requested_mb < 132 ? 132 : requested_mb > 160 ? 160 : requested_mb);
  const uint32_t kBudget = pool_mb * 1024u * 1024u;
  budget[0] = uint8_t(kBudget >> 24); budget[1] = uint8_t(kBudget >> 16);
  budget[2] = uint8_t(kBudget >> 8); budget[3] = uint8_t(kBudget);
  applied = true;
  REXLOG_INFO("FM4 asset pool budget raised from 124 MB to {} MB", pool_mb);
}

// Owned-car thumbnail compatibility. The garage view can reference profile
// .xdc images that are absent, leaving the renderer's empty 32x32 texture.
// Factory images are an explicit compatibility mode; custom paint is not
// represented by them. Keep all garage statistics and ownership columns.
// Added 2026-10-23.
#include <cstring>
#include <mutex>
#include <string_view>
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(fm4_factory_garage_thumbnails, true, "FM4",
                   "Use factory car images in My Cars instead of profile thumbnails")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

extern "C" REX_FUNC(__imp__sub_823C8CE8);
REX_HOOK_RAW(sub_823C8CE8) {
  // This concatenation appends the first SELECT fragment of the garage view.
  // Other string concatenations, and all guest image data, stay untouched.
  if (REXCVAR_GET(fm4_factory_garage_thumbnails) &&
      ctx.r5.u32 == 0x8201FF48u && uint32_t(ctx.lr) == 0x8248997Cu) {
    static std::once_flag prepare;
    static uint32_t factory_fragment = 0;
    std::call_once(prepare, [&] {
      constexpr size_t kLength = 1342;
      constexpr std::string_view kBefore = "Garage.Thumbnail AS Thumbnail";
      constexpr std::string_view kAfter = "Data_Car.Thumbnail Thumbnail ";
      static_assert(kBefore.size() == kAfter.size());
      const auto* text = reinterpret_cast<const char*>(base + 0x8201FF48u);
      const std::string_view fragment(text, kLength);
      const auto offset = fragment.find(kBefore);
      if (text[kLength] != '\0' ||
          !fragment.starts_with(" AS SELECT Garage.Id AS Id,") ||
          offset == std::string_view::npos ||
          fragment.find(kBefore, offset + kBefore.size()) != std::string_view::npos) {
        REXLOG_WARN("FM4 factory thumbnails: unrecognized garage query; keeping original");
        return;
      }
      auto* memory = REX_KERNEL_MEMORY();
      const uint32_t address = memory->SystemHeapAlloc(uint32_t(kLength + 1));
      if (!address) {
        REXLOG_WARN("FM4 factory thumbnails: query allocation failed; keeping original");
        return;
      }
      auto* replacement = memory->TranslateVirtual<char*>(address);
      std::memcpy(replacement, text, kLength + 1);
      std::memcpy(replacement + offset, kAfter.data(), kAfter.size());
      factory_fragment = address;
      REXLOG_INFO("FM4 My Cars uses factory car thumbnails");
    });
    if (factory_fragment) ctx.r5.u64 = factory_fragment;
  }
  __imp__sub_823C8CE8(ctx, base);
}

// Optional speed trace (FM4_SPEED_TRACE set): game seconds the race update advances per real
// second. 1.00 means normal speed at any frame rate. Added 2026-10-23.
#include <chrono>
#include <cstdlib>
#include "../../tools/mods/game_bridge.h"

void Fm4RaceProbe(PPCContext& ctx, uint8_t* base);

extern "C" REX_FUNC(__imp__sub_824107E8);
REX_HOOK_RAW(sub_824107E8) {
  Fm4ModsGuestUpdate(ctx.f1.f64);
  static const bool trace = std::getenv("FM4_SPEED_TRACE") != nullptr;
  if (trace) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point start = Clock::now();
    static double game_seconds = 0;
    static uint32_t updates = 0;
    game_seconds += ctx.f1.f64; ++updates;
    const double real = std::chrono::duration<double>(Clock::now() - start).count();
    if (real >= 5) {
      REXLOG_INFO("FM4 speed: {:.3f} game s per real s, {:.1f} updates/s", game_seconds / real, updates / real);
      start = Clock::now(); game_seconds = 0; updates = 0;
    }
  }
  static const bool probe_enabled = std::getenv("FM4_PACING_PROBE") != nullptr;
  if (probe_enabled) Fm4RaceProbe(ctx, base);
  __imp__sub_824107E8(ctx, base);
}

// Opt-in presentation / race-delta experiments. Added 2026-10-24.
#include <algorithm>
#include <tuple>

REXCVAR_DEFINE_BOOL(fm4_timing_log, true, "FM4",
                   "Log presentation and fixed-delta state only when it changes");
REXCVAR_DEFINE_INT32(fm4_present_interval_override, 0, "FM4",
                    "Experimental: 0 = original interval, 1 = every guest vblank")
    .range(0, 1);
REXCVAR_DEFINE_BOOL(fm4_force_measured_delta, false, "FM4",
                   "Experimental: ignore the race settings fixed-delta branch");

namespace {

// Validate the whole requested span before reading guest memory, including heap
// boundaries and 32-bit addition overflow. TranslateVirtual handles heap aliases.
const uint8_t* Fm4TimingBytes(uint32_t object, uint32_t offset, uint32_t size) {
  if (!object || !size) return nullptr;
  const uint64_t address = uint64_t(object) + offset;
  const uint64_t end = address + size - 1;
  if (end > UINT32_MAX) return nullptr;
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) return nullptr;
  auto* heap = memory->LookupHeap(uint32_t(address));
  rex::memory::HeapAllocationInfo info{};
  if (!heap || memory->LookupHeap(uint32_t(end)) != heap ||
      !heap->QueryRegionInfo(uint32_t(address) & ~(heap->page_size() - 1), &info) ||
      !(info.state & rex::memory::kMemoryAllocationCommit) ||
      !(info.protect & rex::memory::kMemoryProtectRead) ||
      end >= uint64_t(info.base_address) + info.region_size) return nullptr;
  return memory->TranslateVirtual<const uint8_t*>(uint32_t(address));
}

uint32_t Fm4TimingU32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

template <size_t N>
bool Fm4TimingCode(uint32_t address, const uint32_t (&words)[N]) {
  const auto* p = Fm4TimingBytes(address, 0, uint32_t(N * 4));
  if (!p) return false;
  for (size_t i = 0; i < N; ++i) {
    if (Fm4TimingU32(p + i * 4) != words[i]) return false;
  }
  return true;
}

bool Fm4PresentContext() {
  static const bool valid = [] {
    // Setter store/return, then the complete interval conversion and its use.
    constexpr uint32_t setter[] = {0x908336A0, 0x4E800020};
    constexpr uint32_t consumer[] = {
        0x817F36A0, 0x2B0B0000, 0x419A0034, 0x2B0B0001, 0x419A002C,
        0x2B0B0002, 0x419A001C, 0x396BFFFC, 0x39400003, 0x316BFFFF,
        0x7D6B5910, 0x7D6B5038, 0x48000010, 0x39600002, 0x48000008,
        0x39600001, 0x815F2FE4, 0x556B402E};
    const bool ok = Fm4TimingCode(0x826E5F88, setter) &&
                    Fm4TimingCode(0x822B2F2C, consumer);
    if (!ok) REXLOG_WARN("FM4 timing: unrecognized presentation code; hooks disabled");
    return ok;
  }();
  return valid;
}

bool Fm4DeltaContext() {
  static const bool valid = [] {
    // lbz +1348; compare; beq; fmr fixed step; b; residual reset; stfs delta.
    constexpr uint32_t words[] = {0x897C0544, 0x280B0000, 0x41820010,
                                  0xFFE0E090, 0x48000008, 0xFFC0D890,
                                  0xD3FF0198};
    const bool ok = Fm4TimingCode(0x822F77D0, words);
    if (!ok) REXLOG_WARN("FM4 timing: unrecognized race-delta code; hooks disabled");
    return ok;
  }();
  return valid;
}

double Fm4VblankTargetHz() {
  // These two settings require restart, and the worker snapshots them at start.
  // Query the registry to avoid importing cvar storage from a GPU plugin DLL.
  static const double refresh = [] {
    const int32_t override_hz = rex::cvar::Query<int32_t>("vblank_frequency_override");
    return override_hz > 0 ? double(override_hz) :
        double(float(std::clamp(rex::cvar::Query<double>("video_mode_refresh_rate"), 24.0, 240.0)));
  }();
  // vsync is live: the SDK selects the 1000 Hz worker path when it is false.
  return rex::cvar::Query<bool>("vsync") && !rex::cvar::Query<bool>("gpu_unlimited_present") ? refresh : 1000.0;
}

// Each lambda instantiation has its own cache. Serialize comparison and emission
// across guest threads; no per-frame output, guest writes, or unbounded buffers.
template <typename Key, typename Log>
void Fm4TimingChange(const Key& key, Log log) {
  static std::mutex mutex;
  static bool seen = false;
  static Key last{};
  std::lock_guard<std::mutex> lock(mutex);
  if (seen && last == key) return;
  seen = true;
  last = key;
  log();
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_826E5F88);
REX_HOOK_RAW(sub_826E5F88) {
  if (REXCVAR_GET(fm4_timing_log) && Fm4PresentContext() &&
      !(ctx.r3.u32 & 3) && Fm4TimingBytes(ctx.r3.u32, 13984, 4)) {
    const uint32_t device = ctx.r3.u32;
    const uint32_t requested = ctx.r4.u32;
    const double hz = Fm4VblankTargetHz();
    Fm4TimingChange(std::tuple(device, requested, hz), [&] {
      REXLOG_INFO("FM4 interval setter: device={:08X} requested={} vblank_target_hz={:.3f}",
                  device, requested, hz);
    });
  }
  // Keep the requested state intact. Override at consumption so disabling the
  // cvar immediately restores it, including when the setter is never called again.
  __imp__sub_826E5F88(ctx, base);
}

void Fm4PresentInterval(PPCRegister& r31, PPCRegister& r11) {
  const bool trace = REXCVAR_GET(fm4_timing_log);
  static const bool unlimited = rex::cvar::Query<bool>("gpu_unlimited_present");
  const bool force = unlimited || REXCVAR_GET(fm4_present_interval_override) == 1;
  if ((!trace && !force) || !Fm4PresentContext() || (r31.u32 & 3)) return;
  const auto* interval = Fm4TimingBytes(r31.u32, 13984, 4);
  if (!interval || r11.u64 > 3) return;
  const uint32_t mapped = r11.u32;
  if (force) r11.u64 = unlimited ? 0 : 1;
  if (!trace) return;
  const uint32_t requested = Fm4TimingU32(interval);
  const uint32_t effective = r11.u32;
  const auto* refresh = Fm4TimingBytes(r31.u32, 21948, 4);
  // Keep unknown distinct from a reported 0 Hz during device initialization.
  // +21948 is an INTEGER Hz cache (fctidz/stfiwx at 0x826FA664/68).
  const uint64_t device_hz = refresh ? Fm4TimingU32(refresh) : UINT64_MAX;
  const double hz = Fm4VblankTargetHz();
  Fm4TimingChange(std::tuple(r31.u32, requested, mapped, effective, force, hz, device_hz), [&] {
    if (refresh) {
      REXLOG_INFO("FM4 present: device={:08X} requested={} mapped={} effective={} override={} "
                  "vblank_target_hz={:.3f} device_refresh_hz={}",
                  r31.u32, requested, mapped, effective, force, hz,
                  device_hz);
    } else {
      REXLOG_INFO("FM4 present: device={:08X} requested={} mapped={} effective={} override={} "
                  "vblank_target_hz={:.3f} device_refresh_hz=unknown",
                  r31.u32, requested, mapped, effective, force, hz);
    }
  });
}

#include "ingame_hooks.inc"

void Fm4MeasuredDeltaBranch(PPCRegister& r28, PPCRegister& r11) {
  static const bool unlimited = rex::cvar::Query<bool>("gpu_unlimited_present");
  if ((!unlimited && !REXCVAR_GET(fm4_force_measured_delta) && !Fm4TrainerArmed()) ||
      !Fm4DeltaContext() || (r28.u32 & 3)) return;
  const auto* flag = Fm4TimingBytes(r28.u32, 1348, 1);
  // The hook is BEFORE cmplwi at 0x822F77D4, AFTER the byte load. Mask only
  // the register; the original compare and beq select the existing measured path.
  if (flag && r11.u64 <= 255 && r11.u32 == *flag) r11.u64 = 0;
}

void Fm4FixedDeltaState(PPCRegister& r28, PPCRegister& f31) {
  if (Fm4TrainerArmed() && Fm4DeltaContext()) f31.f64 = Fm4TrainerDelta(f31.f64);
  // Observe even the mode paths that skip 0x822F77D0 entirely. The flag alone
  // does not prove that the fixed branch ran on this iteration.
  if (!REXCVAR_GET(fm4_timing_log) || !Fm4DeltaContext() || (r28.u32 & 3)) return;
  const auto* flag = Fm4TimingBytes(r28.u32, 1348, 1);
  if (!flag) return;
  const uint32_t value = *flag;
  const bool force = rex::cvar::Query<bool>("gpu_unlimited_present") || REXCVAR_GET(fm4_force_measured_delta);
  const double hz = Fm4VblankTargetHz();
  Fm4TimingChange(std::tuple(r28.u32, value, force, hz), [&] {
    REXLOG_INFO("FM4 fixed-delta setting: settings={:08X} byte_1348={} enabled={} "
                "force_measured={} vblank_target_hz={:.3f}",
                r28.u32, value, value != 0, force, hz);
  });
}

// Read-only diagnostics: actual simulation clock, constants and call cadence.
// A paused/loading transition starts a new observation window.
#include <bit>
#include <cmath>
#include <atomic>
#include <map>
namespace {
std::atomic<uint64_t> fm4_pose_sequence{0};
thread_local uint64_t fm4_inner_steps = 0;
}
REXCVAR_DEFINE_BOOL(fm4_time_based_visual_updates, false, "FM4",
                   "Scale fixed-60-Hz presentation updates by real elapsed time at unlocked frame rates")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
void Fm4VisualUpdateDelta(PPCContext& ctx, uint32_t phase) {
  if (!REXCVAR_GET(fm4_time_based_visual_updates)) return;
  // The presentation registry's two inline paths pass descriptor+8 as delta.
  // 826DA360 constructs that descriptor from the world clock at +616 but
  // supplies a constant 1/60, i.e. one 60 Hz frame of presentation time per
  // update. Scale that by the real time each object has waited instead, so
  // presentation runs at console speed at any frame rate. Never pass zero:
  // world-clock differences were zero on repeated snapshots, which left road
  // detail washed out and streaked.
  const uint32_t caller = uint32_t(ctx.lr);
  if (caller != 0x822CA4D8u && caller != 0x822CA4E4u &&
      caller != 0x822CA60Cu && caller != 0x822CA618u) return;
  if (phase == 0x822CA3A8u && (ctx.r5.u32 & 255)) return;
  const auto* descriptor = Fm4TimingBytes(ctx.r26.u32, 0, 16);
  if (!descriptor || std::abs(ctx.f1.f64 - 1.0 / 60.0) > 1e-7 ||
      Fm4TimingU32(descriptor + 8) != 0x3C888889u) return;
  using Clock = std::chrono::steady_clock;
  struct Sample { Clock::time_point last; bool seen = false; };
  static thread_local std::map<uint64_t, Sample> samples;
  if (samples.size() > 8192) samples.clear();
  auto& sample = samples[(uint64_t(phase) << 32) | ctx.r3.u32];
  const auto now = Clock::now();
  double delta = 1.0 / 60.0;
  if (sample.seen) {
    // Clamp to 1000 Hz .. 10 Hz: long gaps (loading, menus) restart at a
    // normal step rather than jumping.
    delta = std::chrono::duration<double>(now - sample.last).count();
    delta = delta > 0.1 ? 1.0 / 60.0 : std::max(delta, 1.0 / 1000.0);
  }
  sample = {now, true};
  ctx.f1.f64 = double(float(delta));
}
}

extern "C" REX_FUNC(__imp__sub_822CA3A8);
REX_HOOK_RAW(sub_822CA3A8) {
  Fm4VisualUpdateDelta(ctx, 0x822CA3A8u);
  __imp__sub_822CA3A8(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_822C8A30);
REX_HOOK_RAW(sub_822C8A30) {
  Fm4VisualUpdateDelta(ctx, 0x822C8A30u);
  __imp__sub_822C8A30(ctx, base);
}
REXCVAR_DEFINE_BOOL(fm4_uniform_physics_steps, false, "FM4",
                   "Experimental: accumulate race physics time into uniform substeps")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// 120 Hz engine passes give the vehicle integrator ceil(359/120) = 3 substeps of
// 1/360 s, the same vehicle rate the console runs at 60 FPS.
REXCVAR_DEFINE_INT32(fm4_uniform_physics_hz, 120, "FM4",
                    "Engine pass rate for fm4_uniform_physics_steps (60, 120, 180 or 240)")
    .range(60, 240)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

extern "C" REX_FUNC(__imp__sub_822FEAD0);
REX_HOOK_RAW(sub_822FEAD0) {
  // Only the physics engine's ceil(delta / max_step), after its time scaling
  // and +/-0.2 second clamp. Preserve the engine's own inner loop and all its
  // per-update preparation/cleanup. No global physics setting is overwritten.
  if (REXCVAR_GET(fm4_uniform_physics_steps) && uint32_t(ctx.lr) == 0x828307B8u &&
      std::isfinite(ctx.f31.f64) && std::abs(ctx.f31.f64) <= 0.200001) {
    constexpr uint32_t call[] = {0x48000001u | ((0x822FEAD0u - 0x828307B4u) & 0x03FFFFFCu)};
    static const bool valid = Fm4TimingCode(0x828307B4u, call);
    if (valid && Fm4TimingBytes(ctx.r30.u32, 208, 8)) {
      static thread_local uint32_t engine = 0;
      static thread_local double residual = 0, last_clock = 0;
      static thread_local bool reverse = false;
      const auto* p = base + ctx.r30.u32 + 208;
      const double clock = std::bit_cast<double>((uint64_t(Fm4TimingU32(p)) << 32) |
                                                Fm4TimingU32(p + 4));
      const bool negative = ctx.f31.f64 < 0;
      if (engine != ctx.r30.u32 || reverse != negative ||
          (!negative && clock < last_clock) || (negative && clock > last_clock)) {
        engine = ctx.r30.u32; residual = 0; reverse = negative;
      }
      last_clock = clock;
      static const double rate = double(REXCVAR_GET(fm4_uniform_physics_hz));
      residual += std::abs(ctx.f31.f64) * rate;
      const uint32_t count = uint32_t(std::floor(residual + 1e-9));
      residual -= count;
      // The original zero-count branch skips integration, retaining fractional
      // time for the next update. f31 is not consumed after that branch.
      ctx.f31.f64 = (negative ? -1.0 : 1.0) * double(count) / rate;
      ctx.f1.f64 = double(count);
      return;
    }
  }
  __imp__sub_822FEAD0(ctx, base);
}

void Fm4RaceProbe(PPCContext& ctx, uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  static thread_local Clock::time_point last_wall = Clock::now();
  static thread_local double last_game = 0;
  static thread_local uint32_t calls = 0;
  // This is the original function's live race-manager argument (+24 is its clock).
  const auto* bytes = base + ctx.r3.u32 + 24;
  const double game = std::bit_cast<double>((uint64_t(Fm4TimingU32(bytes)) << 32) |
                                           Fm4TimingU32(bytes + 4));
  const auto now = Clock::now();
  const double real = std::chrono::duration<double>(now - last_wall).count();
  ++calls;
  if (real < 1) return;
  REXLOG_INFO("FM4 race probe: object={:08X} game={:.6f} wall={:.6f} advance={:.6f} calls={} rate={:.6f}",
              ctx.r3.u32, game, real, game - last_game, calls, (game - last_game) / real);
  last_wall = now; last_game = game; calls = 0;
  static bool constants = false;
  if (!constants) {
    constants = true;
    REXLOG_INFO("FM4 physics rates: outer_hz={} vehicle_hz={}",
                Fm4TimingU32(base + 0x82DFD5A4u), Fm4TimingU32(base + 0x82DFD5A0u));
    for (uint32_t a : {0x82000F80u, 0x82000C34u, 0x82066F38u, 0x82000EC0u,
                       0x82000DB8u, 0x82000EC4u, 0x82000D1Cu}) {
      const auto* p = Fm4TimingBytes(a, 0, 4);
      if (p) REXLOG_INFO("FM4 timing constant: address={:08X} value={:.9f}",
                         a, std::bit_cast<float>(Fm4TimingU32(p)));
    }
  }
}

extern "C" REX_FUNC(__imp__sub_828306E8);
REX_HOOK_RAW(sub_828306E8) {
  static const bool probe = std::getenv("FM4_PACING_PROBE") != nullptr;
  const uint32_t engine = ctx.r3.u32;
  const double delta = ctx.f1.f64;
  if (!probe) { __imp__sub_828306E8(ctx, base); return; }
  using Clock = std::chrono::steady_clock;
  static thread_local auto wall_start = Clock::now();
  static thread_local double total = 0, window = 0;
  static thread_local uint32_t calls = 0;
  total += delta; window += delta; ++calls;
  if (window < 1) { __imp__sub_828306E8(ctx, base); return; }
  const auto u32 = [&](uint32_t a) { return Fm4TimingU32(base + a); };
  const auto f32 = [&](uint32_t a) { return std::bit_cast<float>(u32(a)); };
  const uint32_t begin = u32(engine + 64), end = u32(engine + 68);
  const auto now = Clock::now();
  const double wall = std::chrono::duration<double>(now - wall_start).count();
  REXLOG_INFO("FM4 physics probe: engine={:08X} game={:.6f} delta={:.9f} window={:.6f} wall={:.6f} calls={} cars={}",
              engine, total, delta, window, wall, calls, (end - begin) / 4);
  if (begin && end >= begin + 4) {
    const uint32_t car = u32(begin);
    const uint32_t vtable = u32(car);
    const double vx = f32(car + 80), vy = f32(car + 84), vz = f32(car + 88);
    REXLOG_INFO("FM4 car probe: car={:08X} game={:.6f} vtable={:08X} position={:.6f},{:.6f},{:.6f} speed_mps={:.6f}",
                car, total, vtable, f32(car + 16), f32(car + 20), f32(car + 24),
                std::sqrt(vx * vx + vy * vy + vz * vz));
  }
  window = 0; calls = 0; wall_start = now;
  __imp__sub_828306E8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_822C7270);
REX_HOOK_RAW(sub_822C7270) {
  static const bool probe = std::getenv("FM4_PACING_PROBE") != nullptr;
  // The engine reaches this maintenance call after all its integration steps.
  // Publish here rather than after a guest tail-return that may reenter.
  if (probe && uint32_t(ctx.lr) == 0x82830848u) {
    static thread_local uint64_t previous_steps = 0;
    if (fm4_inner_steps != previous_steps) {
      fm4_pose_sequence.fetch_add(1, std::memory_order_relaxed);
      previous_steps = fm4_inner_steps;
    }
  }
  __imp__sub_822C7270(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82830320);
REX_HOOK_RAW(sub_82830320) {
  static const bool probe = std::getenv("FM4_PACING_PROBE") != nullptr;
  if (probe) {
    ++fm4_inner_steps;
    static thread_local auto start = std::chrono::steady_clock::now();
    static thread_local uint32_t ticks = 0;
    static thread_local double seconds = 0, largest = 0;
    ++ticks; seconds += ctx.f1.f64; largest = std::max(largest, std::abs(ctx.f1.f64));
    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(now - start).count();
    if (wall >= 5) {
      REXLOG_INFO("FM4 physics ticks: count={} seconds={:.6f} wall={:.6f} largest={:.9f}", ticks, seconds, wall, largest);
      ticks = 0; seconds = 0; largest = 0; start = now;
    }
  }
  __imp__sub_82830320(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82306F08);
REX_HOOK_RAW(sub_82306F08) {
  static const bool probe = std::getenv("FM4_PACING_PROBE") != nullptr;
  if (probe) {
    struct Sample { uint64_t count = 0; double seconds = 0, largest = 0; };
    static thread_local std::map<uint32_t, Sample> samples;
    if (samples.size() > 128) samples.clear();
    auto& sample = samples[ctx.r3.u32];
    ++sample.count;
    sample.seconds += std::abs(ctx.f1.f64);
    sample.largest = std::max(sample.largest, std::abs(ctx.f1.f64));
    if (sample.seconds >= 5) {
      REXLOG_INFO("FM4 vehicle integration: car={:08X} count={} game={:.9f} largest={:.9f}",
                  ctx.r3.u32, sample.count, sample.seconds, sample.largest);
      sample = {};
    }
  }
  __imp__sub_82306F08(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_8237F598);
REX_HOOK_RAW(sub_8237F598) {
  static const bool probe = std::getenv("FM4_PACING_PROBE") != nullptr;
  if (probe) {
    using Clock = std::chrono::steady_clock;
    static thread_local auto start = Clock::now();
    static thread_local uint64_t previous = 0, frames = 0, poses = 0;
    static thread_local uint64_t zero = 0, one = 0, multiple = 0, largest = 0;
    const uint64_t current = fm4_pose_sequence.load(std::memory_order_relaxed);
    const uint64_t count = current - previous;
    previous = current;
    ++frames; poses += count; largest = std::max(largest, count);
    if (!count) ++zero;
    else if (count == 1) ++one;
    else ++multiple;
    const auto now = Clock::now();
    if (std::chrono::duration<double>(now - start).count() >= 5) {
      REXLOG_INFO("FM4 pose cadence: frames={} new_poses={} zero={} one={} multiple={} max={}",
                  frames, poses, zero, one, multiple, largest);
      frames = poses = zero = one = multiple = largest = 0; start = now;
    }
  }
  __imp__sub_8237F598(ctx, base);
}

// Restore a bounded delay to the guest's otherwise empty fence-poll delay.
#include <rex/cvar.h>
#include <rex/thread.h>
REXCVAR_DEFINE_BOOL(fm4_fence_poll_backoff, false, "CPU",
                    "Pause briefly after bounded guest fence polling")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

extern "C" REX_FUNC(__imp__sub_826EDE50);
REX_HOOK_RAW(sub_826EDE50) {
  if (REXCVAR_GET(fm4_fence_poll_backoff) && ctx.dispatch_address == 0) {
    static thread_local uint32_t polls = 0;
    if (++polls == 32) {
      polls = 0;
      rex::thread::Sleep(std::chrono::microseconds(100));
    }
  }
  __imp__sub_826EDE50(ctx, base);
}
