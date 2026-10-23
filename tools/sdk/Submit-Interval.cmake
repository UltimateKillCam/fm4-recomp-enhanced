# Split long draw runs into several host submissions. Modified 2026-10-23. GPL-3.0-only.
# Applied as a build-local edit of the generated Gpu-Completion D3D12 command processor.
# The guest kicks its ring buffer a few times per frame, and each kick becomes one host
# submission, so the host GPU stays idle while a large kick is translated and recorded.
# With d3d12_submit_draw_interval = N, the open submission ends before every Nth draw, so
# the GPU starts earlier work while later draws are still recorded. The order of commands,
# queue and fence values is unchanged; 0 keeps only the guest's own submission points.
# Smaller intervals increase submission overhead.
function(fm4_submit_interval)
  if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
    return()
  endif()
  set(fm4_file "${CMAKE_BINARY_DIR}/fm4-shader-completion/d3d_command_processor.cpp")
  if(NOT EXISTS "${fm4_file}")
    message(FATAL_ERROR "Submit interval: generated D3D12 command processor overlay is missing")
  endif()
  file(READ "${fm4_file}" fm4_text)
  string(REPLACE "\r\n" "\n" fm4_text "${fm4_text}")
  macro(fm4_interval_swap old new what)
    string(FIND "${fm4_text}" "${old}" fm4_at)
    string(FIND "${fm4_text}" "${old}" fm4_last REVERSE)
    if(fm4_at LESS 0 OR NOT fm4_at EQUAL fm4_last)
      message(FATAL_ERROR "Submit interval: ${what} anchor missing or ambiguous")
    endif()
    string(REPLACE "${old}" "${new}" fm4_text "${fm4_text}")
  endmacro()
  fm4_interval_swap([=[REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, true, "GPU/D3D12",
]=] [=[REXCVAR_DEFINE_INT32(d3d12_submit_draw_interval, 256, "GPU/D3D12",
                     "End the host submission before every Nth draw so the GPU starts earlier work "
                     "while later draws are recorded (0 = only at guest ring buffer kicks)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, true, "GPU/D3D12",
]=] "setting")
  fm4_interval_swap([=[                                      bool major_mode_explicit) {
]=] [=[                                      bool major_mode_explicit) {
  // Between draws, like the shader-completion and primary-buffer-end submission points.
  if (const int32_t fm4_interval = REXCVAR_GET(d3d12_submit_draw_interval); fm4_interval > 0) {
    static uint64_t fm4_interval_submission = 0;
    static int32_t fm4_interval_draws = 0;
    if (fm4_interval_submission != submission_current_) {
      fm4_interval_submission = submission_current_;
      fm4_interval_draws = 0;
    }
    if (++fm4_interval_draws > fm4_interval && submission_open_ && CanEndSubmissionImmediately()) {
      EndSubmission(false);
    }
  }
]=] "draw")
  file(WRITE "${fm4_file}" "${fm4_text}")
endfunction()
fm4_submit_interval()
