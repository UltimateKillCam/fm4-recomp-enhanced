// FM4 full-scene tiling submission. GPL-3.0-only.
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/graphics/d3d12/native_rt/tiling_markers.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <Windows.h>
#endif

extern "C" REX_FUNC(__imp__sub_822FE9C8);

namespace {
namespace markers = rex::graphics::d3d12::native_rt;
constexpr uint32_t kModeDescriptors = 0x82D88518;
constexpr uint32_t kModeStride = 1232;
constexpr size_t kMaxDevices = 8;
constexpr uint32_t kScratchSize = 48;
struct Pass {
  uint32_t scratch = 0;
  uint32_t descriptor = 0;
  uint64_t generation = 0;
  bool active = false;
  bool resolve_redirected = false;
  bool completing = false;
};
std::mutex pass_mutex;
std::map<uint32_t, Pass> passes;
uint64_t next_generation = 1;
uint64_t merged_passes = 0;
uint64_t merged_resolve_loops = 0;

bool NativeTargetsActive() {
#ifdef _WIN32
  // Requested configuration is insufficient: initialization can reject the
  // native backend. Resolve the live backend's explicit capability handshake.
  HMODULE module = GetModuleHandleW(L"rexgpu-xenos.dll");
  if (!module) return false;
  using Capabilities = uint32_t (*)();
  auto capabilities = reinterpret_cast<Capabilities>(
      GetProcAddress(module, "RexNativeRenderTargetCapabilities"));
  // Bit 1 additionally certifies private full-scene domains and ordered
  // last-band publication; an active backend alone cannot merge safely.
  return capabilities && (capabilities() & 3u) == 3u;
#else
  return false;
#endif
}

const uint8_t* GuestBytes(uint32_t object, uint32_t size, bool writable = false) {
  if (!object || !size || uint64_t(object) + size > uint64_t(UINT32_MAX) + 1)
    return nullptr;
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) return nullptr;
  uint32_t end = object + size - 1;
  auto* heap = memory->LookupHeap(object);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || memory->LookupHeap(end) != heap ||
      !heap->QueryRegionInfo(object & ~(heap->page_size() - 1), &info) ||
      !(info.state & rex::memory::kMemoryAllocationCommit) ||
      !(info.protect & rex::memory::kMemoryProtectRead) ||
      (writable && !(info.protect & rex::memory::kMemoryProtectWrite)) ||
      uint64_t(end) >= uint64_t(info.base_address) + info.region_size)
    return nullptr;
  return memory->TranslateVirtual<const uint8_t*>(object);
}
uint32_t Read32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
         uint32_t(bytes[2]) << 8 | bytes[3];
}
void Write32(uint8_t* bytes, uint32_t value) {
  bytes[0] = uint8_t(value >> 24); bytes[1] = uint8_t(value >> 16);
  bytes[2] = uint8_t(value >> 8); bytes[3] = uint8_t(value);
}

bool AppendOrderedMarker(uint32_t device, markers::TilingMarkerType type, uint64_t identity) {
  if (uint64_t(device) + 60 > uint64_t(UINT32_MAX) + 1) return false;
  const auto* fields = GuestBytes(device + 48, 12);
  if (!fields || !GuestBytes(device + 48, 4, true)) return false;
  uint32_t cursor = Read32(fields), end = Read32(fields + 4), limit = Read32(fields + 8);
  if ((cursor | end | limit) & 3u) return false;
  uint64_t next = uint64_t(cursor) + markers::kTilingMarkerPacketBytes;
  if (!cursor || cursor > limit || limit > end || next > limit || next > UINT32_MAX)
    return false;
  // The guest command cursor points at the LAST word, so every packet store
  // preincrements by four, exactly like the original PM4 emitters.
  const auto* packet_bytes = GuestBytes(cursor + 4, markers::kTilingMarkerPacketBytes, true);
  if (!packet_bytes) return false;
  markers::TilingMarker marker;
  marker.type = type; marker.identity = identity;
  const auto payload = markers::EncodeTilingMarker(marker);
  auto* packet = const_cast<uint8_t*>(packet_bytes);
  Write32(packet, markers::kTilingMarkerPacketHeader);
  for (size_t n = 0; n < payload.size(); ++n) Write32(packet + 4 + n * 4, payload[n]);
  std::atomic_thread_fence(std::memory_order_release);
  Write32(const_cast<uint8_t*>(fields), uint32_t(next));
  return true;
}

void AppendEndMarker(PPCContext& ctx, uint8_t* base, uint32_t device, uint64_t identity) {
  if (AppendOrderedMarker(device, markers::TilingMarkerType::kEnd, identity)) return;
  // End is mandatory after an accepted begin. Rotate the original guest PM4
  // buffer when its checked write limit has no room; the next marker remains
  // ordered after all earlier primary/indirect submissions.
  PPCContext flush_context = ctx;
  flush_context.r3.u64 = device;
  flush_context.dispatch_address = 0;
  flush_context.lr = 0;
  __imp__sub_822FE9C8(flush_context, base);
  if (AppendOrderedMarker(device, markers::TilingMarkerType::kEnd, identity)) return;
  REXLOG_ERROR("FM4 native tiling: unable to append mandatory ordered end marker; stopping");
  std::abort();
}

bool BeginFullScene(PPCContext& ctx) {
  // The verified renderer caller supplies mode-descriptor rectangles. Arbitrary
  // D3D tiling calls, one-shot tiling requests and other partitions stay intact.
  if (uint32_t(ctx.lr) != 0x82381078 || ctx.r5.u32 != 3 || ctx.r6.u32 < 32 ||
      ctx.r4.u32 != 0 || !NativeTargetsActive()) return false;
  uint32_t descriptor = ctx.r6.u32 - 32;
  if (descriptor < kModeDescriptors ||
      (descriptor - kModeDescriptors) % kModeStride ||
      (descriptor - kModeDescriptors) / kModeStride >= 64) return false;
  const auto* source = GuestBytes(descriptor, 32 + 3 * 16);
  if (!source || Read32(source + 16) != 3) return false;
  constexpr std::array<uint32_t, 12> kVerifiedBands = {
      0, 0, 1280, 256, 0, 256, 1280, 512, 0, 512, 1280, 720};
  for (size_t n = 0; n < kVerifiedBands.size(); ++n) {
    if (Read32(source + 32 + n * 4) != kVerifiedBands[n]) return false;
  }
  // Flags bit 0 suppresses the initial clear, and device+11071 bit 0x20
  // independently skips it. Accept only the path proven to clear every bound
  // attachment before any pass reads. Recording/capture and already-tiled
  // clients retain their original partition and submission semantics.
  const auto* device = GuestBytes(ctx.r3.u32, 13792);
  if (!device || (device[11071] & 0x20) || (device[11068] & 0xB0) ||
      (device[11069] & 0x20) || Read32(device + 13584) ||
      Read32(device + 13060) != 0xFFFFFFFFu || Read32(device + 13064) != 0) return false;
  // Clear flags 0x3F include color0..3, depth and stencil. The guest clears
  // depth/stencil together with its first bound color; require color0 rather
  // than claiming an unproven depth-only initialization path.
  uint32_t color0 = Read32(device + 12792);
  if (!color0 || !GuestBytes(color0, 48)) return false;
  if (ctx.r7.u32 && !GuestBytes(ctx.r7.u32, 16)) return false;
  // Begin temporarily selects these SDK viewport/scissor constants before its
  // clear. Clear implementation intersects both, so prove they contain the
  // complete enlarged rectangle instead of assuming their constant contents.
  const auto* clear_viewport = GuestBytes(0x820020BC, 24);
  const auto* clear_scissor = GuestBytes(0x820019E0, 16);
  if (!clear_viewport || !clear_scissor || Read32(clear_viewport) != 0 ||
      Read32(clear_viewport + 4) != 0 || Read32(clear_viewport + 8) < 1280 ||
      Read32(clear_viewport + 12) < 720 ||
      int32_t(Read32(clear_scissor)) > 0 || int32_t(Read32(clear_scissor + 4)) > 0 ||
      int32_t(Read32(clear_scissor + 8)) < 1280 ||
      int32_t(Read32(clear_scissor + 12)) < 720) return false;
  std::lock_guard lock(pass_mutex);
  auto found = passes.find(ctx.r3.u32);
  if (found != passes.end() && found->second.active) return false;
  if (found == passes.end()) {
    if (passes.size() >= kMaxDevices) return false;
    found = passes.emplace(ctx.r3.u32, Pass{}).first;
  }
  Pass& pass = found->second;
  auto* memory = REX_KERNEL_MEMORY();
  if (!pass.scratch) pass.scratch = memory->SystemHeapAlloc(kScratchSize);
  if (!pass.scratch) return false;
  auto* scratch = memory->TranslateVirtual<uint8_t*>(pass.scratch);
  std::memcpy(scratch, source, kScratchSize);
  Write32(scratch + 16, 1);
  Write32(scratch + 32, 0); Write32(scratch + 36, 0);
  Write32(scratch + 40, 1280); Write32(scratch + 44, 720);
  uint64_t generation = next_generation++;
  if (!AppendOrderedMarker(ctx.r3.u32, markers::TilingMarkerType::kBegin, generation)) return false;
  pass.descriptor = descriptor;
  pass.generation = generation;
  pass.active = true;
  pass.completing = false;
  pass.resolve_redirected = false;
  ctx.r5.u64 = 1;
  ctx.r6.u64 = pass.scratch + 32;
  ++merged_passes;
  if (merged_passes == 1) {
    REXLOG_INFO("FM4 native tiling: verified 1280x720 three-band pass submitted as one full-scene pass");
  }
  return true;
}

struct FinishPass {
  uint32_t device = 0;
  uint64_t generation = 0;
  ~FinishPass() {
    if (!generation) return;
    std::lock_guard lock(pass_mutex);
    auto found = passes.find(device);
    if (found == passes.end() || found->second.generation != generation) return;
    // Scratch is bounded to one allocation per device and persists through
    // subsequent GPU submissions. No global guest descriptor is ever modified.
    found->second.active = false;
    found->second.resolve_redirected = false;
    found->second.completing = false;
  }
};
}  // namespace

extern "C" REX_FUNC(__imp__sub_82374170);
REX_HOOK_RAW(sub_82374170) {
  BeginFullScene(ctx);
  // With the native path off, every argument and all guest memory is untouched.
  // With it on, the original SDK records exactly one tile. It emits its own
  // original copy predicates and replays geometry once, without a host override.
  __imp__sub_82374170(ctx, base);
}

// At 82376CF4, r11 is the ORIGINAL mode descriptor and r10 its tile count.
// The next instructions derive r27 = descriptor+32 and r23 = tileCount.
// Redirecting those registers makes each output resolve cover the full scene
// once, without writing the shared mode descriptor or suppressing copy packets.
void Fm4NativeTilingResolveLoop(PPCRegister& r31, PPCRegister& r11, PPCRegister& r10) {
  if (uint64_t(r31.u32) + 7828 > uint64_t(UINT32_MAX) + 1) return;
  const auto* device_bytes = GuestBytes(r31.u32 + 7824, 4);
  if (!device_bytes) return;
  uint32_t device = Read32(device_bytes);
  std::lock_guard lock(pass_mutex);
  auto found = passes.find(device);
  if (found == passes.end() || !found->second.active || !found->second.completing ||
      found->second.descriptor != r11.u32 || r10.u32 != 3) return;
  r11.u64 = found->second.scratch;
  r10.u64 = 1;
  found->second.resolve_redirected = true;
  ++merged_resolve_loops;
  if (merged_resolve_loops == 1) {
    REXLOG_INFO("FM4 native tiling: per-band output resolves replaced by one full-scene resolve per output");
  }
}

extern "C" REX_FUNC(__imp__sub_82376AA0);
REX_HOOK_RAW(sub_82376AA0) {
  FinishPass finish;
  const auto* device_bytes = uint64_t(ctx.r3.u32) + 7828 <= uint64_t(UINT32_MAX) + 1
      ? GuestBytes(ctx.r3.u32 + 7824, 4) : nullptr;
  if (device_bytes) {
    finish.device = Read32(device_bytes);
    std::lock_guard lock(pass_mutex);
    auto found = passes.find(finish.device);
    if (found != passes.end() && found->second.active && !found->second.completing) {
      finish.generation = found->second.generation;
      found->second.completing = true;
    }
  }
  __imp__sub_82376AA0(ctx, base);
  if (finish.generation) AppendEndMarker(ctx, base, finish.device, finish.generation);
}
