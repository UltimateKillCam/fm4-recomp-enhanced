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
// The PC build has spare guest physical memory, so give that pool 8 MB more budget.
// Applies only to the stock value. Added 2026-10-19.
#include <rex/hook.h>
#include <rex/logging.h>

extern "C" REX_FUNC(__imp__sub_8237CFD8);
REX_HOOK_RAW(sub_8237CFD8) {
  __imp__sub_8237CFD8(ctx, base);
  static bool applied = false;
  if (applied || !ctx.r3.u32) return;
  uint8_t* budget = base + ctx.r3.u32 + 1640;
  const uint32_t value = uint32_t(budget[0]) << 24 | uint32_t(budget[1]) << 16 |
                         uint32_t(budget[2]) << 8 | budget[3];
  if (value != 0x07C00000u) return;  // settings not populated yet, or not the stock budget
  constexpr uint32_t kBudget = 0x07C00000u + 0x00800000u;
  budget[0] = uint8_t(kBudget >> 24); budget[1] = uint8_t(kBudget >> 16);
  budget[2] = uint8_t(kBudget >> 8); budget[3] = uint8_t(kBudget);
  applied = true;
  REXLOG_INFO("FM4 asset pool budget raised from 124 MB to 132 MB");
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

extern "C" REX_FUNC(__imp__sub_824107E8);
REX_HOOK_RAW(sub_824107E8) {
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
  return rex::cvar::Query<bool>("vsync") ? refresh : 1000.0;
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
  const bool force = REXCVAR_GET(fm4_present_interval_override) == 1;
  if ((!trace && !force) || !Fm4PresentContext() || (r31.u32 & 3)) return;
  const auto* interval = Fm4TimingBytes(r31.u32, 13984, 4);
  if (!interval || r11.u64 > 3) return;
  const uint32_t mapped = r11.u32;
  if (force) r11.u64 = 1;
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

void Fm4MeasuredDeltaBranch(PPCRegister& r28, PPCRegister& r11) {
  if (!REXCVAR_GET(fm4_force_measured_delta) || !Fm4DeltaContext() || (r28.u32 & 3)) return;
  const auto* flag = Fm4TimingBytes(r28.u32, 1348, 1);
  // The hook is BEFORE cmplwi at 0x822F77D4, AFTER the byte load. Mask only
  // the register; the original compare and beq select the existing measured path.
  if (flag && r11.u64 <= 255 && r11.u32 == *flag) r11.u64 = 0;
}

void Fm4FixedDeltaState(PPCRegister& r28) {
  // Observe even the mode paths that skip 0x822F77D0 entirely. The flag alone
  // does not prove that the fixed branch ran on this iteration.
  if (!REXCVAR_GET(fm4_timing_log) || !Fm4DeltaContext() || (r28.u32 & 3)) return;
  const auto* flag = Fm4TimingBytes(r28.u32, 1348, 1);
  if (!flag) return;
  const uint32_t value = *flag;
  const bool force = REXCVAR_GET(fm4_force_measured_delta);
  const double hz = Fm4VblankTargetHz();
  Fm4TimingChange(std::tuple(r28.u32, value, force, hz), [&] {
    REXLOG_INFO("FM4 fixed-delta setting: settings={:08X} byte_1348={} enabled={} "
                "force_measured={} vblank_target_hz={:.3f}",
                r28.u32, value, value != 0, force, hz);
  });
}
