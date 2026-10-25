# GPU-ordered shader completion and synthetic visibility counter snapshots.
# Modified 2026-10-18. GPL-3.0-only. Applied as build-local source/header overlays.
# Cumulative query semantics adapted from Xenia Canary, Copyright 2026 Ben Vanik,
# under the retained BSD-3-Clause notice in licenses/rexglue-xenia-bsd.txt.
# Reference: xenia-canary/xenia-canary@50e9c715df05e3d93ac18e253d14956900683226
# src/xenia/gpu/{pm4_command_processor_implement.h,xenos_zpd_report.h}.
if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
  return()
endif()
function(fm4_completion_replace variable before after reason)
  string(FIND "${${variable}}" "${before}" fm4_match)
  if(fm4_match LESS 0)
    message(FATAL_ERROR "Pinned GPU source does not match shader completion ${reason}")
  endif()
  string(REPLACE "${before}" "${after}" fm4_result "${${variable}}")
  set(${variable} "${fm4_result}" PARENT_SCOPE)
endfunction()

set(fm4_completion_root "${CMAKE_BINARY_DIR}/fm4-shader-completion")
set(fm4_completion_include "${fm4_completion_root}/include")
get_target_property(fm4_completion_sources rexgpu-xenos SOURCES)
set(fm4_completion_replaced 0)
set(fm4_completion_target_sources)
foreach(fm4_entry IN LISTS fm4_completion_sources)
  set(fm4_part "")
  if(fm4_entry STREQUAL "command_processor.cpp" OR fm4_entry MATCHES "/core_command_processor[.]cpp$")
    set(fm4_part core)
  elseif(fm4_entry STREQUAL "d3d12/command_processor.cpp" OR fm4_entry MATCHES "/d3d_command_processor[.]cpp$")
    set(fm4_part d3d)
  endif()
  if(fm4_part)
    if(IS_ABSOLUTE "${fm4_entry}")
      set(fm4_input "${fm4_entry}")
    else()
      set(fm4_input "${CMAKE_SOURCE_DIR}/src/graphics/${fm4_entry}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fm4_input}")
    file(READ "${fm4_input}" fm4_completion_${fm4_part})
    string(REPLACE "\r\n" "\n" fm4_completion_${fm4_part} "${fm4_completion_${fm4_part}}")
    list(APPEND fm4_completion_target_sources "${fm4_completion_root}/${fm4_part}_command_processor.cpp")
    math(EXPR fm4_completion_replaced "${fm4_completion_replaced}+1")
  else()
    list(APPEND fm4_completion_target_sources "${fm4_entry}")
  endif()
endforeach()
if(NOT fm4_completion_replaced EQUAL 2)
  message(FATAL_ERROR "Shader completion expected exactly two reviewed GPU sources")
endif()

set(fm4_completion_old_shd [=[  memory::store(memory_->TranslatePhysical(address), data_value);
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), 4);
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT]=])
set(fm4_completion_new_shd [=[  return WriteShaderCompletion(address, data_value);
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT]=])
fm4_completion_replace(fm4_completion_core "${fm4_completion_old_shd}" "${fm4_completion_new_shd}" "SHD dispatch")
set(fm4_completion_base [=[bool CommandProcessor::WriteShaderCompletion(uint32_t address, uint32_t value, bool visibility_data) {
  (void)visibility_data;
  memory::store(memory_->TranslatePhysical(address), value);
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), 4);
  return true;
}

]=])
fm4_completion_replace(fm4_completion_core "void CommandProcessor::PrepareForWait() {"
  "${fm4_completion_base}void CommandProcessor::PrepareForWait() {" "base implementation")

# Synthetic visibility reports are cumulative CPU snapshots, separate from
# real GPU shader-completion labels. Guest code can overwrite pending-query
# sentinels while queued GPU work is still running. Keep fake reports immediate
# and publish the ZPass/Stencil readiness half after the other counter fields.
set(fm4_visibility_old [=[    std::memset(pSampleCounts, 0, sizeof(xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      pSampleCounts->ZPass_A = fake_sample_count;
      pSampleCounts->Total_A = fake_sample_count;
    }]=])
set(fm4_visibility_new [=[    if (REXCVAR_GET(gpu_occlusion_query_cumulative_fallback)) {
      // Xenos reports snapshots of free-running counters. QueryBatch subtracts
      // adjacent snapshots without a conventional BEGIN/END sentinel pair.
      // Follow Xenia Canary's cumulative fake-report semantics, retaining the
      // configured conservative visible-sample increment. This is synthetic
      // visibility data, so it is published immediately, like Canary's fake
      // path, rather than waiting for GPU rendering to finish.
      static thread_local uint32_t running_samples = 0;
      const auto completed = fm4::visibility::Next(running_samples, uint32_t(fake_sample_count));
      // D3D may poll ZPass_A or StencilFail_B. Publish the ready half last;
      // Total_A is not the readiness marker used by the guest query API.
      fm4::visibility::Publish(pSampleCounts, completed);
      trace_writer_.WriteMemoryWrite(CpuToGpu(
          register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]), sizeof(completed));
      static bool cumulative_logged = false;
      if (!cumulative_logged) {
        REXGPU_INFO("FM4 cumulative visibility snapshots active; native readiness counters published last");
        cumulative_logged = true;
      }
      return true;
    }
    xe_gpu_depth_sample_counts completed{};
    if (is_end_via_z_pass || is_end_via_z_fail) {
      completed.ZPass_A = fake_sample_count;
      completed.Total_A = fake_sample_count;
    }
    static_assert(sizeof(completed) == 32);
    std::memcpy(pSampleCounts, &completed, 16);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&pSampleCounts->ZPass_A, &completed.ZPass_A, 16);
    trace_writer_.WriteMemoryWrite(CpuToGpu(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]), sizeof(completed));]=])
fm4_completion_replace(fm4_completion_core "${fm4_visibility_old}" "${fm4_visibility_new}" "visibility result ordering")
set(fm4_visibility_setting [=[REXCVAR_DEFINE_BOOL(gpu_occlusion_query_cumulative_fallback, true, "GPU",
                    "Use free-running synthetic visibility snapshots for conventional and batched queries")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

]=])
file(MAKE_DIRECTORY "${fm4_completion_include}/rex/graphics")
set(fm4_visibility_header "${CMAKE_CURRENT_LIST_DIR}/Visibility-Counter.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fm4_visibility_header}")
configure_file("${fm4_visibility_header}" "${fm4_completion_include}/rex/graphics/fm4_visibility_counter.h" COPYONLY)
set(fm4_completion_core "#include <rex/graphics/fm4_visibility_counter.h>\n${fm4_completion_core}")
fm4_completion_replace(fm4_completion_core "REXCVAR_DEFINE_BOOL(vsync,"
  "${fm4_visibility_setting}REXCVAR_DEFINE_BOOL(vsync," "cumulative visibility setting")

set(fm4_completion_cvar [=[REXCVAR_DEFINE_BOOL(d3d12_fence_early_completion, false, "GPU/D3D12",
                    "Complete clean guest fences on the command processor; retain GPU ordering for CPU-visible writes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(d3d12_readback_resolve_gpu_completion, true, "GPU/D3D12",
                    "Order shader completion writes on the GPU when full host resolve batching is enabled")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(d3d12_readback_resolve_completion_boundary, true, "GPU/D3D12",
                    "Submit each ordered shader completion at its event position before recording later resolves")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

]=])
fm4_completion_replace(fm4_completion_d3d "REXCVAR_DEFINE_BOOL(d3d12_bindless,"
  "${fm4_completion_cvar}REXCVAR_DEFINE_BOOL(d3d12_bindless," "setting")
# Keep synthetic EXT extents immediate. Delaying EXT changes guest rendering
# decisions and produced a reproducible tint bug.
set(fm4_completion_methods [=[bool D3D12CommandProcessor::PrepareForGuestMemoryWrite() {
  if (!REXCVAR_GET(d3d12_fence_early_completion)) return true;
  const uint64_t pending = fence_completion_.PendingSubmission();
  if (pending <= submission_completed_) return true;
  // Submit an open producer before waiting. This covers imported RAM copies,
  // immutable on-demand snapshots and previously queued shader markers.
  CheckSubmissionFence(pending);
  return pending <= submission_completed_;
}

bool D3D12CommandProcessor::GpuOrderedShaderCompletionEnabled() {
  return REXCVAR_GET(d3d12_readback_resolve_gpu_completion) &&
      REXCVAR_GET(d3d12_readback_resolve_frame_batch) &&
      !REXCVAR_GET(occlusion_query_enable) && !trace_writer_.is_open() &&
      shared_memory_ && shared_memory_->GetReadbackResolveHostBuffer();
}

bool D3D12CommandProcessor::WriteShaderCompletion(uint32_t address, uint32_t value, bool visibility_data) {
  const bool enabled = GpuOrderedShaderCompletionEnabled() &&
      uint64_t(address) + sizeof(value) <= SharedMemory::kBufferSize;
  // Trace capture may change at a frame boundary. Drain outstanding writes
  // before returning to CPU completion so an older GPU write cannot arrive late.
  if (gpu_shader_completion_active_ && !enabled) {
    if (!AwaitAllQueueOperationsCompletion()) return false;
    readback_host_copies_pending_ = false;
  }
  if (enabled && !gpu_shader_completion_active_)
    REXGPU_INFO("FM4 GPU-ordered shader completion active; host resolve copies remain full");
  gpu_shader_completion_active_ = enabled;
  if (REXCVAR_GET(d3d12_fence_early_completion)) {
    CheckSubmissionFence(0);
    if (!visibility_data && fence_completion_.CanCompleteEarly(submission_completed_)) {
      ++fence_early_count_;
      if ((fence_early_count_ & 4095) == 1)
        REXGPU_INFO("Guest fence completion: early={} ordered={} address={:08X}",
                    fence_early_count_, fence_ordered_count_, address);
      return CommandProcessor::WriteShaderCompletion(address, value, visibility_data);
    }
    // A CPU write must never overtake an older GPU marker, including when
    // tracing or a configuration transition disables GPU-ordered completion.
    if (!enabled && !PrepareForGuestMemoryWrite()) return false;
  }
  if (!enabled) return CommandProcessor::WriteShaderCompletion(address, value, visibility_data);
  if (!BeginSubmission(true)) return false;
  ID3D12Resource* upload = nullptr;
  size_t offset = 0;
  auto* mapping = constant_buffer_pool_->Request(frame_current_, sizeof(value), 16,
                                                &upload, &offset, nullptr);
  if (!mapping) {
    if (!AwaitAllQueueOperationsCompletion()) return false;
    return CommandProcessor::WriteShaderCompletion(address, value, visibility_data);
  }
  std::memcpy(mapping, &value, sizeof(value));
  shared_memory_->UseReadbackResolveHostAsCopyDestination();
  SubmitBarriers();
  if (visibility_data) {
    // Counter payload belongs to the main command list. Its marker is in a
    // separate ExecuteCommandLists call, which guarantees all payload copies
    // finish first even if independent copies within one list overlap.
    deferred_command_list_.D3DCopyBufferRegion(shared_memory_->GetReadbackResolveHostBuffer(),
        address, upload, offset, sizeof(value));
  } else {
    gpu_shader_completions_.push_back({address, upload, offset});
  }
  if (REXCVAR_GET(d3d12_fence_early_completion)) {
    fence_completion_.OrderedFence(submission_current_);
    ++fence_ordered_count_;
    if ((fence_ordered_count_ & 4095) == 1)
      REXGPU_INFO("Guest fence completion: early={} ordered={} address={:08X}",
                  fence_early_count_, fence_ordered_count_, address);
  }
  readback_host_copies_pending_ = true;
  // A marker accumulated until a later batch boundary may be overtaken by
  // later resolves in the main list. Keep the event's original command
  // position by submitting this list and its marker now. This queues work;
  // it does not synchronously wait for the GPU on the command-processor CPU.
  if (REXCVAR_GET(d3d12_readback_resolve_completion_boundary))
    return EndSubmission(false);
  return true;
}

]=])
if(EXISTS "${CMAKE_SOURCE_DIR}/include/rex/graphics/d3d12/fm4_frame_metrics.h")
  # Same-process paused comparisons are available only to the explicitly
  # requested hidden, muted metrics harness. Foreground behavior uses CVars.
  string(REPLACE "bool D3D12CommandProcessor::GpuOrderedShaderCompletionEnabled() {"
    [=[bool D3D12CommandProcessor::GpuOrderedShaderCompletionEnabled() {
  static const bool profile = std::getenv("FM4_BACKGROUND") &&
      std::getenv("FM4_FRAME_METRICS") && std::getenv("FM4_PROFILE_SHADER_COMPLETION");
  if (profile && fm4::frame_metrics::Get().phase.starts_with("sync_")) return false;]=]
    fm4_completion_methods "${fm4_completion_methods}")
endif()
fm4_completion_replace(fm4_completion_d3d "void D3D12CommandProcessor::OnPrimaryBufferEnd() {"
  "${fm4_completion_methods}void D3D12CommandProcessor::OnPrimaryBufferEnd() {" "D3D12 implementation")
fm4_completion_replace(fm4_completion_d3d "  if (readback_host_copies_pending_) {"
  "  if (readback_host_copies_pending_ && !GpuOrderedShaderCompletionEnabled()) {" "swap/wait fallback")
# PrepareForWait must submit ordered values when the command processor polls
# guest memory. It must not wait for the GPU on the command processor thread.
fm4_completion_replace(fm4_completion_d3d "void D3D12CommandProcessor::PrepareForWait() {"
  [=[void D3D12CommandProcessor::PrepareForWait() {
  if (readback_host_copies_pending_ && GpuOrderedShaderCompletionEnabled()) {
    if (!EndSubmission(false)) REXGPU_ERROR("Shader completion submission failed");
    CommandProcessor::PrepareForWait();
    return;
  }]=] "guest polling submission")

set(fm4_completion_record [=[    // Successive ExecuteCommandLists calls establish a full completion
    // boundary: host resolve copies and guest shaders finish before the guest
    // CPU can observe the shader-done value. The upload pool and both command
    // lists share the existing frame/submission allocator and completion fence.
    if (!gpu_shader_completions_.empty()) {
      if (!gpu_shader_completion_list_) {
        if (FAILED(provider.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                command_allocator, nullptr, IID_PPV_ARGS(&gpu_shader_completion_list_)))) return false;
        gpu_shader_completion_list_->SetName(L"FM4 GPU-ordered shader completion");
      } else if (FAILED(gpu_shader_completion_list_->Reset(command_allocator, nullptr))) return false;
      for (const auto& write : gpu_shader_completions_)
        gpu_shader_completion_list_->CopyBufferRegion(shared_memory_->GetReadbackResolveHostBuffer(),
            write.address, write.source, write.offset, sizeof(uint32_t));
      if (FAILED(gpu_shader_completion_list_->Close())) return false;
    }
]=])
fm4_completion_replace(fm4_completion_d3d "    ID3D12CommandList* execute_command_lists[] = {command_list_};"
  "${fm4_completion_record}    ID3D12CommandList* execute_command_lists[] = {command_list_};" "completion list recording")
fm4_completion_replace(fm4_completion_d3d "direct_queue->ExecuteCommandLists(1, execute_command_lists);"
  [=[direct_queue->ExecuteCommandLists(1, execute_command_lists);
      if (!gpu_shader_completions_.empty()) {
        ID3D12CommandList* completion_lists[] = {gpu_shader_completion_list_.Get()};
        direct_queue->ExecuteCommandLists(1, completion_lists);
        gpu_shader_completions_.clear();
      }]=] "ordered submission")
fm4_completion_replace(fm4_completion_d3d "    submission_open_ = false;"
  "    submission_open_ = false;\n    if (GpuOrderedShaderCompletionEnabled()) readback_host_copies_pending_ = false;" "submitted state")
fm4_completion_replace(fm4_completion_d3d "void D3D12CommandProcessor::ShutdownContext() {\n  AwaitAllQueueOperationsCompletion();"
  "void D3D12CommandProcessor::ShutdownContext() {\n  AwaitAllQueueOperationsCompletion();\n  gpu_shader_completion_list_.Reset();\n  gpu_shader_completions_.clear();" "shutdown")

file(READ "${CMAKE_SOURCE_DIR}/include/rex/graphics/command_processor.h" fm4_completion_core_header)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/include/rex/graphics/command_processor.h")
string(REPLACE "\r\n" "\n" fm4_completion_core_header "${fm4_completion_core_header}")
fm4_completion_replace(fm4_completion_core_header "  virtual void PrepareForWait();"
  "  virtual bool PrepareForGuestMemoryWrite() { return true; }\n  virtual bool WriteShaderCompletion(uint32_t address, uint32_t value, bool visibility_data = false);\n  virtual void PrepareForWait();" "base declaration")
set(fm4_completion_d3d_path "rex/graphics/d3d12/command_processor.h")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/include/rex/graphics/d3d12/fm4_frame_metrics.h" AND
   EXISTS "${CMAKE_BINARY_DIR}/fm4-runtime-fixes/include/${fm4_completion_d3d_path}")
  set(fm4_completion_d3d_input "${CMAKE_BINARY_DIR}/fm4-runtime-fixes/include/${fm4_completion_d3d_path}")
else()
  set(fm4_completion_d3d_input "${CMAKE_SOURCE_DIR}/include/${fm4_completion_d3d_path}")
endif()
file(READ "${fm4_completion_d3d_input}" fm4_completion_d3d_header)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fm4_completion_d3d_input}")
string(REPLACE "\r\n" "\n" fm4_completion_d3d_header "${fm4_completion_d3d_header}")
fm4_completion_replace(fm4_completion_d3d_header "  void PrepareForWait() override;"
  "  bool PrepareForGuestMemoryWrite() override;\n  bool WriteShaderCompletion(uint32_t address, uint32_t value, bool visibility_data = false) override;\n  bool GpuOrderedShaderCompletionEnabled();\n  void PrepareForWait() override;" "D3D12 declarations")
set(fm4_completion_fields [=[
  struct GpuShaderCompletion {
    uint32_t address;
    ID3D12Resource* source;
    size_t offset;
  };
  std::vector<GpuShaderCompletion> gpu_shader_completions_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> gpu_shader_completion_list_;
  bool gpu_shader_completion_active_ = false;
  fm4::fences::CompletionState fence_completion_;
  uint64_t fence_early_count_ = 0;
  uint64_t fence_ordered_count_ = 0;]=])
fm4_completion_replace(fm4_completion_d3d_header "  std::vector<draw_util::MemExportRange> memexport_ranges_;"
  "  std::vector<draw_util::MemExportRange> memexport_ranges_;${fm4_completion_fields}" "D3D12 state")

# Track the last producer submission, rather than a per-frame flag: submission
# closure is not GPU completion, and several fences may follow one readback.
fm4_completion_replace(fm4_completion_d3d
  "  if (use_resolve_host_copy) {\n    if (REXCVAR_GET(d3d12_readback_resolve_frame_batch)"
  "  if (use_resolve_host_copy) {\n    if (REXCVAR_GET(d3d12_fence_early_completion)) fence_completion_.CpuWrite(submission_current_);\n    if (REXCVAR_GET(d3d12_readback_resolve_frame_batch)" "CPU-visible resolve producer")
fm4_completion_replace(fm4_completion_d3d "      if (memexport_total_size != 0) {"
  "      if (memexport_total_size != 0) {\n        if (REXCVAR_GET(d3d12_fence_early_completion)) fence_completion_.CpuWrite(submission_current_);" "memexport readback producer")
fm4_completion_replace(fm4_completion_d3d "  snapshot->producer_submission = submission_current_;"
  "  snapshot->producer_submission = submission_current_;\n  if (REXCVAR_GET(d3d12_fence_early_completion)) fence_completion_.CpuWrite(submission_current_);" "on-demand resolve producer")
# Shader labels have a GPU path above. Other CPU-emulated writes and interrupts
# must drain older producers/labels before publishing guest-visible completion.
# Synthetic EXT/ZPD reports retain their immediate, CPU-only visibility semantics.
foreach(fm4_write_opcode INTERRUPT REG_TO_MEM MEM_WRITE COND_WRITE)
  fm4_completion_replace(fm4_completion_core "    case PM4_${fm4_write_opcode}:\n"
    "    case PM4_${fm4_write_opcode}:\n      if (!PrepareForGuestMemoryWrite()) return false;\n" "${fm4_write_opcode} ordering")
endforeach()
set(fm4_fence_header "${CMAKE_CURRENT_LIST_DIR}/Fence-Completion.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fm4_fence_header}")
configure_file("${fm4_fence_header}" "${fm4_completion_include}/rex/graphics/fm4_fence_completion.h" COPYONLY)
set(fm4_completion_d3d_header "#include <rex/graphics/fm4_fence_completion.h>\n${fm4_completion_d3d_header}")
install(FILES "${fm4_fence_header}" DESTINATION "include/rex/graphics" RENAME fm4_fence_completion.h)

file(MAKE_DIRECTORY "${fm4_completion_include}/rex/graphics/d3d12")
foreach(fm4_part core d3d)
  set(fm4_completion_output "${fm4_completion_root}/${fm4_part}_command_processor.cpp")
  if(fm4_part STREQUAL core)
    set(fm4_completion_header "${fm4_completion_include}/rex/graphics/command_processor.h")
    set(fm4_completion_header_dest "include/rex/graphics")
  else()
    set(fm4_completion_header "${fm4_completion_include}/${fm4_completion_d3d_path}")
    set(fm4_completion_header_dest "include/rex/graphics/d3d12")
  endif()
  foreach(fm4_kind source header)
    if(fm4_kind STREQUAL source)
      set(fm4_dest "${fm4_completion_output}")
      set(fm4_content "${fm4_completion_${fm4_part}}")
    else()
      set(fm4_dest "${fm4_completion_header}")
      set(fm4_content "${fm4_completion_${fm4_part}_header}")
    endif()
    set(fm4_previous "")
    if(EXISTS "${fm4_dest}")
      file(READ "${fm4_dest}" fm4_previous)
    endif()
    if(NOT fm4_previous STREQUAL fm4_content)
      file(WRITE "${fm4_dest}" "${fm4_content}")
    endif()
  endforeach()
  install(FILES "${fm4_completion_header}" DESTINATION "${fm4_completion_header_dest}")
endforeach()
target_include_directories(rexgpu-xenos BEFORE PRIVATE "${fm4_completion_include}")
set_property(TARGET rexgpu-xenos PROPERTY SOURCES "${fm4_completion_target_sources}")
set_property(SOURCE "${fm4_completion_root}/d3d_command_processor.cpp" TARGET_DIRECTORY rexgpu-xenos APPEND PROPERTY
  INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/graphics/d3d12")
