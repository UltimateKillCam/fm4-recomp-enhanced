#pragma once
// Modified FM4 graphics integration, 2026-10-16. GPL-3.0-only.
// Assemble actual ROV scene depth on the GPU before each scene-bin clear.
// Resources belong to the graphics thread; no per-frame CPU depth readback.
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace fm4::scene_depth {
using Microsoft::WRL::ComPtr;
inline ComPtr<ID3D12RootSignature> signature;
inline ComPtr<ID3D12PipelineState> pipeline;
inline ComPtr<ID3D12Resource> texture, readback;
inline D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
inline D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
inline uint64_t readback_size = 0;
inline uint32_t scale_x = 0, scale_y = 0, rows = 0, bins = 0;
inline bool failed = false;

inline bool Enabled() {
  if (!REXCVAR_GET(n1_hotpath_cache)) return std::getenv("FM4_SCENE_DEPTH_SHADER") != nullptr;
  static const bool enabled = std::getenv("FM4_SCENE_DEPTH_SHADER") != nullptr;
  return enabled;
}
inline bool Complete() { return texture && rows == 720; }
inline bool Wants(const rex::graphics::draw_util::ResolveInfo& info) {
  return Enabled() && !failed && rows < 720 && info.IsClearingDepth() &&
      !info.IsCopyingDepth() && info.coordinate_info.width_div_8 == 160 &&
      info.height_div_8 >= 16 && rows + info.height_div_8 * 8 <= 720 &&
      // Exclude FM4's later single-sample UI/post-processing depth clears.
      uint32_t(info.depth_edram_info.msaa_samples) != 0;
}

inline bool Initialize(ID3D12Device* device, uint32_t sx, uint32_t sy) {
  if (failed) return false;
  if (texture) return sx == scale_x && sy == scale_y;
  auto fail = [] { failed = true; REXGPU_ERROR("FM4 scene-depth GPU initialization failed; temporal inputs disabled"); return false; };
  const char* shader_path = std::getenv("FM4_SCENE_DEPTH_SHADER");
  if (!shader_path || sx < 1 || sy < 1 || sx > 7 || sy > 7) return fail();
  // Shader blobs retain their implementation in the compiler module. Keep it
  // loaded until those blobs and the serialized root signature are released.
  struct CompilerLibrary { HMODULE handle; ~CompilerLibrary() { if (handle) FreeLibrary(handle); } };
  CompilerLibrary compiler{LoadLibraryExW(L"D3DCompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
  if (!compiler.handle) return fail();
  auto compile = reinterpret_cast<decltype(&D3DCompileFromFile)>(GetProcAddress(compiler.handle, "D3DCompileFromFile"));
  ComPtr<ID3DBlob> shader, errors, root;
  HRESULT result = compile ? compile(std::filesystem::path(shader_path).c_str(), nullptr,
      D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3,
      0, &shader, &errors) : E_FAIL;
  if (FAILED(result) && errors) REXGPU_ERROR("FM4 scene-depth shader: {}", static_cast<const char*>(errors->GetBufferPointer()));
  if (FAILED(result)) return fail();
  D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                  {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
  D3D12_ROOT_PARAMETER parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = 13;
  for (unsigned i = 0; i < 2; ++i) {
    parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[i + 1].DescriptorTable = {1, &ranges[i]};
  }
  D3D12_ROOT_SIGNATURE_DESC rs{}; rs.NumParameters = 3; rs.pParameters = parameters;
  if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &root, &errors)) ||
      FAILED(device->CreateRootSignature(0, root->GetBufferPointer(), root->GetBufferSize(), IID_PPV_ARGS(&signature)))) return fail();
  D3D12_COMPUTE_PIPELINE_STATE_DESC ps{}; ps.pRootSignature = signature.Get();
  ps.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  if (FAILED(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline)))) return fail();
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 1280 * sx; desc.Height = 720 * sy; desc.DepthOrArraySize = 1;
  desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R32_FLOAT; desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture)))) return fail();
  scale_x = sx; scale_y = sy; state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  REXGPU_INFO("FM4 scene-depth GPU texture ready: {}x{}, real ROV depth", desc.Width, desc.Height);
  return true;
}

template<class Processor>
inline void Record(Processor& cp, ID3D12Resource* edram,
                   const rex::graphics::draw_util::ResolveInfo& info) {
  if (!Wants(info)) return;
  auto* device = cp.GetD3D12Provider().GetDevice();
  uint32_t sx = info.coordinate_info.draw_resolution_scale_x;
  uint32_t sy = info.coordinate_info.draw_resolution_scale_y;
  if (!Initialize(device, sx, sy)) return;
  rex::ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[2];
  if (!cp.RequestOneUseSingleViewDescriptors(2, descriptors)) { failed = true; return; }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R32_UINT; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Buffer.NumElements = UINT(edram->GetDesc().Width / 4);
  device->CreateShaderResourceView(edram, &srv, descriptors[0].first);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, descriptors[1].first);
  cp.PushTransitionBarrier(texture.Get(), state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  cp.PushUAVBarrier(texture.Get());
  cp.SubmitBarriers();
  const auto& depth = info.depth_edram_info;
  std::array<uint32_t, 13> constants{1280 * sx, uint32_t(info.height_div_8) * 8 * sy, sx, sy,
      uint32_t(info.coordinate_info.edram_offset_x_div_8) * 8, uint32_t(info.coordinate_info.edram_offset_y_div_8) * 8,
      uint32_t(depth.base_tiles), uint32_t(depth.pitch_tiles), uint32_t(depth.msaa_samples), uint32_t(depth.format),
      rows * sy, 720 * sy, 1};
  auto& list = cp.GetDeferredCommandList();
  list.D3DSetComputeRootSignature(signature.Get());
  list.D3DSetComputeRoot32BitConstants(0, UINT(constants.size()), constants.data(), 0);
  list.D3DSetComputeRootDescriptorTable(1, descriptors[0].second);
  list.D3DSetComputeRootDescriptorTable(2, descriptors[1].second);
  cp.SetExternalPipeline(pipeline.Get());
  list.D3DDispatch((constants[0] + 7) / 8, (constants[1] + 7) / 8, 1);
  rows += info.height_div_8 * 8; ++bins;
}

template<class Processor>
inline void FinishFrame(Processor& cp, bool capture) {
  if (!Complete()) return;
  cp.PushTransitionBarrier(texture.Get(), state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (!capture) return;
  auto* device = cp.GetD3D12Provider().GetDevice();
  auto desc = texture->GetDesc();
  if (!readback) {
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = readback_size; buffer.Height = 1; buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return;
  }
  cp.PushTransitionBarrier(texture.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  state = D3D12_RESOURCE_STATE_COPY_SOURCE; cp.SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
  source.pResource = texture.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.pResource = readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = footprint;
  cp.GetDeferredCommandList().D3DCopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

inline void ExportAfterWait(const std::filesystem::path& folder) {
  if (!readback || !Complete()) return;
  void* mapped = nullptr; D3D12_RANGE range{0, size_t(readback_size)};
  if (FAILED(readback->Map(0, &range, &mapped))) return;
  std::ofstream output(folder / "scene-depth-gpu.f32", std::ios::binary);
  for (uint32_t y = 0; y < 720 * scale_y; ++y) {
    output.write(static_cast<const char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                 size_t(1280 * scale_x) * sizeof(float));
  }
  D3D12_RANGE empty{0, 0}; readback->Unmap(0, &empty);
  std::ofstream meta(folder / "scene-depth-gpu.json");
  meta << "{\"width\":" << 1280 * scale_x << ",\"height\":" << 720 * scale_y
       << ",\"bins\":" << bins << ",\"orientation\":\"presented\",\"format\":\"R32_FLOAT\"}\n";
}
inline void NextFrame() { rows = bins = 0; }
inline void Shutdown() {
  texture.Reset(); readback.Reset(); pipeline.Reset(); signature.Reset();
  scale_x = scale_y = rows = bins = 0; readback_size = 0; failed = false;
}
} // namespace fm4::scene_depth
