#pragma once
// Preserve original mip data in textures with a resolution-scaled GPU base.
// Modified 2026-10-17. GPL-3.0-only. No image model or asset rewriting.
#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>
#include <cstring>
#include <cstdint>
#include <utility>

namespace fm4::native_mips {
inline constexpr char shader_source[] = R"(
cbuffer Constants : register(b0) {
  uint scale_x, scale_y, pixel_size_log2, byte_count, source_offset;
};
ByteAddressBuffer original : register(t0);
RWByteAddressBuffer scaled : register(u0);
uint OriginalAddress(uint output_byte) {
  uint group_bytes = pixel_size_log2 == 0 ? 8 : 16;
  uint pixel_bytes = 1u << pixel_size_log2;
  uint group_pixels = group_bytes >> pixel_size_log2;
  uint area = scale_x * scale_y;
  uint native_group = output_byte / (group_bytes * area);
  uint host_group = (output_byte / group_bytes) % area;
  uint host_x = (host_group / scale_y) * group_pixels +
                (output_byte % group_bytes) / pixel_bytes;
  return source_offset + native_group * group_bytes +
         (host_x / scale_x) * pixel_bytes + output_byte % pixel_bytes;
}
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint offset = id.x * 4;
  if (offset >= byte_count) return;
  uint value = 0;
  [unroll] for (uint component = 0; component < 4; ++component) {
    uint address = OriginalAddress(offset + component);
    uint byte_value = (original.Load(address & ~3u) >> ((address & 3u) * 8)) & 255;
    value |= byte_value << (component * 8);
  }
  scaled.Store(offset, value);
}
)";

class Uploader {
  Microsoft::WRL::ComPtr<ID3D12RootSignature> signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
 public:
  bool Initialize(ID3D12Device* device) {
    if (pipeline_) return true;
    struct Library { HMODULE handle; ~Library() { if (handle) FreeLibrary(handle); } };
    Library library{LoadLibraryExW(L"D3DCompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!library.handle) return false;
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library.handle, "D3DCompile"));
    Microsoft::WRL::ComPtr<ID3DBlob> shader, errors, root;
    if (!compile || FAILED(compile(shader_source, sizeof(shader_source) - 1, "fm4-native-mips",
        nullptr, nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors))) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                    {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 5;
    for (unsigned i = 0; i < 2; ++i) {
      parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[i + 1].DescriptorTable = {1, &ranges[i]};
    }
    D3D12_ROOT_SIGNATURE_DESC desc{3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &root, &errors)) ||
        FAILED(device->CreateRootSignature(0, root->GetBufferPointer(), root->GetBufferSize(), IID_PPV_ARGS(&signature_)))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{}; ps.pRootSignature = signature_.Get();
    ps.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    return SUCCEEDED(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline_)));
  }

  // Caller owns resource transitions and makes the current native CPU range
  // resident before this dispatch. Only pages without a scaled GPU resolve
  // are copied; current rendered detail remains in its scaled representation.
  template<class Processor>
  bool Record(Processor& cp, ID3D12Resource* original, ID3D12Resource* scaled,
              uint32_t source_offset, uint64_t target_offset, uint32_t original_bytes,
              uint32_t sx, uint32_t sy, uint32_t pixel_size_log2) {
    auto* device = cp.GetD3D12Provider().GetDevice();
    if (!original || !scaled || !sx || !sy || sx > 7 || sy > 7 || pixel_size_log2 > 4 ||
        !original_bytes || (source_offset & 4095) || (original_bytes & 4095) ||
        uint64_t(original_bytes) * sx * sy > UINT32_MAX ||
        uint64_t(source_offset) + original_bytes > original->GetDesc().Width ||
        target_offset + uint64_t(original_bytes) * sx * sy > scaled->GetDesc().Width ||
        !Initialize(device)) return false;
    std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE> descriptors[2];
    if (!cp.RequestOneUseSingleViewDescriptors(2, descriptors)) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = UINT(original->GetDesc().Width / 4); srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(original, &srv, descriptors[0].first);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Buffer.FirstElement = target_offset / 4;
    uav.Buffer.NumElements = original_bytes * sx * sy / 4; uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(scaled, nullptr, &uav, descriptors[1].first);
    cp.SetExternalPipeline(pipeline_.Get());
    auto& list = cp.GetDeferredCommandList();
    list.D3DSetComputeRootSignature(signature_.Get());
    const uint32_t constants[]{sx, sy, pixel_size_log2, original_bytes * sx * sy, source_offset};
    list.D3DSetComputeRoot32BitConstants(0, 5, constants, 0);
    list.D3DSetComputeRootDescriptorTable(1, descriptors[0].second);
    list.D3DSetComputeRootDescriptorTable(2, descriptors[1].second);
    list.D3DDispatch((constants[3] + 1023) / 1024, 1, 1);
    return true;
  }
};
} // namespace fm4::native_mips
