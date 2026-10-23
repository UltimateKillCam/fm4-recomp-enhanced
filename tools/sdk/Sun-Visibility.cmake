# Sun visibility from the ROV EDRAM. Modified 2026-10-20. GPL-3.0-only.
# Applied as a build-local edit of the generated Gpu-Completion overlay.
# FM4's only occlusion queries are a sun-disc visibility test for each predicated-tiling band:
# a BEGIN report (sample address ending 0x20), one depth-only sun-disc draw, and an END report
# (address - 0x20). Host occlusion queries cannot count ROV depth tests, so the result was a
# constant and sun glare ignored the roof and scenery. Here the depth/stencil EDRAM range is
# saved at BEGIN, the sun draw inverts the stencil of every sample that passes its depth test,
# and at END a compute pass counts those samples and restores the saved values, so the image
# is unchanged. The next frame's END report publishes that count in guest samples; BEGIN
# contributes nothing. Cvar: d3d12_sun_visibility.
# With host render targets (RTV path, 2026-10-23) the sun draw is wrapped in a real D3D12
# occlusion query instead (hardware depth tests count there); the result, divided by the
# resolution scale, is published the same way. Without this the RTV path used the constant
# fake count and the sun glare was always at full strength.
function(fm4_sun_visibility)
  if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
    return()
  endif()
  set(fm4_root "${CMAKE_BINARY_DIR}/fm4-shader-completion")
  if(NOT EXISTS "${fm4_root}/d3d_command_processor.cpp" OR
     NOT EXISTS "${fm4_root}/core_command_processor.cpp")
    message(FATAL_ERROR "Sun visibility: generated GPU overlay is missing")
  endif()
  macro(fm4_swap old new what)
    string(FIND "${fm4_text}" "${old}" fm4_at)
    string(FIND "${fm4_text}" "${old}" fm4_last REVERSE)
    if(fm4_at LESS 0 OR NOT fm4_at EQUAL fm4_last)
      message(FATAL_ERROR "Sun visibility: ${what} anchor missing or ambiguous")
    endif()
    string(REPLACE "${old}" "${new}" fm4_text "${fm4_text}")
  endmacro()

  # 1. Render target cache: three small public helpers (no layout change).
  set(fm4_header_out "${fm4_root}/include/rex/graphics/d3d12/render_target_cache.h")
  file(READ "${CMAKE_SOURCE_DIR}/include/rex/graphics/d3d12/render_target_cache.h" fm4_text)
  string(REPLACE "\r\n" "\n" fm4_text "${fm4_text}")
  fm4_swap([=[  void WriteEdramRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
]=] [=[  void WriteEdramRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  // FM4 sun visibility: external compute access to the ROV EDRAM buffer.
  ID3D12Resource* Fm4EdramBuffer() const { return edram_buffer_; }
  void Fm4PrepareEdramForCompute() {
    TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CommitEdramBufferUAVWrites();
  }
  void Fm4MarkEdramComputeAccess() {
    MarkEdramBufferModified(EdramBufferModificationStatus::kAsUAV);
  }
]=] "render target cache public helpers")
  file(MAKE_DIRECTORY "${fm4_root}/include/rex/graphics/d3d12")
  set(fm4_previous "")
  if(EXISTS "${fm4_header_out}")
    file(READ "${fm4_header_out}" fm4_previous)
  endif()
  if(NOT fm4_previous STREQUAL fm4_text)
    file(WRITE "${fm4_header_out}" "${fm4_text}")
  endif()

  # 2. Core command processor: END/BEGIN increment override.
  set(fm4_file "${fm4_root}/core_command_processor.cpp")
  file(READ "${fm4_file}" fm4_text)
  string(FIND "${fm4_text}" "zpd_increment_override" fm4_done)
  if(fm4_done LESS 0)
    fm4_swap([=[      const auto completed = fm4::visibility::Next(running_samples, uint32_t(fake_sample_count));
]=] [=[      // FM4 sun visibility may supply this report's measured increment.
      const uint32_t fm4_increment = fm4::zpd_increment_override >= 0
                                         ? uint32_t(fm4::zpd_increment_override)
                                         : uint32_t(fake_sample_count);
      fm4::zpd_increment_override = -1;
      const auto completed = fm4::visibility::Next(running_samples, fm4_increment);
]=] "core increment")
    string(PREPEND fm4_text "#include <cstdint>\nnamespace fm4 { extern int64_t zpd_increment_override; }\n")
    file(WRITE "${fm4_file}" "${fm4_text}")
  endif()


  # 3. D3D12 command processor: measurement passes.
  set(fm4_file "${fm4_root}/d3d_command_processor.cpp")
  file(READ "${fm4_file}" fm4_text)
  string(FIND "${fm4_text}" "Fm4SunVisibility" fm4_done)
  if(fm4_done GREATER_EQUAL 0)
    return()
  endif()
  fm4_swap([=[#include <rex/ui/d3d12/d3d12_util.h>
]=] [=[#include <rex/ui/d3d12/d3d12_util.h>

#include <deque>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(d3d12_sun_visibility, true, "GPU/D3D12",
                    "Measure FM4's sun visibility query from the ROV EDRAM instead of a constant");

namespace fm4 {
int64_t zpd_increment_override = -1;
}

namespace {
struct Fm4SunVisibility {
  static constexpr uint32_t kSlots = 256;
  struct Pending {
    uint64_t submission;
    uint32_t slot;
    uint32_t end_address;
    uint32_t divisor;
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> copy_pipeline, diff_pipeline;
  Microsoft::WRL::ComPtr<ID3D12Resource> snapshot, result, readback;
  const uint32_t* readback_mapping = nullptr;
  uint32_t snapshot_size = 0;
  std::deque<Pending> pending;
  std::unordered_map<uint32_t, uint32_t> latest;
  uint32_t next_slot = 0, slot = 0, begin_address = 0, draws = 0;
  uint32_t range_start = 0, range_count = 0, divisor = 1, logged = 0;
  uint32_t stat_reports = 0, stat_nonzero = 0, stat_max = 0;
  bool initialized = false, failed = false, begun = false;
  // Host render target path: an occlusion query around the sun draw.
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> query_heap;
  Microsoft::WRL::ComPtr<ID3D12Resource> query_readback;
  const uint64_t* query_mapping = nullptr;
  bool host_initialized = false, host_failed = false, host_pending_draw = false;
};
Fm4SunVisibility fm4_sun;

// copy_main saves the depth/stencil range; diff_main counts the dwords the sun draw changed
// (one per sample that passed its depth test, see IssueDraw) and puts the saved values back.
constexpr char kFm4SunShader[] = R"(
RWByteAddressBuffer edram : register(u0);
RWByteAddressBuffer snap : register(u1);
RWByteAddressBuffer result : register(u2);
cbuffer Constants : register(b0) { uint start_dword; uint count_dwords; uint slot; };
[numthreads(64, 1, 1)] void copy_main(uint id : SV_DispatchThreadID) {
  if (id == 0) result.Store(slot * 4, 0);
  for (uint i = 0; i < 16; ++i) {
    uint d = id * 16 + i;
    if (d < count_dwords) snap.Store(d * 4, edram.Load((start_dword + d) * 4));
  }
}
[numthreads(64, 1, 1)] void diff_main(uint id : SV_DispatchThreadID) {
  uint changed = 0;
  for (uint i = 0; i < 16; ++i) {
    uint d = id * 16 + i;
    if (d >= count_dwords) continue;
    uint before = snap.Load(d * 4);
    if (before != edram.Load((start_dword + d) * 4)) {
      edram.Store((start_dword + d) * 4, before);
      ++changed;
    }
  }
  if (changed) result.InterlockedAdd(slot * 4, changed);
}
)";
}  // namespace
]=] "includes")

  fm4_swap([=[                                      bool major_mode_explicit) {
]=] [=[                                      bool major_mode_explicit) {
  // FM4 sun visibility: the sun-disc draw writes no colour or depth, so make every sample that
  // passes its depth (and stencil) test invert its stencil; the END pass counts and undoes that.
  struct Fm4SunRegisters {
    uint32_t* reg[3] = {};
    uint32_t value[3] = {};
    ~Fm4SunRegisters() {
      for (uint32_t i = 0; i < 3; ++i)
        if (reg[i]) *reg[i] = value[i];
    }
  } fm4_sun_registers;
  struct Fm4SunQuery {
    DeferredCommandList* list = nullptr;
    ~Fm4SunQuery() {
      if (list) list->D3DEndQuery(fm4_sun.query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, fm4_sun.slot);
    }
  } fm4_sun_query;
  if (fm4_sun.host_pending_draw && fm4_sun.query_heap && BeginSubmission(true)) {
    // Only the first draw between BEGIN and END is measured (FM4 issues exactly one).
    fm4_sun.host_pending_draw = false;
    ++fm4_sun.draws;
    deferred_command_list_.D3DBeginQuery(fm4_sun.query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         fm4_sun.slot);
    fm4_sun_query.list = &deferred_command_list_;
  }
  if (fm4_sun.begun) {
    ++fm4_sun.draws;
    uint32_t* values = register_file_->values;
    const uint32_t indices[3] = {XE_GPU_REG_RB_DEPTHCONTROL, XE_GPU_REG_RB_STENCILREFMASK,
                                 XE_GPU_REG_RB_STENCILREFMASK_BF};
    for (uint32_t i = 0; i < 3; ++i) {
      fm4_sun_registers.reg[i] = &values[indices[i]];
      fm4_sun_registers.value[i] = values[indices[i]];
    }
    reg::RB_DEPTHCONTROL depth_control;
    depth_control.value = values[XE_GPU_REG_RB_DEPTHCONTROL];
    if (!depth_control.stencil_enable) {
      depth_control.stencil_enable = 1;
      depth_control.backface_enable = 0;
      depth_control.stencilfunc = xenos::CompareFunction::kAlways;
    }
    depth_control.stencilfail = xenos::StencilOp::kKeep;
    depth_control.stencilzfail = xenos::StencilOp::kKeep;
    depth_control.stencilzpass = xenos::StencilOp::kInvert;
    depth_control.stencilfail_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzfail_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzpass_bf = xenos::StencilOp::kInvert;
    values[XE_GPU_REG_RB_DEPTHCONTROL] = depth_control.value;
    values[XE_GPU_REG_RB_STENCILREFMASK] |= 0x00FF0000;
    values[XE_GPU_REG_RB_STENCILREFMASK_BF] |= 0x00FF0000;
  }
]=] "sun draw stencil count")

  fm4_swap([=[                                                               uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }
]=] [=[                                                               uint32_t packet, uint32_t count) {
  if (REXCVAR_GET(d3d12_sun_visibility) && !fm4_sun.host_failed && render_target_cache_ &&
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
      !REXCVAR_GET(occlusion_query_enable)) {
    auto& sun = fm4_sun;
    ID3D12Device* device = GetD3D12Provider().GetDevice();
    while (!sun.pending.empty() && sun.pending.front().submission <= submission_completed_) {
      const auto& done = sun.pending.front();
      sun.latest[done.end_address] =
          uint32_t((sun.query_mapping[done.slot] + done.divisor / 2) / done.divisor);
      sun.pending.pop_front();
    }
    if (!sun.host_initialized) {
      sun.host_initialized = true;
      sun.host_failed = true;  // cleared on success
      D3D12_QUERY_HEAP_DESC heap_desc = {};
      heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
      heap_desc.Count = sun.kSlots;
      D3D12_RESOURCE_DESC buffer_desc;
      ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sun.kSlots * 8, D3D12_RESOURCE_FLAG_NONE);
      void* mapping = nullptr;
      if (SUCCEEDED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&sun.query_heap))) &&
          SUCCEEDED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                                    D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&sun.query_readback))) &&
          SUCCEEDED(sun.query_readback->Map(0, nullptr, &mapping))) {
        sun.query_mapping = static_cast<const uint64_t*>(mapping);
        sun.host_failed = false;
        REXGPU_INFO("FM4 sun visibility active (host render targets)");
      } else {
        sun.query_heap.Reset();
        REXGPU_WARN("FM4 sun visibility unavailable: occlusion query resources failed");
      }
    }
    const uint32_t address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
    if (!sun.host_failed && (address & 0x20)) {
      // BEGIN: measure the next draw (the sun disc).
      sun.host_pending_draw = sun.pending.size() < sun.kSlots - 1;
      if (sun.host_pending_draw) {
        sun.slot = sun.next_slot++ % sun.kSlots;
        sun.begin_address = address;
        sun.draws = 0;
        sun.divisor = texture_cache_->draw_resolution_scale_x() *
                      texture_cache_->draw_resolution_scale_y();
        fm4::zpd_increment_override = 0;  // BEGIN contributes no visible samples
      }
    } else if (!sun.host_failed && address + 0x20 == sun.begin_address) {
      // END: the query ended with the draw; resolve it and read back without waiting.
      if (sun.draws && BeginSubmission(true)) {
        deferred_command_list_.D3DResolveQueryData(sun.query_heap.Get(),
                                                   D3D12_QUERY_TYPE_OCCLUSION, sun.slot, 1,
                                                   sun.query_readback.Get(), sun.slot * 8);
        sun.pending.push_back({submission_current_, sun.slot, address, sun.divisor});
      }
      sun.host_pending_draw = false;
      sun.begin_address = 0;
      const auto measured = sun.latest.find(address);
      fm4::zpd_increment_override =
          measured != sun.latest.end() ? int64_t(measured->second) : int64_t(-1);
    }
  }
  if (REXCVAR_GET(d3d12_sun_visibility) && !fm4_sun.failed && render_target_cache_ &&
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock &&
      !REXCVAR_GET(occlusion_query_enable)) {
    auto& sun = fm4_sun;
    ID3D12Device* device = GetD3D12Provider().GetDevice();
    // Harvest finished measurements without waiting for the GPU.
    while (!sun.pending.empty() && sun.pending.front().submission <= submission_completed_) {
      const auto& done = sun.pending.front();
      const uint32_t visible = (sun.readback_mapping[done.slot] + done.divisor / 2) / done.divisor;
      sun.latest[done.end_address] = visible;
      ++sun.stat_reports;
      sun.stat_nonzero += visible != 0;
      sun.stat_max = std::max(sun.stat_max, visible);
      if (sun.stat_reports == 3600) {
        if (sun.logged < 4) {
          ++sun.logged;
          REXGPU_INFO("FM4 sun visibility: {} of the last 3600 reports saw the sun, max {} samples",
                      sun.stat_nonzero, sun.stat_max);
        }
        sun.stat_reports = sun.stat_nonzero = sun.stat_max = 0;
      }
      sun.pending.pop_front();
    }
    auto initialize = [&]() -> bool {
      sun.initialized = true;
      sun.failed = true;  // cleared on success
      HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
      using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                         ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                         ID3DBlob**);
      auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile"))
                              : nullptr;
      Microsoft::WRL::ComPtr<ID3DBlob> copy_cs, diff_cs, blob, errors;
      if (!compile ||
          FAILED(compile(kFm4SunShader, sizeof(kFm4SunShader) - 1, "fm4_sun", nullptr, nullptr,
                         "copy_main", "cs_5_0", 0, 0, &copy_cs, &errors)) ||
          FAILED(compile(kFm4SunShader, sizeof(kFm4SunShader) - 1, "fm4_sun", nullptr, nullptr,
                         "diff_main", "cs_5_0", 0, 0, &diff_cs, &errors))) {
        REXGPU_WARN("FM4 sun visibility unavailable: shader compilation failed");
        return false;
      }
      D3D12_DESCRIPTOR_RANGE ranges[3] = {};
      D3D12_ROOT_PARAMETER params[4] = {};
      for (uint32_t i = 0; i < 3; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      }
      params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[3].Constants.Num32BitValues = 3;
      params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC root_desc = {4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                                             &errors)) ||
          FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&sun.root_signature)))) {
        REXGPU_WARN("FM4 sun visibility unavailable: root signature failed");
        return false;
      }
      auto make_pipeline = [&](ID3DBlob* cs,
                               Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = {};
        pipeline_desc.pRootSignature = sun.root_signature.Get();
        pipeline_desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(
            &pipeline_desc, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())));
      };
      if (!make_pipeline(copy_cs.Get(), sun.copy_pipeline) ||
          !make_pipeline(diff_cs.Get(), sun.diff_pipeline)) {
        REXGPU_WARN("FM4 sun visibility unavailable: pipeline creation failed");
        return false;
      }
      sun.snapshot_size = xenos::kEdramSizeBytes * texture_cache_->draw_resolution_scale_x() *
                          texture_cache_->draw_resolution_scale_y();
      D3D12_RESOURCE_DESC buffer_desc;
      ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sun.snapshot_size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                                 D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&sun.snapshot)))) {
        return false;
      }
      ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sun.kSlots * 4,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                                 D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&sun.result)))) {
        return false;
      }
      ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sun.kSlots * 4, D3D12_RESOURCE_FLAG_NONE);
      void* mapping = nullptr;
      if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                                 D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&sun.readback))) ||
          FAILED(sun.readback->Map(0, nullptr, &mapping))) {
        return false;
      }
      sun.readback_mapping = static_cast<const uint32_t*>(mapping);
      sun.failed = false;
      REXGPU_INFO("FM4 sun visibility active");
      return true;
    };
    auto dispatch = [&](ID3D12PipelineState* pipeline) -> bool {
      ID3D12Resource* edram = render_target_cache_->Fm4EdramBuffer();
      ui::d3d12::util::DescriptorCpuGpuHandlePair views[3];
      if (!edram || !RequestOneUseSingleViewDescriptors(3, views)) return false;
      render_target_cache_->Fm4PrepareEdramForCompute();
      render_target_cache_->WriteEdramRawUAVDescriptor(views[0].first);
      ui::d3d12::util::CreateBufferRawUAV(device, views[1].first, sun.snapshot.Get(),
                                          sun.snapshot_size);
      ui::d3d12::util::CreateBufferRawUAV(device, views[2].first, sun.result.Get(),
                                          sun.kSlots * 4);
      SubmitBarriers();
      SetExternalPipeline(pipeline);
      deferred_command_list_.D3DSetComputeRootSignature(sun.root_signature.Get());
      for (uint32_t i = 0; i < 3; ++i)
        deferred_command_list_.D3DSetComputeRootDescriptorTable(i, views[i].second);
      const uint32_t constants[3] = {sun.range_start, sun.range_count, sun.slot};
      deferred_command_list_.D3DSetComputeRoot32BitConstants(3, 3, constants, 0);
      deferred_command_list_.D3DDispatch((sun.range_count + 1023) / 1024, 1, 1);
      PushUAVBarrier(sun.snapshot.Get());
      PushUAVBarrier(sun.result.Get());
      render_target_cache_->Fm4MarkEdramComputeAccess();
      return true;
    };
    const uint32_t address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
    if (address & 0x20) {
      // BEGIN: save the depth/stencil EDRAM range the sun draw will test against.
      sun.begun = false;
      if ((sun.initialized || initialize()) && !sun.failed &&
          sun.pending.size() < sun.kSlots - 1) {
        reg::RB_COLOR_INFO color_info;
        color_info.value = register_file_->values[XE_GPU_REG_RB_COLOR_INFO];
        reg::RB_DEPTH_INFO depth_info;
        depth_info.value = register_file_->values[XE_GPU_REG_RB_DEPTH_INFO];
        const uint32_t color_base = color_info.color_base | (color_info.color_base_bit_11 << 11);
        const uint32_t depth_base = depth_info.depth_base | (depth_info.depth_base_bit_11 << 11);
        const uint32_t end_tile = color_base > depth_base ? color_base : xenos::kEdramTileCount;
        const uint32_t scale = texture_cache_->draw_resolution_scale_x() *
                               texture_cache_->draw_resolution_scale_y();
        const uint32_t tile_dwords =
            xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples * scale;
        sun.range_start = depth_base * tile_dwords;
        sun.range_count = (std::min(end_tile, xenos::kEdramTileCount) - depth_base) * tile_dwords;
        sun.divisor = scale;
        sun.slot = sun.next_slot++ % sun.kSlots;
        if (depth_base < end_tile && dispatch(sun.copy_pipeline.Get())) {
          sun.begun = true;
          sun.begin_address = address;
          sun.draws = 0;
          fm4::zpd_increment_override = 0;  // BEGIN contributes no visible samples
        }
      }
    } else if (sun.begun && address + 0x20 == sun.begin_address) {
      // END: count and undo the stencil inversions, read back without waiting.
      sun.begun = false;
      if (sun.draws && dispatch(sun.diff_pipeline.Get())) {
        PushTransitionBarrier(sun.result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
        SubmitBarriers();
        deferred_command_list_.D3DCopyBufferRegion(sun.readback.Get(), sun.slot * 4,
                                                   sun.result.Get(), sun.slot * 4, 4);
        PushTransitionBarrier(sun.result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sun.pending.push_back({submission_current_, sun.slot, address, sun.divisor});
      }
      const auto measured = sun.latest.find(address);
      fm4::zpd_increment_override =
          measured != sun.latest.end() ? int64_t(measured->second) : int64_t(-1);
    } else {
      sun.begun = false;
    }
  }
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }
]=] "zpd override")
  file(WRITE "${fm4_file}" "${fm4_text}")
  message(STATUS "FM4 sun visibility applied")
endfunction()
fm4_sun_visibility()
