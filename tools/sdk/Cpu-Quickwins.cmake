# Experimental CPU fast paths. All runtime switches default off.
if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
  return()
endif()
set(qw_path "${CMAKE_BINARY_DIR}/fm4-shader-completion/d3d_command_processor.cpp")
file(READ "${qw_path}" qw_text)
macro(qw_replace old new label)
  string(FIND "${qw_text}" "${old}" qw_first)
  string(FIND "${qw_text}" "${old}" qw_last REVERSE)
  if(qw_first LESS 0 OR NOT qw_first EQUAL qw_last)
    message(FATAL_ERROR "CPU fast paths: ${label} anchor missing or ambiguous")
  endif()
  string(REPLACE "${old}" "${new}" qw_text "${qw_text}")
endmacro()
qw_replace([=[REXCVAR_DEFINE_BOOL(d3d12_recorder_thread, false,]=]
 [=[#include "quick_register_update.h"
#include <intrin.h>
REXCVAR_DEFINE_BOOL(d3d12_ticketed_submit_ordering, false, "GPU/D3D12",
                    "Record inline work before waiting for its fixed submission ticket")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(d3d12_constant_blocks, false, "GPU/D3D12",
                    "Swap float constants in blocks and batch stage invalidation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(d3d12_state_domain_batch, false, "GPU/D3D12",
                    "Batch changed draw-state domains without per-word callbacks")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(d3d12_binding_layout_compare, false, "GPU/D3D12",
                    "Compare constant binding layouts as a single immutable bitmap")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(d3d12_recorder_thread, false,]=] "switches")
target_include_directories(rexgpu-xenos PRIVATE "${FM4_CONTROLLER_BRIDGE_DIR}/../graphics")

# Both submitters expose a fixed CPU-dispatch boundary. GPU-fence retirement is
# unchanged; a ticket never permits reuse of a GPU-owned allocator or resource.
string(REPLACE "  void Drain() {" [=[  uint64_t PublishedTicket() {
    std::lock_guard lock(mutex_);
    return pushed_;
  }
  void WaitThrough(uint64_t ticket) { if (ticket) WaitFor(ticket); }
  void Drain() {]=] qw_queues "${qw_text}")
# The facade has no mutex: add its routing methods independently.
string(REPLACE [=[  uint64_t PublishedTicket() {
    std::lock_guard lock(mutex_);
    return pushed_;
  }
  void WaitThrough(uint64_t ticket) { if (ticket) WaitFor(ticket); }
  void Drain() {
    if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.Drain();]=]
 [=[  uint64_t PublishedTicket() {
    return REXCVAR_GET(d3d12_recorder_thread) ? recorder_.PublishedTicket()
                                           : legacy_.PublishedTicket();
  }
  void WaitThrough(uint64_t ticket) {
    if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.WaitThrough(ticket);
    else legacy_.WaitThrough(ticket);
  }
  void Drain() {
    if (REXCVAR_GET(d3d12_recorder_thread)) recorder_.Drain();]=] qw_text "${qw_queues}")
qw_replace([=[    const bool fm4_record_async = fm4_slot &&]=]
 [=[    const uint64_t fm4_inline_dependency = !fm4_slot &&
        REXCVAR_GET(d3d12_ticketed_submit_ordering)
        ? fm4_submit_thread.PublishedTicket() : 0;
    const bool fm4_record_async = fm4_slot &&]=] "inline boundary")
qw_replace([=[      Fm4RecorderTiming timing(Fm4RecorderTiming::DrainInline);
      fm4_submit_thread.Drain();]=]
 [=[      if (!REXCVAR_GET(d3d12_ticketed_submit_ordering)) {
        Fm4RecorderTiming timing(Fm4RecorderTiming::DrainInline);
        fm4_submit_thread.Drain();
      }]=] "overlap inline replay")
qw_replace([=[      direct_queue->ExecuteCommandLists(1, execute_command_lists);]=]
 [=[      if (REXCVAR_GET(d3d12_ticketed_submit_ordering)) {
        Fm4RecorderTiming timing(Fm4RecorderTiming::DrainInline);
        fm4_submit_thread.WaitThrough(fm4_inline_dependency);
      }
      direct_queue->ExecuteCommandLists(1, execute_command_lists);]=] "inline ordering")
qw_replace([=[  Fm4RecorderTiming timing(Fm4RecorderTiming::DrainDirect);
  fm4_submit_thread.Drain();]=]
 [=[  Fm4RecorderTiming timing(Fm4RecorderTiming::DrainDirect);
  if (REXCVAR_GET(d3d12_ticketed_submit_ordering))
    fm4_submit_thread.WaitThrough(fm4_submit_thread.PublishedTicket());
  else fm4_submit_thread.Drain();]=] "direct boundary")
qw_replace([=[{ Fm4RecorderTiming timing(Fm4RecorderTiming::DrainFence); fm4_submit_thread.Drain(); }]=]
 [=[{ Fm4RecorderTiming timing(Fm4RecorderTiming::DrainFence);
        if (REXCVAR_GET(d3d12_ticketed_submit_ordering))
          fm4_submit_thread.WaitThrough(fm4_submit_thread.PublishedTicket());
        else fm4_submit_thread.Drain(); }]=] "fence boundary")

qw_replace([=[    if (REXCVAR_GET(d3d12_precise_constant_updates)) {]=]
 [=[    static const bool fm4_byte_shuffle = [] {
      int info[4]; __cpuid(info, 1); return (info[2] & (1 << 9)) != 0;
    }();
    if (REXCVAR_GET(d3d12_constant_blocks) && fm4_byte_shuffle) {
      const uint64_t* vertex_map = vertex_used && cbuffer_binding_float_vertex_.up_to_date
          ? current_float_constant_map_vertex_ : nullptr;
      const uint64_t* pixel_map = pixel_used && cbuffer_binding_float_pixel_.up_to_date
          ? current_float_constant_map_pixel_ : nullptr;
      uint32_t changed = fm4::registers::CopySwappedConstantBlocks(
          register_file_->values + start_index, base, num_registers,
          start_index - XE_GPU_REG_SHADER_CONSTANT_000_X, vertex_map, pixel_map);
      if (changed & 1) cbuffer_binding_float_vertex_.up_to_date = false;
      if (changed & 2) cbuffer_binding_float_pixel_.up_to_date = false;
      if (fm4_constant_metrics) {
        ++fm4::frame_metrics::Get().constant_calls;
        if (!changed) ++fm4::frame_metrics::Get().constant_skips;
      }
      return;
    }
    if (REXCVAR_GET(d3d12_precise_constant_updates)) {]=] "constant blocks")
qw_replace([=[        fm4::registers::CopySwappedAndNotify(
            register_file_->values + start_index, base, num_registers,]=]
 [=[        static const bool fm4_state_shuffle = [] {
          int info[4]; __cpuid(info, 1); return (info[2] & (1 << 9)) != 0;
        }();
        if (REXCVAR_GET(d3d12_state_domain_batch) && fm4_state_shuffle) {
          draw_state_dirty_ |= fm4::registers::CopySwappedStateDomains(
              register_file_->values + start_index, base, num_registers,
              kDrawStateRegisterDomains.data() + start_index - 0x2000,
              size_t(XE_GPU_REG_VGT_DRAW_INITIATOR) - start_index);
        } else fm4::registers::CopySwappedAndNotify(
            register_file_->values + start_index, base, num_registers,]=] "state domains")

# Shader maps are immutable after translation. Keep buffer/root invalidation and
# descriptor/version checks intact; only consolidate the layout comparison.
qw_replace([=[  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {]=]
 [=[  if (REXCVAR_GET(d3d12_binding_layout_compare)) {
    if (fm4::registers::CopyConstantLayoutIfChanged(current_float_constant_map_vertex_,
            float_constant_map_vertex.float_bitmap)) {
      if (float_constant_count_vertex) cbuffer_binding_float_vertex_.up_to_date = false;
    }
  } else for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {]=]
 "vertex layout")
qw_replace([=[    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {]=]
 [=[    if (REXCVAR_GET(d3d12_binding_layout_compare)) {
      if (fm4::registers::CopyConstantLayoutIfChanged(current_float_constant_map_pixel_,
            float_constant_map_pixel.float_bitmap)) {
        if (float_constant_count_pixel) cbuffer_binding_float_pixel_.up_to_date = false;
      }
    } else for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {]=]
 "pixel layout")
file(WRITE "${qw_path}" "${qw_text}")

# Exact geometry updates only replace metadata. Never cache a read or skip a
# version/ownership check. Adjacent equal content still coalesces as before.
set(qw_original "${CMAKE_SOURCE_DIR}/src/graphics/d3d12/native_rt/aliases.cpp")
file(READ "${qw_original}" qw_text)
qw_replace([=[REXCVAR_DEFINE_BOOL(d3d12_native_alias_reuse_nodes,]=]
 [=[REXCVAR_DEFINE_BOOL(d3d12_native_alias_exact_range, false, "GPU/D3D12",
                    "Update exact native alias intervals without extracting map nodes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(d3d12_native_alias_reuse_nodes,]=] "alias switch")
qw_replace([=[#include <rex/graphics/d3d12/native_rt/interval_assignment.h>]=]
 [=[#include <rex/graphics/d3d12/native_rt/interval_assignment.h>
#include "quick_alias_update.h"]=] "alias helper")
qw_replace([=[  static void AssignIn(RangeMap& ranges, const Range& replacement) {
]=]
 [=[  static void AssignIn(RangeMap& ranges, const Range& replacement) {
    if (REXCVAR_GET(d3d12_native_alias_exact_range) &&
        TryAssignExactInterval(ranges, replacement, SameContent)) return;
]=] "alias exact update")
set(qw_alias_path "${CMAKE_BINARY_DIR}/fm4-shader-completion/quick_aliases.cpp")
file(WRITE "${qw_alias_path}" "${qw_text}")
get_target_property(qw_sources rexgpu-xenos SOURCES)
list(REMOVE_ITEM qw_sources "${qw_original}" "d3d12/native_rt/aliases.cpp")
set_property(TARGET rexgpu-xenos PROPERTY SOURCES "${qw_sources}")
target_sources(rexgpu-xenos PRIVATE "${qw_alias_path}")
message(STATUS "FM4 experimental CPU fast paths applied (runtime defaults off)")
include("${FM4_CONTROLLER_BRIDGE_DIR}/Pm4-Fast.cmake")
