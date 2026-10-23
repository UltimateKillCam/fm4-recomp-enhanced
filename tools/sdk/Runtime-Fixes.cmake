# Pinned runtime corrections. Modified 2026-10-17. GPL-3.0-only.
# Generate build-local source overlays; the verified SDK checkout stays clean.
if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
  return()
endif()
# The separately patched graphics SDK already contains these corrections.
if(EXISTS "${CMAKE_SOURCE_DIR}/include/rex/graphics/d3d12/fm4_frame_metrics.h")
  return()
endif()
set(fm4_fixed_root "${CMAKE_BINARY_DIR}/fm4-runtime-fixes")
file(MAKE_DIRECTORY "${fm4_fixed_root}")
file(READ "${CMAKE_SOURCE_DIR}/src/graphics/command_processor.cpp" fm4_core_source)
string(REPLACE "\r\n" "\n" fm4_core_source "${fm4_core_source}")
string(REGEX MATCHALL "[.]range[(]1, 100000[)]" fm4_ranges "${fm4_core_source}")
list(LENGTH fm4_ranges fm4_range_count)
if(NOT fm4_range_count EQUAL 1)
  message(FATAL_ERROR "Pinned GPU source does not match the reviewed occlusion-range correction")
endif()
string(REPLACE ".range(1, 100000)" ".range(1, 2147483647)" fm4_core_source "${fm4_core_source}")

set(fm4_old_type0 [=[  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
    uint32_t target_index = write_one_reg ? base_index : base_index + m;
    WriteRegister(target_index, reg_data);
  }]=])
set(fm4_new_type0 [=[  if (REXCVAR_GET(gpu_bulk_type0_register_writes) && !write_one_reg) {
    // Reuse the existing endian-copy and invalidation handlers for sequential
    // packets. Repeated writes to one register retain their original order.
    WriteRegisterRangeFromRing(reader, base_index, count);
  } else {
    for (uint32_t m = 0; m < count; m++) {
      uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
      uint32_t target_index = write_one_reg ? base_index : base_index + m;
      WriteRegister(target_index, reg_data);
    }
  }]=])
string(FIND "${fm4_core_source}" "${fm4_old_type0}" fm4_type0_at)
if(fm4_type0_at LESS 0)
  message(FATAL_ERROR "Pinned GPU source does not match the reviewed Type-0 range-write correction")
endif()
string(REPLACE "${fm4_old_type0}" "${fm4_new_type0}" fm4_core_source "${fm4_core_source}")
set(fm4_vsync [=[REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");]=])
set(fm4_bulk_cvar [=[REXCVAR_DEFINE_BOOL(gpu_bulk_type0_register_writes, true, "GPU",
                    "Use existing range-write handlers for sequential Type-0 register packets")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);]=])
string(FIND "${fm4_core_source}" "${fm4_vsync}" fm4_vsync_at)
if(fm4_vsync_at LESS 0)
  message(FATAL_ERROR "Pinned GPU source does not match the reviewed Type-0 setting insertion")
endif()
string(REPLACE "${fm4_vsync}" "${fm4_vsync}\n\n${fm4_bulk_cvar}" fm4_core_source "${fm4_core_source}")

file(READ "${CMAKE_SOURCE_DIR}/src/graphics/d3d12/command_processor.cpp" fm4_d3d_source)
string(REPLACE "\r\n" "\n" fm4_d3d_source "${fm4_d3d_source}")
set(fm4_old_fence [=[if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value) &&
                    SUCCEEDED(queue_operations_since_submission_fence_->SetEventOnCompletion(
                        fence_value, fence_completion_event_)))) {]=])
set(fm4_new_fence [=[if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value)) &&
                    SUCCEEDED(queue_operations_since_submission_fence_->SetEventOnCompletion(
                        fence_value, fence_completion_event_))) {]=])
string(FIND "${fm4_d3d_source}" "${fm4_old_fence}" fm4_fence_at)
if(fm4_fence_at LESS 0)
  message(FATAL_ERROR "Pinned D3D12 source does not match the reviewed fence correction")
endif()
string(REPLACE "${fm4_old_fence}" "${fm4_new_fence}" fm4_d3d_source "${fm4_d3d_source}")

function(fm4_checked_replace variable before after reason)
  string(FIND "${${variable}}" "${before}" fm4_match)
  if(fm4_match LESS 0)
    message(FATAL_ERROR "Pinned GPU source does not match the reviewed ${reason}")
  endif()
  string(REPLACE "${before}" "${after}" fm4_result "${${variable}}")
  set(${variable} "${fm4_result}" PARENT_SCOPE)
endfunction()

fm4_checked_replace(fm4_d3d_source "#include <rex/ui/d3d12/d3d12_util.h>"
  "#include <rex/ui/d3d12/d3d12_util.h>\n\nREXCVAR_DECLARE(bool, gpu_bulk_type0_register_writes);" "bulk setting declaration")
set(fm4_scalar_range [=[  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);]=])
set(fm4_state_range [=[  if (REXCVAR_GET(gpu_bulk_type0_register_writes) &&
      start_index >= 0x2000 && end_index < 0x3000) {
    // Pinned D3D12 draw-state bank: scratch, coherency, LUT and constant-binding
    // write-side effects are all outside it. Preserve debug diagnostics.
    auto* logger = rex::GetLoggerRaw(rex::log::gpu());
    if (!logger || !logger->should_log(spdlog::level::debug)) {
      memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
      return;
    }
  }
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);]=])
fm4_checked_replace(fm4_d3d_source "${fm4_scalar_range}" "${fm4_state_range}" "draw-state range writes")

# Keep full copies, but optionally defer their completion wait to guest waits
# and swap. This is opt-in until wider scene/coherency validation is complete.
set(fm4_bindless [=[REXCVAR_DEFINE_BOOL(d3d12_bindless, true, "GPU/D3D12", "Use bindless resources where available")]=])
set(fm4_batch_cvar [=[REXCVAR_DEFINE_BOOL(d3d12_readback_resolve_frame_batch, false, "GPU/D3D12",
                    "Experimental: wait for host resolve copies at swap and guest waits instead of every copy")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

]=])
fm4_checked_replace(fm4_d3d_source "${fm4_bindless}" "${fm4_batch_cvar}${fm4_bindless}" "batch setting insertion")
set(fm4_swap_start [=[  SCOPE_profile_cpu_f("gpu");
  vertex_buffers_in_sync_[0] = 0;]=])
set(fm4_swap_wait [=[  SCOPE_profile_cpu_f("gpu");
  if (readback_host_copies_pending_) {
    readback_host_copies_pending_ = false;
    if (!AwaitAllQueueOperationsCompletion()) return;
  }
  vertex_buffers_in_sync_[0] = 0;]=])
fm4_checked_replace(fm4_d3d_source "${fm4_swap_start}" "${fm4_swap_wait}" "batch swap wait")
set(fm4_prepare_wait [=[void D3D12CommandProcessor::PrepareForWait() {
  if (readback_host_copies_pending_) {
    readback_host_copies_pending_ = false;
    AwaitAllQueueOperationsCompletion();
  }
  CommandProcessor::PrepareForWait();
}

]=])
fm4_checked_replace(fm4_d3d_source "void D3D12CommandProcessor::OnPrimaryBufferEnd() {"
  "${fm4_prepare_wait}void D3D12CommandProcessor::OnPrimaryBufferEnd() {" "batch guest wait")
set(fm4_host_copy [=[  if (use_resolve_host_copy) {
    const auto wait_begin = std::chrono::steady_clock::now();]=])
set(fm4_batched_copy [=[  if (use_resolve_host_copy) {
    if (REXCVAR_GET(d3d12_readback_resolve_frame_batch)) {
      readback_host_copies_pending_ = true;
      return true;
    }
    const auto wait_begin = std::chrono::steady_clock::now();]=])
fm4_checked_replace(fm4_d3d_source "${fm4_host_copy}" "${fm4_batched_copy}" "batch host-copy wait")

set(fm4_overlay_include "${fm4_fixed_root}/include")
set(fm4_d3d_header_path "rex/graphics/d3d12/command_processor.h")
file(READ "${CMAKE_SOURCE_DIR}/include/${fm4_d3d_header_path}" fm4_d3d_header)
string(REPLACE "\r\n" "\n" fm4_d3d_header "${fm4_d3d_header}")
fm4_checked_replace(fm4_d3d_header "  void OnPrimaryBufferEnd() override;"
  "  void PrepareForWait() override;\n  void OnPrimaryBufferEnd() override;" "batch override declaration")
fm4_checked_replace(fm4_d3d_header "  bool submission_open_ = false;"
  "  bool readback_host_copies_pending_ = false;\n  bool submission_open_ = false;" "batch state declaration")
file(MAKE_DIRECTORY "${fm4_overlay_include}/rex/graphics/d3d12")
set(fm4_overlay_header "${fm4_overlay_include}/${fm4_d3d_header_path}")
set(fm4_previous_header "")
if(EXISTS "${fm4_overlay_header}")
  file(READ "${fm4_overlay_header}" fm4_previous_header)
endif()
if(NOT fm4_previous_header STREQUAL fm4_d3d_header)
  file(WRITE "${fm4_overlay_header}" "${fm4_d3d_header}")
endif()
# Compile every GPU source against the same class layout. Install the reviewed
# header after upstream headers so external SDK users see that layout too.
target_include_directories(rexgpu-xenos BEFORE PRIVATE "${fm4_overlay_include}")
install(FILES "${fm4_overlay_header}" DESTINATION "include/rex/graphics/d3d12")

file(READ "${CMAKE_SOURCE_DIR}/src/core/threading_win.cpp" fm4_thread_source)
string(REPLACE "\r\n" "\n" fm4_thread_source "${fm4_thread_source}")
set(fm4_old_alertable [=[SleepResult AlertableSleep(std::chrono::microseconds duration) {
  if (SleepEx(static_cast<DWORD>(duration.count() / 1000), TRUE) == WAIT_IO_COMPLETION) {
    return SleepResult::kAlerted;
  }
  return SleepResult::kSuccess;
}]=])
set(fm4_new_alertable [=[SleepResult AlertableSleep(std::chrono::microseconds duration) {
  if (duration.count() >= 100) {
    thread_local HighResSleepTimer hr_timer;
    if (hr_timer.handle) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(duration.count()) * 10;
      if (SetWaitableTimer(hr_timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
        // Preserve APC delivery while avoiding SleepEx's coarse timer tick.
        DWORD result = WaitForSingleObjectEx(hr_timer.handle, INFINITE, TRUE);
        if (result == WAIT_IO_COMPLETION) {
          CancelWaitableTimer(hr_timer.handle);
          return SleepResult::kAlerted;
        }
        if (result == WAIT_OBJECT_0) return SleepResult::kSuccess;
        CancelWaitableTimer(hr_timer.handle);
      }
    }
  }
  if (SleepEx(static_cast<DWORD>(std::max<int64_t>(duration.count(), 0) / 1000), TRUE) == WAIT_IO_COMPLETION) {
    return SleepResult::kAlerted;
  }
  return SleepResult::kSuccess;
}]=])
fm4_checked_replace(fm4_thread_source "${fm4_old_alertable}" "${fm4_new_alertable}" "alertable timer correction")
set(fm4_timing_output "${fm4_fixed_root}/threading_win.cpp")
set(fm4_previous_timing "")
if(EXISTS "${fm4_timing_output}")
  file(READ "${fm4_timing_output}" fm4_previous_timing)
endif()
if(NOT fm4_previous_timing STREQUAL fm4_thread_source)
  file(WRITE "${fm4_timing_output}" "${fm4_thread_source}")
endif()
get_target_property(fm4_core_sources rexcore SOURCES)
set(fm4_overlaid_core_sources)
set(fm4_timing_replaced 0)
foreach(fm4_source IN LISTS fm4_core_sources)
  if(fm4_source STREQUAL "threading_win.cpp" OR fm4_source STREQUAL "${CMAKE_SOURCE_DIR}/src/core/threading_win.cpp")
    list(APPEND fm4_overlaid_core_sources "${fm4_timing_output}")
    math(EXPR fm4_timing_replaced "${fm4_timing_replaced}+1")
  else()
    list(APPEND fm4_overlaid_core_sources "${fm4_source}")
  endif()
endforeach()
if(NOT fm4_timing_replaced EQUAL 1)
  message(FATAL_ERROR "Pinned core source list does not match the reviewed timer overlay")
endif()
set_property(TARGET rexcore PROPERTY SOURCES "${fm4_overlaid_core_sources}")
set_property(SOURCE "${fm4_timing_output}" TARGET_DIRECTORY rexcore APPEND PROPERTY
  INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/core")

foreach(fm4_part core d3d)
  set(fm4_output "${fm4_fixed_root}/${fm4_part}_command_processor.cpp")
  set(fm4_previous "")
  if(EXISTS "${fm4_output}")
    file(READ "${fm4_output}" fm4_previous)
  endif()
  if(NOT fm4_previous STREQUAL fm4_${fm4_part}_source)
    file(WRITE "${fm4_output}" "${fm4_${fm4_part}_source}")
  endif()
endforeach()

get_target_property(fm4_gpu_sources rexgpu-xenos SOURCES)
set(fm4_overlaid_sources)
set(fm4_replaced 0)
foreach(fm4_source IN LISTS fm4_gpu_sources)
  if(fm4_source STREQUAL "command_processor.cpp")
    list(APPEND fm4_overlaid_sources "${fm4_fixed_root}/core_command_processor.cpp")
    math(EXPR fm4_replaced "${fm4_replaced}+1")
  elseif(fm4_source STREQUAL "d3d12/command_processor.cpp")
    list(APPEND fm4_overlaid_sources "${fm4_fixed_root}/d3d_command_processor.cpp")
    math(EXPR fm4_replaced "${fm4_replaced}+1")
  else()
    list(APPEND fm4_overlaid_sources "${fm4_source}")
  endif()
endforeach()
if(NOT fm4_replaced EQUAL 2)
  message(FATAL_ERROR "Pinned GPU target source list does not match the reviewed overlays")
endif()
set_property(TARGET rexgpu-xenos PROPERTY SOURCES "${fm4_overlaid_sources}")
# Preserve the original source's relative shader-header includes.
set_property(SOURCE "${fm4_fixed_root}/d3d_command_processor.cpp" TARGET_DIRECTORY rexgpu-xenos APPEND PROPERTY
  INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/graphics/d3d12")
