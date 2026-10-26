# Native range processing, built with the opt-in CPU fast paths.
if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
  return()
endif()
# The range table is valid only for the audited write-side effects.
function(n2_check_writer path begin end expected)
  file(READ "${path}" text)
  string(REPLACE "\r\n" "\n" text "${text}")
  string(FIND "${text}" "${begin}" first)
  string(FIND "${text}" "${end}" last)
  if(first LESS 0 OR last LESS first)
    message(FATAL_ERROR "Native PM4: register writer boundaries changed")
  endif()
  math(EXPR length "${last} - ${first}")
  string(SUBSTRING "${text}" ${first} ${length} writer)
  string(SHA256 actual "${writer}")
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Native PM4: register side effects changed; audit the range table")
  endif()
endfunction()
n2_check_writer("${CMAKE_SOURCE_DIR}/src/graphics/command_processor.cpp"
  "void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {"
  "void CommandProcessor::WriteRegistersFromMem("
  "4b5aa49dce7d278fa372131b439aff132f1008a344b042a4000c1f9a78d5dfa4")
n2_check_writer("${CMAKE_SOURCE_DIR}/src/graphics/d3d12/command_processor.cpp"
  "void D3D12CommandProcessor::DrawStateRegisterWritten("
  "void D3D12CommandProcessor::WriteRegistersFromMem("
  "45149f81a0edbf255c7dbd12f158e4de052ded2e7155e651be0209ba61d6689f")
set(n2_dir "${FM4_CONTROLLER_BRIDGE_DIR}/../graphics")
target_sources(rexgpu-xenos PRIVATE "${n2_dir}/pm4_native.cpp")
set_source_files_properties("${n2_dir}/pm4_native.cpp" PROPERTIES COMPILE_OPTIONS "/arch:AVX2")

macro(n2_replace old new label)
  string(FIND "${n2_text}" "${old}" n2_first)
  string(FIND "${n2_text}" "${old}" n2_last REVERSE)
  if(n2_first LESS 0 OR NOT n2_first EQUAL n2_last)
    message(FATAL_ERROR "Native PM4: ${label} anchor missing or ambiguous")
  endif()
  string(REPLACE "${old}" "${new}" n2_text "${n2_text}")
endmacro()
set(n2_path "${CMAKE_BINARY_DIR}/fm4-shader-completion/d3d_command_processor.cpp")
file(READ "${n2_path}" n2_text)
n2_replace([=[#include "quick_register_update.h"]=]
 [=[#include "quick_register_update.h"
#include "pm4_native.h"
REXCVAR_DEFINE_BOOL(n2_pm4_fast, false, "GPU",
                    "Use native AVX2 constant ranges and register range handlers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);]=] "switch")
n2_replace([=[                                                  uint32_t num_registers) {
  if (!num_registers) {]=]
 [=[                                                  uint32_t num_registers) {
  // Retain the complete original path for diagnostics, side effects and fallback.
  auto legacy = [&](uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (!num_registers) {]=] "legacy range start")
n2_replace([=[  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten()]=]
 [=[  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
  };
  auto* logger = rex::GetLoggerRaw(rex::log::gpu());
  if (!REXCVAR_GET(n2_pm4_fast) || !fm4::pm4::HasAvx2() ||
      (logger && logger->should_log(spdlog::level::debug)) ||
      start_index >= RegisterFile::kRegisterCount ||
      num_registers > RegisterFile::kRegisterCount - start_index) {
    legacy(start_index, base, num_registers);
    return;
  }
  enum class Handler : uint8_t { Shadow, Legacy, DrawState, Constants };
  struct Range { uint32_t last; Handler handler; };
  // Partition at every core write side effect and backend dependency bank.
  // Scratch and LUT writes must dispatch even when the value is unchanged.
  static constexpr Range ranges[] = {
      {XE_GPU_REG_SCRATCH_REG0 - 1, Handler::Shadow},
      {XE_GPU_REG_SCRATCH_REG7, Handler::Legacy},
      {XE_GPU_REG_COHER_STATUS_HOST - 1, Handler::Shadow},
      {XE_GPU_REG_COHER_STATUS_HOST, Handler::Legacy},
      {XE_GPU_REG_DC_LUT_RW_INDEX - 1, Handler::Shadow},
      {XE_GPU_REG_DC_LUT_30_COLOR, Handler::Legacy},
      {0x1FFF, Handler::Shadow},
      {0x2FFF, Handler::DrawState},
      {XE_GPU_REG_SHADER_CONSTANT_000_X - 1, Handler::Shadow},
      {XE_GPU_REG_SHADER_CONSTANT_511_W, Handler::Constants},
      {XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5, Handler::Legacy},
      {XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 - 1, Handler::Shadow},
      {XE_GPU_REG_SHADER_CONSTANT_LOOP_31, Handler::Legacy},
      {uint32_t(RegisterFile::kRegisterCount - 1), Handler::Shadow},
  };
  while (num_registers) {
    const auto* range = std::lower_bound(std::begin(ranges), std::end(ranges), start_index,
        [](const Range& range, uint32_t index) { return range.last < index; });
    const uint32_t count = std::min(num_registers, range->last - start_index + 1);
    auto* destination = register_file_->values + start_index;
    switch (range->handler) {
      case Handler::Shadow:
        fm4::pm4::CopySwapped(destination, base, count);
        break;
      case Handler::DrawState:
        draw_state_dirty_ |= fm4::pm4::CopyState(destination, base, count,
            kDrawStateRegisterDomains.data() + start_index - 0x2000,
            size_t(XE_GPU_REG_VGT_DRAW_INITIATOR) - start_index);
        if (fm4_constant_metrics) fm4::frame_metrics::Get().bulk_state_words += count;
        break;
      case Handler::Constants: {
        const uint64_t* vertex = frame_open_ && cbuffer_binding_float_vertex_.up_to_date
            ? current_float_constant_map_vertex_ : nullptr;
        const uint64_t* pixel = frame_open_ && cbuffer_binding_float_pixel_.up_to_date
            ? current_float_constant_map_pixel_ : nullptr;
        const uint32_t dirty = fm4::pm4::CopyConstants(destination, base, count,
            start_index - XE_GPU_REG_SHADER_CONSTANT_000_X, vertex, pixel);
        if (dirty & 1) cbuffer_binding_float_vertex_.up_to_date = false;
        if (dirty & 2) cbuffer_binding_float_pixel_.up_to_date = false;
        if (fm4_constant_metrics) {
          ++fm4::frame_metrics::Get().constant_calls;
          if (!dirty) ++fm4::frame_metrics::Get().constant_skips;
        }
        break;
      }
      case Handler::Legacy:
        legacy(start_index, base, count);
        break;
    }
    base += count;
    start_index += count;
    num_registers -= count;
  }
}

void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten()]=] "native range dispatch")
file(WRITE "${n2_path}" "${n2_text}")

set(n2_path "${CMAKE_BINARY_DIR}/fm4-shader-completion/core_command_processor.cpp")
file(READ "${n2_path}" n2_text)
n2_replace([=[REXCVAR_DEFINE_BOOL(gpu_bulk_type0_register_writes,]=]
 [=[#include <xmmintrin.h>
REXCVAR_DECLARE(bool, n2_pm4_fast);
REXCVAR_DEFINE_BOOL(gpu_bulk_type0_register_writes,]=] "core declaration")
n2_replace([=[if (bulk && !write_one_reg)]=]
 [=[if ((bulk || REXCVAR_GET(n2_pm4_fast)) && !write_one_reg)]=]
 "type zero routing")
n2_replace([=[  if (range.first_length != 0) {
    uint32_t first_count]=]
 [=[  if (REXCVAR_GET(n2_pm4_fast)) {
    // Prefetch only valid addresses in each contiguous ring segment.
    if (range.first_length) {
      _mm_prefetch(reinterpret_cast<const char*>(range.first), _MM_HINT_T0);
      if (range.first_length > 128)
        _mm_prefetch(reinterpret_cast<const char*>(range.first + 128), _MM_HINT_T0);
    }
    if (range.second_length)
      _mm_prefetch(reinterpret_cast<const char*>(range.second), _MM_HINT_T0);
  }
  if (range.first_length != 0) {
    uint32_t first_count]=] "ring prefetch")
file(WRITE "${n2_path}" "${n2_text}")
message(STATUS "FM4 native PM4 range processing applied (runtime default off)")
