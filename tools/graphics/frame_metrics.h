// Opt-in renderer profiling. Modified 2026-10-17. GPL-3.0-only.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <fstream>
#include <string>
#include <vector>
#include <rex/logging.h>
#include <rex/graphics/d3d12/fm4_frame_trace.h>

namespace fm4::frame_metrics {
// Event timeline for one short window (FM4_TIMELINE=<metrics phase>, with FM4_FRAME_METRICS):
// GPU-thread wait spans, guest ring kicks, shader completions, swaps and each submission's
// CPU close and GPU execution, all on the steady (QPC) clock. Written once to
// <FM4_CAPTURE_DIR>/timeline.csv, 2 s after the phase starts, covering 2 s.
struct Timeline {
  struct Event { char kind; int64_t t0, t1; uint64_t a, b; };
  std::vector<Event> events;
  const std::string target = std::getenv("FM4_TIMELINE") ? std::getenv("FM4_TIMELINE") : "";
  int64_t begin = 0, end = 0;
  bool written = false;
  ID3D12CommandQueue* queue = nullptr;
  uint64_t gpu_frequency = 0, gpu_reference = 0;
  int64_t cpu_reference_ns = 0;
  static int64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  bool Active(int64_t t) const { return begin && t >= begin && t < end; }
  void Add(char kind, int64_t t0, int64_t t1, uint64_t a = 0, uint64_t b = 0) {
    if (Active(t0) || Active(t1)) events.push_back({kind, t0, t1, a, b});
  }
  void Arm(const std::string& phase) {
    if (begin || target.empty() || phase != target || !queue) return;
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    UINT64 cpu = 0;
    if (FAILED(queue->GetTimestampFrequency(&gpu_frequency)) ||
        FAILED(queue->GetClockCalibration(&gpu_reference, &cpu))) return;
    counter.QuadPart = LONGLONG(cpu);
    cpu_reference_ns = int64_t(double(counter.QuadPart) * 1e9 / double(frequency.QuadPart));
    begin = Now() + 2'000'000'000; end = begin + 2'000'000'000;
    events.reserve(1 << 16);
    REXGPU_INFO("FM4 timeline armed for phase {} begin_ns={}", phase, begin);
  }
  int64_t GpuToCpu(uint64_t gpu) const {
    return cpu_reference_ns + int64_t(double(int64_t(gpu - gpu_reference)) * 1e9 / double(gpu_frequency));
  }
  void MaybeWrite() {
    if (!begin || written || Now() < end + 1'000'000'000) return;
    written = true;
    const char* directory = std::getenv("FM4_CAPTURE_DIR");
    if (!directory) return;
    std::ofstream output(std::filesystem::path(directory) / "timeline.csv");
    output << "kind,t0_us,t1_us,a,b\n";
    for (const auto& e : events)
      output << e.kind << ',' << double(e.t0 - begin) / 1000.0 << ',' << double(e.t1 - begin) / 1000.0
             << ',' << e.a << ',' << e.b << '\n';
    REXGPU_INFO("FM4 timeline written: {} events", events.size());
    events.clear(); events.shrink_to_fit();
  }
};
inline Timeline& Line() { static Timeline timeline; return timeline; }
enum class Stage : size_t { Other, Idle, Commands, GuestWait, Draw, Resolve, Submit, Pipeline, Record, Queue, Fence, Wait, Swap, Constants, Bindings, Textures, Primitives, Camera, Count };
using Clock = std::chrono::steady_clock;
struct Trace {
  const bool enabled = std::getenv("FM4_FRAME_METRICS") != nullptr;
  Stage stage = Stage::Other;
  Clock::time_point mark = Clock::now(), previous = mark, window = mark;
  std::array<double, size_t(Stage::Count)> ms{};
  std::array<uint64_t, size_t(Stage::Count)> calls{};
  // Stage totals at the previous swap, so a slow frame can be broken down on its own.
  std::array<double, size_t(Stage::Count)> at_previous_frame{};
  uint32_t slow_frames_logged = 0;
  std::vector<double> frames;
  uint64_t previous_cpu = 0, swaps = 0;
  double gpu_commands_ms = 0;
  uint64_t gpu_timed_submissions = 0;
  double gpu_resolve_ms = 0;
  uint64_t gpu_timed_resolves = 0;
  uint64_t constant_calls = 0, constant_skips = 0;
  uint64_t type0_words = 0, type0_constant_words = 0;
  uint64_t bulk_state_words = 0;
  uint64_t draw_state_hits = 0, pipeline_state_hits = 0;
  uint64_t shader_state_hits = 0, raster_state_hits = 0;
  uint64_t system_state_hits = 0, sampler_state_hits = 0;
  std::string phase = "unmarked";
  // Set when Frame() closed a metrics window, for the per-pass GPU report.
  std::string reported_phase;
  double reported_frames = 0;
  static uint64_t ThreadCpu() {
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0;
    auto value = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return value(kernel) + value(user);
  }
  void Mark(Clock::time_point now) {
    if (Line().begin && (stage == Stage::Idle || stage == Stage::GuestWait || stage == Stage::Swap ||
                         stage == Stage::Wait || stage == Stage::Fence || stage == Stage::Queue)) {
      const int64_t t1 = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
      const int64_t t0 = std::chrono::duration_cast<std::chrono::nanoseconds>(mark.time_since_epoch()).count();
      Line().Add('s', t0, t1, uint64_t(stage));
    }
    ms[size_t(stage)] += std::chrono::duration<double, std::milli>(now - mark).count();
    mark = now;
  }
  void Reset(Clock::time_point now) {
    ms.fill(0); calls.fill(0); frames.clear(); at_previous_frame.fill(0);
    mark = previous = window = now; previous_cpu = ThreadCpu();
    gpu_commands_ms = 0; gpu_timed_submissions = 0;
    gpu_resolve_ms = 0; gpu_timed_resolves = 0;
    constant_calls = constant_skips = 0;
    type0_words = type0_constant_words = 0;
    bulk_state_words = 0;
    draw_state_hits = pipeline_state_hits = 0;
    shader_state_hits = raster_state_hits = system_state_hits = sampler_state_hits = 0;
  }
  bool ReadPhase() {
    if (const char* directory = std::getenv("FM4_CAPTURE_DIR")) {
      std::ifstream input(std::filesystem::path(directory) / "metrics-phase.txt");
      std::string next; input >> next;
      if (!next.empty() && next.size() <= 64 &&
          next.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") == std::string::npos && next != phase) {
        phase = next; return true;
      }
    }
    return false;
  }
  bool Frame(uint32_t width, uint32_t height) {
    if (!enabled) return false;
    auto now = Clock::now(); Mark(now);
    Line().Arm(phase);
    Line().Add('F', Timeline::Now(), Timeline::Now());
    Line().MaybeWrite();
    if (!previous_cpu || (++swaps % 15 == 0 && ReadPhase())) { Reset(now); return false; }
    // Include long stalls. Dropping >1-second intervals hides the very faults
    // this diagnostic is intended to measure.
    const double frame_ms = std::chrono::duration<double, std::milli>(now - previous).count();
    frames.push_back(frame_ms);
    previous = now;
    // Where a hitch went: GPU-thread time per stage for this one frame. Idle is time spent
    // waiting for the game to submit commands (a guest-side stall).
    if (frame_ms > 24 && phase != "unmarked" && slow_frames_logged < 600) {
      ++slow_frames_logged;
      auto d = [&](Stage s) { return ms[size_t(s)] - at_previous_frame[size_t(s)]; };
      REXGPU_INFO("FM4 slow frame: phase={} ms={:.1f} idle={:.1f} commands={:.1f} guest_wait={:.1f} draw={:.1f} "
          "resolve={:.1f} submit={:.1f} pipeline={:.1f} record={:.1f} queue={:.1f} fence={:.1f} wait={:.1f} swap={:.1f} other={:.1f}",
          phase, frame_ms, d(Stage::Idle), d(Stage::Commands), d(Stage::GuestWait), d(Stage::Draw), d(Stage::Resolve),
          d(Stage::Submit), d(Stage::Pipeline), d(Stage::Record), d(Stage::Queue), d(Stage::Fence), d(Stage::Wait),
          d(Stage::Swap), d(Stage::Other));
    }
    at_previous_frame = ms;
    double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds < 5 || frames.size() < 2) return false;
    const auto cpu = ThreadCpu();
    std::sort(frames.begin(), frames.end());
    const double count = double(frames.size());
    auto percentile = [&](double p) { return frames[std::min(frames.size() - 1, size_t(std::ceil(count * p)) - 1)]; };
    REXGPU_INFO("FM4 phase metrics: phase={} source={}x{} frames={} seconds={:.3f} fps={:.2f} "
        "p50_ms={:.2f} p95_ms={:.2f} p99_ms={:.2f} max_ms={:.2f} gpu_thread_cpu_ms={:.2f} "
        "idle_ms={:.2f} commands_ms={:.2f} guest_wait_ms={:.2f} draw_ms={:.2f} resolve_ms={:.2f} submit_ms={:.2f} pipeline_ms={:.2f} record_ms={:.2f} queue_ms={:.2f} fence_cpu_ms={:.2f} gpu_wait_ms={:.2f} swap_ms={:.2f} other_ms={:.2f} "
        "gpu_commands_ms={:.2f} gpu_timed_submits={:.1f} gpu_resolve_ms={:.2f} gpu_timed_resolves={:.1f} "
        "draws={:.1f} resolves={:.1f} submits={:.1f} waits={:.1f} constant_calls={:.1f} constant_skips={:.1f} type0_words={:.1f} type0_constant_words={:.1f} bulk_state_words={:.1f} draw_state_hits={:.1f} pipeline_state_hits={:.1f} shader_state_hits={:.1f} raster_state_hits={:.1f} system_state_hits={:.1f} sampler_state_hits={:.1f} system_constants_ms={:.3f} bindings_ms={:.3f} textures_ms={:.3f} primitives_ms={:.3f} camera_ms={:.3f}",
        phase, width, height, frames.size(), seconds, count / seconds,
        percentile(.5), percentile(.95), percentile(.99), frames.back(),
        cpu >= previous_cpu ? double(cpu - previous_cpu) / 10000.0 / count : 0,
        ms[size_t(Stage::Idle)] / count,
        ms[size_t(Stage::Commands)] / count, ms[size_t(Stage::GuestWait)] / count,
        ms[size_t(Stage::Draw)] / count, ms[size_t(Stage::Resolve)] / count,
        ms[size_t(Stage::Submit)] / count, ms[size_t(Stage::Pipeline)] / count,
        ms[size_t(Stage::Record)] / count, ms[size_t(Stage::Queue)] / count,
        ms[size_t(Stage::Fence)] / count,
        ms[size_t(Stage::Wait)] / count, ms[size_t(Stage::Swap)] / count, ms[size_t(Stage::Other)] / count,
        gpu_commands_ms / count, double(gpu_timed_submissions) / count,
        gpu_resolve_ms / count, double(gpu_timed_resolves) / count,
        calls[size_t(Stage::Draw)] / count, calls[size_t(Stage::Resolve)] / count,
        calls[size_t(Stage::Submit)] / count, calls[size_t(Stage::Wait)] / count,
        double(constant_calls) / count, double(constant_skips) / count,
        double(type0_words) / count, double(type0_constant_words) / count,
        double(bulk_state_words) / count,
        double(draw_state_hits) / count, double(pipeline_state_hits) / count,
        double(shader_state_hits) / count, double(raster_state_hits) / count,
        double(system_state_hits) / count, double(sampler_state_hits) / count,
        ms[size_t(Stage::Constants)] / count, ms[size_t(Stage::Bindings)] / count,
        ms[size_t(Stage::Textures)] / count, ms[size_t(Stage::Primitives)] / count,
        ms[size_t(Stage::Camera)] / count);
    reported_phase = phase; reported_frames = count;
    Reset(now);
    return true;
  }
};
inline Trace& Get() { static thread_local Trace trace; return trace; }
// Optional timestamp ring. Never read or reuse a slot before its submission
// fence completes. Exhaustion skips timing, rather than stalling the game.
struct GpuTrace {
  explicit GpuTrace(bool resolve = false) : resolve(resolve) {}
  bool resolve;
  static constexpr UINT kSlots = 2048;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  uint64_t* mapped = nullptr;
  uint64_t frequency = 0;
  UINT next = 0;
  bool attempted = false;
  struct Pending { UINT slot; uint64_t fence; };
  std::deque<Pending> pending;
  void Poll(uint64_t completed) {
    while (!pending.empty() && pending.front().fence <= completed) {
      const UINT index = pending.front().slot * 2;
      const uint64_t start = mapped[index], end = mapped[index + 1];
      if (!resolve && Line().begin && end >= start)
        Line().Add('G', Line().GpuToCpu(start), Line().GpuToCpu(end), pending.front().fence);
      if (end >= start) {
        const double milliseconds = double(end - start) * 1000.0 / double(frequency);
        if (resolve) { Get().gpu_resolve_ms += milliseconds; ++Get().gpu_timed_resolves; }
        else { Get().gpu_commands_ms += milliseconds; ++Get().gpu_timed_submissions; }
      }
      pending.pop_front();
    }
  }
  template<class CommandList>
  UINT Begin(ID3D12Device* device, ID3D12CommandQueue* queue,
             CommandList* list, uint64_t completed) {
    if (!Get().enabled) return UINT_MAX;
    if (!resolve) Line().queue = queue;
    if (!attempted) {
      attempted = true;
      D3D12_QUERY_HEAP_DESC queries{};
      queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queries.Count = kSlots * 2;
      D3D12_HEAP_PROPERTIES properties{};
      properties.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC buffer{};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = kSlots * 2 * sizeof(uint64_t); buffer.Height = 1;
      buffer.DepthOrArraySize = buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency ||
          FAILED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&heap))) ||
          FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
              &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
          FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
        heap.Reset(); readback.Reset(); mapped = nullptr;
        REXGPU_WARN("FM4 GPU timestamp profiling unavailable; CPU profiling remains active");
      }
    }
    if (!mapped) return UINT_MAX;
    Poll(completed);
    if (pending.size() >= kSlots) return UINT_MAX;
    const UINT slot = next; next = (next + 1) % kSlots;
    if constexpr(requires { list->D3DEndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2); })
      list->D3DEndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    else list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    return slot;
  }
  template<class CommandList>
  void End(CommandList* list, UINT slot, uint64_t fence) {
    if (!resolve) Line().Add('E', Timeline::Now(), Timeline::Now(), fence);
    if (slot == UINT_MAX) return;
    if constexpr(requires { list->D3DEndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2); }) {
      list->D3DEndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
      list->D3DResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                               readback.Get(), slot * 2 * sizeof(uint64_t));
    } else {
      list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
      list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                             readback.Get(), slot * 2 * sizeof(uint64_t));
    }
    pending.push_back({slot, fence});
  }
  void Shutdown() {
    if (mapped) { D3D12_RANGE no_write{0, 0}; readback->Unmap(0, &no_write); }
    mapped = nullptr; pending.clear(); readback.Reset(); heap.Reset(); attempted = false;
  }
};
inline GpuTrace& Gpu() { static thread_local GpuTrace trace; return trace; }
inline GpuTrace& ResolveGpu() { static thread_local GpuTrace trace(true); return trace; }
// GPU time per render pass. A timestamp is written into the command stream whenever the
// guest render target setup (surface info, colour/depth targets, write mask, EDRAM mode)
// changes and at every resolve; the time to the next timestamp is charged to that setup.
// Opt-in with FM4_FRAME_METRICS; never stalls (exhaustion skips timing).
inline uint64_t PassKey(const uint32_t* registers, size_t count) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < count; ++i) { hash ^= registers[i]; hash *= 1099511628211ull; }
  return hash;
}
// Bounded command-recording order, armed for a named metrics phase. Kept
// separate from timings: logging must only be used in correctness captures.
inline uint64_t NextRTOrderEvent() {
  static const char* phase = std::getenv("FM4_RT_ORDER_PHASE");
  static const uint64_t limit = [] {
    const char* value = std::getenv("FM4_RT_ORDER_LIMIT");
    return value ? std::min<uint64_t>(std::strtoull(value, nullptr, 10), 1000000) : 16384;
  }();
  static thread_local uint64_t serial = 0;
  if (!phase || !Get().enabled || Get().phase != phase || serial >= limit) return 0;
  return ++serial;
}

// CPU command counts, independent of timestamp availability and submission
// fences. One lookup per transfer / resolve-clear setup, no queries or waits.
// Report all keys, including setups that issued no transfers.
struct TransferTrace {
  struct Stat {
    std::array<uint32_t, 8> registers{};
    uint64_t setups = 0, transfers = 0, draws = 0;
    uint64_t host_pixels = 0, host_samples = 0;
    uint64_t depth_store_dispatches = 0, depth_store_samples = 0;
    uint64_t clear_draws = 0, native_clears = 0;
    uint64_t stencil_init_clears = 0, resolve_depth_clears = 0, depth_overwrite_tiles = 0;
  };
  std::map<uint64_t, Stat> stats;
  Stat& ForPass(const std::array<uint32_t, 8>& registers) {
    auto& stat = stats[PassKey(registers.data(), registers.size())];
    stat.registers = registers;
    ++stat.setups;
    return stat;
  }
  void Report(const std::string& phase, double frames) {
    if (frames > 0) {
      for (const auto& [key, stat] : stats) {
        const auto& r = stat.registers;
        REXGPU_INFO("FM4 RT work: phase={} key={:016X} setups/frame={:.1f} transfers/frame={:.1f} "
            "transfer_draws/frame={:.1f} host_pixels/frame={:.1f} host_samples/frame={:.1f} "
            "depth_store_dispatches/frame={:.1f} depth_store_samples/frame={:.1f} "
            "clear_draws/frame={:.1f} native_clears/frame={:.1f} "
            "stencil_init_clears/frame={:.1f} resolve_depth_clears/frame={:.1f} "
            "depth_overwrite_tiles/frame={:.1f} pitch={} msaa={} "
            "c0={:03X}/{} c1={:03X}/{} c2={:03X}/{} c3={:03X}/{} depth={:03X}/{} mask={:04X} mode={}",
            phase, key, stat.setups / frames, stat.transfers / frames, stat.draws / frames,
            stat.host_pixels / frames, stat.host_samples / frames,
            stat.depth_store_dispatches / frames, stat.depth_store_samples / frames,
            stat.clear_draws / frames, stat.native_clears / frames,
            stat.stencil_init_clears / frames, stat.resolve_depth_clears / frames,
            stat.depth_overwrite_tiles / frames,
            r[0] & 0x3FFF, (r[0] >> 16) & 3,
            r[1] & 0xFFF, (r[1] >> 16) & 0xF, r[3] & 0xFFF, (r[3] >> 16) & 0xF,
            r[4] & 0xFFF, (r[4] >> 16) & 0xF, r[5] & 0xFFF, (r[5] >> 16) & 0xF,
            r[2] & 0xFFF, (r[2] >> 16) & 1, r[6] & 0xFFFF, r[7] & 7);
      }
    }
    stats.clear();
  }
};
inline TransferTrace& Transfers() { static thread_local TransferTrace trace; return trace; }
struct PassTrace {
  static constexpr UINT kSlots = 32768;
  static constexpr uint64_t kResolveKey = 0x5245534F4C564531ull;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  uint64_t* mapped = nullptr;
  uint64_t frequency = 0;
  bool attempted = false;
  UINT next = 0, in_flight = 0;
  struct Mark { UINT slot; uint64_t key; uint32_t draws; uint32_t batches = 0, barriers = 0; uint64_t vertices = 0; };
  struct Submission { std::vector<Mark> marks; uint64_t fence; };
  std::vector<Mark> open;
  std::deque<Submission> pending;
  uint64_t current = 0;
  struct Info { std::array<uint32_t, 8> registers{}; };
  std::map<uint64_t, Info> info;
  struct Stat { double ms = 0; uint64_t draws = 0, passes = 0, batches = 0, barriers = 0, vertices = 0; };
  std::map<uint64_t, Stat> stats;
  // Barrier kinds in the window: type, before and after state (transitions) packed in a key.
  std::map<uint64_t, uint64_t> barrier_kinds;
  // Charges a draw's host vertex (index) count to the open pass.
  void Vertices(uint32_t count) { if (mapped && !open.empty()) open.back().vertices += count; }
  // Charges a ResourceBarrier batch recorded before the next timestamp to the open pass.
  void Barriers(const D3D12_RESOURCE_BARRIER* barriers, UINT count) {
    if (!mapped || open.empty()) return;
    ++open.back().batches; open.back().barriers += count;
    for (UINT i = 0; i < count; ++i) {
      const auto& b = barriers[i];
      uint64_t kind = uint64_t(b.Type) << 60;
      if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        kind |= (uint64_t(b.Transition.StateBefore) << 30) | uint64_t(b.Transition.StateAfter);
      ++barrier_kinds[kind];
    }
  }
  bool Ready(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (attempted) return mapped != nullptr;
    attempted = true;
    D3D12_QUERY_HEAP_DESC queries{};
    queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queries.Count = kSlots;
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = kSlots * sizeof(uint64_t);
    buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency ||
        FAILED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&heap))) ||
        FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
        FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
      heap.Reset(); readback.Reset(); mapped = nullptr;
    }
    return mapped != nullptr;
  }
  template<class DeferredList>
  void Mark(ID3D12Device* device, ID3D12CommandQueue* queue, DeferredList& list, uint64_t key,
            const uint32_t* registers = nullptr) {
    if (!Get().enabled || !Ready(device, queue)) return;
    if (key == current && !open.empty()) { ++open.back().draws; return; }
    if (in_flight + 2 >= kSlots) return;
    const UINT slot = next; next = (next + 1) % kSlots; ++in_flight;
    list.D3DEndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
    open.push_back({slot, key, key == kResolveKey ? 0u : 1u});
    current = key;
    if (registers && !info.count(key)) std::copy(registers, registers + 8, info[key].registers.begin());
  }
  template<class CommandList>
  void EndSubmission(CommandList* list, uint64_t fence) {
    if (!mapped || open.empty()) { open.clear(); current = 0; return; }
    const UINT end = next; next = (next + 1) % kSlots; ++in_flight;
    list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, end);
    open.push_back({end, 0, 0});
    for (const auto& mark : open)
      list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, mark.slot, 1, readback.Get(),
                             mark.slot * sizeof(uint64_t));
    pending.push_back({std::move(open), fence});
    open.clear(); current = 0;
  }
  void Poll(uint64_t completed) {
    while (!pending.empty() && pending.front().fence <= completed) {
      auto& marks = pending.front().marks;
      for (size_t i = 0; i + 1 < marks.size(); ++i) {
        const uint64_t a = mapped[marks[i].slot], b = mapped[marks[i + 1].slot];
        auto& stat = stats[marks[i].key];
        const double ms = b >= a ? double(b - a) * 1000.0 / double(frequency) : -1;
        if (ms < 0 || ms > 100) continue;  // a timestamp lost to a reset list; skip it
        stat.ms += ms;
        stat.draws += marks[i].draws; ++stat.passes;
        stat.batches += marks[i].batches; stat.barriers += marks[i].barriers; stat.vertices += marks[i].vertices;
      }
      in_flight -= UINT(marks.size());
      pending.pop_front();
    }
  }
  void Report(const std::string& phase, double frames) {
    if (stats.empty() || frames <= 0) return;
    std::vector<std::pair<double, uint64_t>> order;
    double total = 0;
    for (auto& [key, stat] : stats) { order.push_back({stat.ms, key}); total += stat.ms; }
    std::sort(order.rbegin(), order.rend());
    REXGPU_INFO("FM4 pass timing: phase={} gpu_ms_per_frame={:.2f} pass_types={}", phase, total / frames, order.size());
    for (size_t i = 0; i < order.size() && i < 12; ++i) {
      const auto key = order[i].second; const auto& stat = stats[key];
      const auto& r = info[key].registers;
      if (key == kResolveKey) {
        REXGPU_INFO("FM4 pass: resolves ms/frame={:.2f} count/frame={:.1f} barrier_batches/frame={:.1f} "
            "barriers/frame={:.1f}", stat.ms / frames, stat.passes / frames, stat.batches / frames,
            stat.barriers / frames);
        continue;
      }
      // RB_SURFACE_INFO pitch and MSAA; RB_COLORn_INFO base and format; RB_DEPTH_INFO;
      // RB_COLOR_MASK; RB_MODECONTROL EDRAM mode.
      REXGPU_INFO("FM4 pass: ms/frame={:.2f} passes/frame={:.1f} draws/frame={:.1f} kvertices/frame={:.1f} "
          "barrier_batches/frame={:.1f} barriers/frame={:.1f} pitch={} msaa={} "
          "c0={:03X}/{} c1={:03X}/{} c2={:03X}/{} c3={:03X}/{} depth={:03X}/{} mask={:04X} mode={}",
          stat.ms / frames, stat.passes / frames, stat.draws / frames, stat.vertices / frames / 1000.0,
          stat.batches / frames, stat.barriers / frames, r[0] & 0x3FFF, (r[0] >> 16) & 3,
          r[1] & 0xFFF, (r[1] >> 16) & 0xF, r[3] & 0xFFF, (r[3] >> 16) & 0xF, r[4] & 0xFFF, (r[4] >> 16) & 0xF,
          r[5] & 0xFFF, (r[5] >> 16) & 0xF, r[2] & 0xFFF, (r[2] >> 16) & 1, r[6] & 0xFFFF, r[7] & 7);
    }
    std::vector<std::pair<uint64_t, uint64_t>> kinds;
    for (auto& [kind, count] : barrier_kinds) kinds.push_back({count, kind});
    std::sort(kinds.rbegin(), kinds.rend());
    for (size_t i = 0; i < kinds.size() && i < 8; ++i) {
      const uint64_t kind = kinds[i].second;
      REXGPU_INFO("FM4 barrier kind: per_frame={:.1f} type={} before={:X} after={:X}", kinds[i].first / frames,
          kind >> 60, (kind >> 30) & 0x3FFFFFFF, kind & 0x3FFFFFFF);
    }
    barrier_kinds.clear();
    stats.clear();
  }
};
inline PassTrace& Passes() { static thread_local PassTrace trace; return trace; }
struct Scope {
  Trace& trace = Get();
  Stage previous = trace.stage;
  explicit Scope(Stage next) {
    if (!trace.enabled) return;
    trace.Mark(Clock::now()); trace.stage = next; ++trace.calls[size_t(next)];
  }
  ~Scope() {
    if (!trace.enabled) return;
    trace.Mark(Clock::now()); trace.stage = previous;
  }
};
inline void Frame(uint32_t width, uint32_t height) {
  // The trace can use metrics-phase.txt without enabling CPU/GPU timers.
  if (fm4::frame_trace::enabled && !Get().enabled) {
    Get().ReadPhase();
    fm4::frame_trace::Boundary(Get().phase);
  }
  if (!Get().enabled) return;
  const auto previous_window = Get().window;
  if (Get().Frame(width, height)) {
    Passes().Report(Get().reported_phase, Get().reported_frames);
    Transfers().Report(Get().reported_phase, Get().reported_frames);
  } else if (Get().window != previous_window) {
    // Discard initialization / phase-change commands with the CPU window.
    Transfers().stats.clear();
  }
  if (fm4::frame_trace::enabled) fm4::frame_trace::Boundary(Get().phase);
}
}  // namespace fm4::frame_metrics
