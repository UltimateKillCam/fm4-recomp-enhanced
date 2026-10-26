# Per-pass GPU timing. Modified 2026-10-21. GPL-3.0-only.
# Applied as a build-local edit of the generated Gpu-Completion D3D12 command processor.
# With FM4_FRAME_METRICS set, every change of the guest render target setup and every
# resolve writes a GPU timestamp into the command stream (fm4::frame_metrics::PassTrace),
# and each 5 s metrics window logs GPU milliseconds and resource barriers per frame by
# render pass, plus the most frequent barrier kinds. Off by
# default; the disabled cost is one flag test per draw.
function(fm4_pass_timing)
  if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
    return()
  endif()
  set(fm4_file "${CMAKE_BINARY_DIR}/fm4-shader-completion/d3d_command_processor.cpp")
  if(NOT EXISTS "${fm4_file}")
    message(FATAL_ERROR "Pass timing: generated D3D12 command processor overlay is missing")
  endif()
  file(READ "${fm4_file}" fm4_text)
  string(REPLACE "\r\n" "\n" fm4_text "${fm4_text}")
  macro(fm4_pass_swap old new what)
    string(FIND "${fm4_text}" "${old}" fm4_at)
    string(FIND "${fm4_text}" "${old}" fm4_last REVERSE)
    if(fm4_at LESS 0 OR NOT fm4_at EQUAL fm4_last)
      message(FATAL_ERROR "Pass timing: ${what} anchor missing or ambiguous")
    endif()
    string(REPLACE "${old}" "${new}" fm4_text "${fm4_text}")
  endmacro()
  fm4_pass_swap([=[  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  if (!BeginSubmission(true)) {
    return false;
  }
]=] [=[  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  if (!BeginSubmission(true)) {
    return false;
  }
  // After BeginSubmission: a submission it opens would otherwise discard the timestamp.
  if (fm4::frame_metrics::Get().enabled) {
    const uint32_t* fm4_rv = register_file_->values;
    const uint32_t fm4_pass_registers[8] = {
        fm4_rv[XE_GPU_REG_RB_SURFACE_INFO], fm4_rv[XE_GPU_REG_RB_COLOR_INFO], fm4_rv[XE_GPU_REG_RB_DEPTH_INFO],
        fm4_rv[XE_GPU_REG_RB_COLOR1_INFO], fm4_rv[XE_GPU_REG_RB_COLOR2_INFO], fm4_rv[XE_GPU_REG_RB_COLOR3_INFO],
        fm4_rv[XE_GPU_REG_RB_COLOR_MASK], fm4_rv[XE_GPU_REG_RB_MODECONTROL]};
    fm4::frame_metrics::Passes().Mark(GetD3D12Provider().GetDevice(), GetD3D12Provider().GetDirectQueue(),
        deferred_command_list_, fm4::frame_metrics::PassKey(fm4_pass_registers, 8), fm4_pass_registers);
  }
]=] "draw")
  fm4_pass_swap([=[bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  if (!BeginSubmission(true)) {
    return false;
  }
]=] [=[bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  if (!BeginSubmission(true)) {
    return false;
  }
  if (fm4::frame_metrics::Get().enabled) {
    fm4::frame_metrics::Passes().Mark(GetD3D12Provider().GetDevice(), GetD3D12Provider().GetDirectQueue(),
        deferred_command_list_, fm4::frame_metrics::PassTrace::kResolveKey);
  }
]=] "resolve")
  fm4_pass_swap([=[      deferred_command_list_.Execute(command_list_, command_list_1_);
]=] [=[      deferred_command_list_.Execute(command_list_, command_list_1_);
      fm4::frame_metrics::Passes().EndSubmission(command_list_, submission_current_);
]=] "submission")
  fm4_pass_swap([=[  if (fm4::frame_metrics::Get().enabled) fm4::frame_metrics::Gpu().Poll(submission_completed_);
]=] [=[  if (fm4::frame_metrics::Get().enabled) fm4::frame_metrics::Gpu().Poll(submission_completed_);
  if (fm4::frame_metrics::Get().enabled) fm4::frame_metrics::Passes().Poll(submission_completed_);
]=] "poll")
  fm4_pass_swap([=[  if (barrier_count != 0) {
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
]=] [=[  if (barrier_count != 0) {
    if (fm4::frame_metrics::Get().enabled) fm4::frame_metrics::Passes().Barriers(barriers_.data(), barrier_count);
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
]=] "barriers")
  # Both draw paths (indexed and not) record the host vertex count.
  set(fm4_old "    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
")
  string(REGEX MATCHALL "PROFILE_VERTICES\\(primitive_processing_result\\.host_draw_vertex_count\\)" fm4_vertex_sites "${fm4_text}")
  list(LENGTH fm4_vertex_sites fm4_vertex_site_count)
  if(NOT fm4_vertex_site_count EQUAL 2)
    message(FATAL_ERROR "Pass timing: expected 2 vertex count anchors, found ${fm4_vertex_site_count}")
  endif()
  string(REPLACE "${fm4_old}" "${fm4_old}    if (fm4::frame_metrics::Get().enabled)
      fm4::frame_metrics::Passes().Vertices(primitive_processing_result.host_draw_vertex_count);
" fm4_text "${fm4_text}")
  # Timeline: guest ring kicks and shader-completion (fence) writes.
  fm4_pass_swap([=[void D3D12CommandProcessor::OnPrimaryBufferEnd() {
]=] [=[void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  if (fm4::frame_metrics::Line().begin)
    fm4::frame_metrics::Line().Add('K', fm4::frame_metrics::Timeline::Now(), fm4::frame_metrics::Timeline::Now(),
                                   submission_current_);
]=] "kick")
  fm4_pass_swap([=[bool D3D12CommandProcessor::WriteShaderCompletion(uint32_t address, uint32_t value, bool visibility_data) {
]=] [=[bool D3D12CommandProcessor::WriteShaderCompletion(uint32_t address, uint32_t value, bool visibility_data) {
  if (fm4::frame_metrics::Line().begin)
    fm4::frame_metrics::Line().Add('C', fm4::frame_metrics::Timeline::Now(), fm4::frame_metrics::Timeline::Now(),
                                   (uint64_t(address) << 32) | value, submission_current_);
]=] "completion")
  file(WRITE "${fm4_file}" "${fm4_text}")
endfunction()
fm4_pass_timing()
