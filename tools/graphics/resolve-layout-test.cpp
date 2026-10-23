#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#define XXH_INLINE_ALL
#include "texture_mods.h"
#include "native_texture_upload.h"
#include "../../.local/rexglue-enhanced-sdk/src/graphics/shaders/bytecode/d3d12_5_1/texture_load_32bpb_scaled_cs.h"
#include "../../.local/rexglue-enhanced-sdk/src/graphics/shaders/bytecode/d3d12_5_1/texture_load_64bpb_scaled_cs.h"
#include "../../.local/rexglue-enhanced-sdk/src/graphics/shaders/bytecode/d3d12_5_1/texture_load_128bpb_scaled_cs.h"
#include "../../.local/rexglue-enhanced-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../../.local/rexglue-enhanced-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../../.local/rexglue-enhanced-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_scaled_cs.h"
#include "../../.local/rexglue-enhanced-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_64bpp_scaled_cs.h"
#include "../../.local/rexglue-enhanced-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_128bpp_scaled_cs.h"
namespace original_full_resolve {
#include "../../.local/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_scaled_cs.h"
}
namespace pinned_native_resolve {
#include "../../.local/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_64bpp_cs.h"
#include "../../.local/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_128bpp_cs.h"
#include "../../.local/rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"
}
using Microsoft::WRL::ComPtr;
static void Check(HRESULT result) {
  if (FAILED(result)) throw std::runtime_error("D3D failure " + std::to_string(uint32_t(result)));
}
// Independent forward writer: the real resolve maps a host XY position to a
// guest group origin, computes its tiled address, then adds host group offsets.
static uint32_t Tiled(uint32_t x, uint32_t y, uint32_t pitch_tiles, uint32_t b) {
  uint32_t outer = ((y >> 5) * pitch_tiles + (x >> 5)) << 6;
  uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
  uint32_t oi = (outer | inner) << b;
  uint32_t bank = (y >> 4) & 1;
  uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
  return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 15)
      | (((oi >> 4) & 1) << 5) | (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
}
static uint32_t Scaled(uint32_t x, uint32_t y, uint32_t pitch_tiles,
                       uint32_t b, uint32_t sx, uint32_t sy) {
  uint32_t lx = (b == 0 ? 3 : 4) - b, ly = 0;
  uint32_t hx = x >> lx, hy = y >> ly;
  uint32_t gx = hx / sx, gy = hy / sy;
  uint32_t in_group = ((hx - sx * gx) * sy + hy - sy * gy) << (lx + ly + b);
  in_group += (y & ((1u << ly) - 1)) << (lx + b);
  in_group += (x & ((1u << lx) - 1)) << b;
  return Tiled(gx << lx, gy << ly, pitch_tiles, b) * sx * sy + in_group;
}
static int TestPack(ID3D12Device* device, ID3D12CommandQueue* queue, const char* folder) {
  _putenv_s("FM4_TEXTURE_PACK", folder);
  ComPtr<ID3D12CommandAllocator> allocator;
  Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  Check(list->Close());
  ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  UINT64 serial = 0; unsigned count = 0;
  for (const auto& file : std::filesystem::directory_iterator(folder)) {
    if (file.path().extension() != ".dds") continue;
    const auto id = file.path().stem().string();
    unsigned bc, w, h;
    if (std::sscanf(id.c_str(), "bc%u-%ux%u-", &bc, &w, &h) != 3 || bc < 1 || bc > 3 || !w || !h)
      throw std::runtime_error("Invalid colour pack identity: " + id);
    DXGI_FORMAT format = bc == 1 ? DXGI_FORMAT_BC1_UNORM : bc == 2 ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
    auto texture = fm4::textures::Load(device, id, w, h, format);
    if (!texture) throw std::runtime_error("Rejected texture pack DDS: " + id);
    auto desc = texture->upload->GetDesc(); D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    Check(allocator->Reset()); Check(list->Reset(allocator.Get(), nullptr));
    for (unsigned mip = 0; mip < texture->mip_count; ++mip) {
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = texture->upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = texture->layouts[mip];
      dst.pResource = texture->resource.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = mip;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture->resource.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    for (unsigned mip = 0; mip < texture->mip_count; ++mip) {
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = texture->resource.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = mip;
      dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = texture->layouts[mip];
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Check(list->Close()); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence.Get(), ++serial)); Check(fence->SetEventOnCompletion(serial, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("Pack GPU copy timeout");
    Check(device->GetDeviceRemovedReason());
    void *expected, *actual; Check(texture->upload->Map(0, nullptr, &expected)); Check(readback->Map(0, nullptr, &actual));
    for (const auto& layout : texture->layouts) {
      unsigned rows = (layout.Footprint.Height + 3) / 4;
      unsigned bytes = ((layout.Footprint.Width + 3) / 4) * fm4::textures::BlockBytes(format);
      for (unsigned row = 0; row < rows; ++row) {
        size_t offset = layout.Offset + size_t(row) * layout.Footprint.RowPitch;
        if (std::memcmp(static_cast<uint8_t*>(actual) + offset, static_cast<uint8_t*>(expected) + offset, bytes))
          throw std::runtime_error("Pack GPU roundtrip mismatch: " + id);
      }
    }
    D3D12_RANGE empty{0, 0}; readback->Unmap(0, &empty); texture->upload->Unmap(0, &empty);
    std::printf("PASS: %s, %u GPU mip levels\n", id.c_str(), texture->mip_count); ++count;
  }
  CloseHandle(event);
  if (!count || fm4::textures::allocated_bytes.load()) throw std::runtime_error("Empty pack or unreleased pack allocation");
  std::printf("PASS: %u real texture replacements uploaded and read back exactly\n", count); return 0;
}
int main(int argc, char** argv) try {
  if (argc != 2 && !(argc == 3 && (std::string(argv[1]) == "--pack" ||
                                  std::string(argv[2]) == "--original-full-resolve"))) return 2;
  ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 desc; Check(adapter->GetDesc1(&desc));
    if (desc.VendorId == 0x10DE && std::wstring(desc.Description).find(L"RTX 4080") != std::wstring::npos) {
      std::wprintf(L"GPU test on %ls\n", desc.Description); break;
    }
    adapter.Reset();
  }
  if (!adapter) throw std::runtime_error("RTX 4080 not found");
  ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue;
  Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  if (argc == 3 && std::string(argv[1]) == "--pack") return TestPack(device.Get(), queue.Get(), argv[2]);
  std::wstring path; for (const char* p = argv[1]; *p; ++p) path += wchar_t(*p);
  ComPtr<ID3DBlob> shader, errors;
  HRESULT compiled = D3DCompileFromFile(path.c_str(), nullptr, nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
  if (errors) std::printf("%s\n", static_cast<char*>(errors->GetBufferPointer())); Check(compiled);
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants.Num32BitValues = 5;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 3; rd.pParameters = params;
  ComPtr<ID3DBlob> signature; Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors));
  ComPtr<ID3D12RootSignature> root; Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root.Get(); pd.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline; Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)));
  auto buffer = [&](uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource; Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&resource))); return resource;
  };
  constexpr uint32_t width = 64, height = 64, max_guest = width * height * 16, prefix = 4096;
  auto input = buffer(uint64_t(max_guest) * 49 + prefix, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
  auto output = buffer(max_guest, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  auto readback = buffer(max_guest, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
  void* mapped; D3D12_RANGE empty{0, 0}; Check(input->Map(0, &empty, &mapped));
  ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list; Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))); Check(list->Close());
  ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr); uint64_t serial = 0; uint32_t cases = 0;
  for (uint32_t b = 0; b <= 4; ++b) for (uint32_t sx = 1; sx <= 7; ++sx) for (uint32_t sy = 1; sy <= 7; ++sy) for (uint32_t half = 0; half <= 1; ++half) {
    uint32_t guest_bytes = width * height * (1u << b);
    std::fill_n(static_cast<uint8_t*>(mapped), guest_bytes * sx * sy + prefix, 0xCD);
    std::vector<uint8_t> expected(guest_bytes, 0);
    for (uint32_t y = 0; y < height * sy; ++y) for (uint32_t x = 0; x < width * sx; ++x) {
      uint32_t address = Scaled(x, y, width / 32, b, sx, sy);
      for (uint32_t c = 0; c < (1u << b); ++c) {
        uint8_t value = uint8_t((x * 131 + y * 37 + c * 61 + (x ^ y) * 11) & 255);
        static_cast<uint8_t*>(mapped)[prefix + address + c] = value;
        if (x % sx == (half ? sx / 2 : 0) && y % sy == (half ? sy / 2 : 0)) expected[Tiled(x / sx, y / sy, width / 32, b) + c] = value;
      }
    }
    Check(allocator->Reset()); Check(list->Reset(allocator.Get(), pipeline.Get())); list->SetComputeRootSignature(root.Get());
    uint32_t constants[5]{sx, sy, b, guest_bytes, half};
    list->SetComputeRoot32BitConstants(0, 5, constants, 0);
    list->SetComputeRootShaderResourceView(1, input->GetGPUVirtualAddress() + prefix);
    list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    list->Dispatch((guest_bytes / 4 + 255) / 256, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier); list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, guest_bytes);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); Check(list->Close());
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence.Get(), ++serial)); Check(fence->SetEventOnCompletion(serial, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    void* result; D3D12_RANGE range{0, guest_bytes}; Check(readback->Map(0, &range, &result));
    auto actual = static_cast<uint8_t*>(result); bool ok = std::equal(expected.begin(), expected.end(), actual);
    if (!ok) {
      size_t pos = 0; while (pos < expected.size() && actual[pos] == expected[pos]) ++pos;
      std::printf("Mismatch b=%u scale=%ux%u half=%u byte=%zu actual=%u expected=%u\n", b,sx,sy,half,pos,actual[pos],expected[pos]); return 1;
    }
    readback->Unmap(0, &empty); ++cases;
  }
  // Replicate original native bytes into the same grouped scaled layout.
  // Compare every host subpixel with the independently addressed guest pixel,
  // including component packing and nonzero source/destination offsets.
  ComPtr<ID3DBlob> native_shader;
  Check(D3DCompile(fm4::native_mips::shader_source, sizeof(fm4::native_mips::shader_source) - 1,
        nullptr, nullptr, nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &native_shader, &errors));
  pd.CS = {native_shader->GetBufferPointer(), native_shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> native_upload;
  Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&native_upload)));
  auto native_scaled = buffer(uint64_t(max_guest) * 49 + prefix, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  auto native_readback = buffer(uint64_t(max_guest) * 49, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST, false);
  uint32_t native_cases = 0;
  for (uint32_t b = 0; b <= 4; ++b) for (uint32_t sx = 1; sx <= 7; ++sx) for (uint32_t sy = 1; sy <= 7; ++sy) {
    const uint32_t guest_bytes = width * height * (1u << b), scaled_bytes = guest_bytes * sx * sy;
    std::fill_n(static_cast<uint8_t*>(mapped), guest_bytes + prefix, 0xCD);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
      for (uint32_t c = 0; c < (1u << b); ++c)
        static_cast<uint8_t*>(mapped)[prefix + Tiled(x, y, width / 32, b) + c] =
            uint8_t(x * 73 + y * 31 + c * 41 + (x ^ y) * 3);
    std::vector<uint8_t> expected(scaled_bytes);
    for (uint32_t y = 0; y < height * sy; ++y) for (uint32_t x = 0; x < width * sx; ++x)
      for (uint32_t c = 0; c < (1u << b); ++c)
        expected[Scaled(x, y, width / 32, b, sx, sy) + c] =
            static_cast<uint8_t*>(mapped)[prefix + Tiled(x / sx, y / sy, width / 32, b) + c];
    Check(allocator->Reset()); Check(list->Reset(allocator.Get(), native_upload.Get()));
    list->SetComputeRootSignature(root.Get()); uint32_t nc[5]{sx, sy, b, scaled_bytes, prefix};
    list->SetComputeRoot32BitConstants(0, 5, nc, 0);
    list->SetComputeRootShaderResourceView(1, input->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, native_scaled->GetGPUVirtualAddress() + prefix);
    list->Dispatch((scaled_bytes + 1023) / 1024, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {native_scaled.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(native_readback.Get(), 0, native_scaled.Get(), prefix, scaled_bytes);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier);
    Check(list->Close()); ID3D12CommandList* commands[]{list.Get()}; queue->ExecuteCommandLists(1, commands);
    Check(queue->Signal(fence.Get(), ++serial)); Check(fence->SetEventOnCompletion(serial, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("Native mip upload timeout");
    void* actual; Check(native_readback->Map(0, nullptr, &actual));
    const bool match = !std::memcmp(actual, expected.data(), scaled_bytes); native_readback->Unmap(0, &empty);
    if (!match) throw std::runtime_error("Original mip replication differs from guest texels at " +
        std::to_string(sx) + "x" + std::to_string(sy) + " bpp=" + std::to_string(8u << b));
    ++native_cases;
  }
  std::printf("PASS: %u original mip upload cases preserve all guest components and subpixels\n", native_cases);
  // Cross-check the forward layout against the pinned SDK's actual loader
  // bytecode, so a newer upstream layout cannot silently become the oracle.
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1;
  D3D12_ROOT_PARAMETER lp[3]{}; lp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; lp[0].Constants.Num32BitValues = 10;
  for (int i = 0; i < 2; ++i) { lp[i+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; lp[i+1].DescriptorTable.NumDescriptorRanges = 1; lp[i+1].DescriptorTable.pDescriptorRanges = &ranges[i]; }
  D3D12_ROOT_SIGNATURE_DESC lrd{}; lrd.NumParameters = 3; lrd.pParameters = lp;
  ComPtr<ID3DBlob> lb; Check(D3D12SerializeRootSignature(&lrd, D3D_ROOT_SIGNATURE_VERSION_1, &lb, &errors));
  ComPtr<ID3D12RootSignature> lr; Check(device->CreateRootSignature(0, lb->GetBufferPointer(), lb->GetBufferSize(), IID_PPV_ARGS(&lr)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC lpd{}; lpd.pRootSignature = lr.Get(); lpd.CS = {texture_load_32bpb_scaled_cs, sizeof(texture_load_32bpb_scaled_cs)};
  ComPtr<ID3D12PipelineState> loader; Check(device->CreateComputePipelineState(&lpd, IID_PPV_ARGS(&loader)));
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap; Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  auto linear = buffer(4 * 1024 * 1024, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  auto linear_rb = buffer(4 * 1024 * 1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart(); auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
  auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R32G32B32A32_UINT; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Buffer.FirstElement = prefix / 16; srv.Buffer.NumElements = max_guest * 49 / 16;
  device->CreateShaderResourceView(input.Get(), &srv, cpu); cpu.ptr += stride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = DXGI_FORMAT_R32G32B32A32_UINT; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Buffer.NumElements = 4 * 1024 * 1024 / 16;
  device->CreateUnorderedAccessView(linear.Get(), nullptr, &uav, cpu);
  for (uint32_t b : {2u, 3u, 4u}) {
  lpd.CS = b == 2 ? D3D12_SHADER_BYTECODE{texture_load_32bpb_scaled_cs, sizeof(texture_load_32bpb_scaled_cs)} :
           b == 3 ? D3D12_SHADER_BYTECODE{texture_load_64bpb_scaled_cs, sizeof(texture_load_64bpb_scaled_cs)} :
                    D3D12_SHADER_BYTECODE{texture_load_128bpb_scaled_cs, sizeof(texture_load_128bpb_scaled_cs)};
  loader.Reset(); Check(device->CreateComputePipelineState(&lpd, IID_PPV_ARGS(&loader)));
  for (uint32_t sx = 1; sx <= 7; ++sx) for (uint32_t sy = 1; sy <= 7; ++sy) {
    uint32_t w = width * sx, h = height * sy;
    std::vector<uint8_t> expected(w * h * (1u << b));
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
      for (uint32_t c = 0; c < (1u << b); ++c) {
        const uint8_t value = uint8_t(x * 131 + y * 97 + c * 43);
        expected[(y * w + x) * (1u << b) + c] = value;
        static_cast<uint8_t*>(mapped)[prefix + Scaled(x, y, width / 32, b, sx, sy) + c] = value;
      }
    }
    Check(allocator->Reset()); Check(list->Reset(allocator.Get(), loader.Get())); list->SetComputeRootSignature(lr.Get());
    ID3D12DescriptorHeap* heaps[]{heap.Get()}; list->SetDescriptorHeaps(1, heaps);
    uint32_t lc[10]{1u | (sx << 4) | (sy << 7), 0, width, height, w, h, 1, 0, w * (1u << b), height};
    list->SetComputeRoot32BitConstants(0, 10, lc, 0); list->SetComputeRootDescriptorTable(1, gpu);
    auto target = gpu; target.ptr += stride; list->SetComputeRootDescriptorTable(2, target); list->Dispatch((w + (128u >> b) - 1) / (128u >> b), h / 32, 1);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource = linear.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier); list->CopyBufferRegion(linear_rb.Get(), 0, linear.Get(), 0, expected.size());
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); Check(list->Close());
    ID3D12CommandList* cmds[]{list.Get()}; queue->ExecuteCommandLists(1, cmds); Check(queue->Signal(fence.Get(), ++serial)); Check(fence->SetEventOnCompletion(serial, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("Pinned loader GPU timeout");
    void* actual; Check(linear_rb->Map(0, nullptr, &actual));
    bool match = std::memcmp(expected.data(), actual, expected.size()) == 0; linear_rb->Unmap(0, &empty);
    if (!match) throw std::runtime_error("Forward reference disagrees with pinned SDK loader at " + std::to_string(sx) + "x" + std::to_string(sy) + " bpp=" + std::to_string(8u << b));
  }
  }
  std::printf("PASS: 147 forward-layout cases matched the pinned SDK's 32/64/128bpp scaled texture-loader bytecode\n");
  // Also check the real resolve writer. A port may accidentally mix shader
  // generations even when its texture loader is internally consistent.
  D3D12_ROOT_PARAMETER wp[3]{};
  wp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; wp[0].Constants.Num32BitValues = 5;
  wp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; wp[1].DescriptorTable.NumDescriptorRanges = 1; wp[1].DescriptorTable.pDescriptorRanges = &ranges[0];
  wp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; wp[2].DescriptorTable.NumDescriptorRanges = 1; wp[2].DescriptorTable.pDescriptorRanges = &ranges[1];
  D3D12_ROOT_SIGNATURE_DESC wrd{}; wrd.NumParameters = 3; wrd.pParameters = wp;
  ComPtr<ID3DBlob> wb; Check(D3D12SerializeRootSignature(&wrd, D3D_ROOT_SIGNATURE_VERSION_1, &wb, &errors));
  ComPtr<ID3D12RootSignature> wr; Check(device->CreateRootSignature(0, wb->GetBufferPointer(), wb->GetBufferSize(), IID_PPV_ARGS(&wr)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC wpd{}; wpd.pRootSignature = wr.Get(); wpd.CS = {resolve_fast_32bpp_1x2xmsaa_scaled_cs, sizeof(resolve_fast_32bpp_1x2xmsaa_scaled_cs)};
  ComPtr<ID3D12PipelineState> writer; Check(device->CreateComputePipelineState(&wpd, IID_PPV_ARGS(&writer)));
  constexpr uint32_t wsx = 2, wsy = 2;
  for (uint32_t y = 0; y < 16 * wsy; ++y) for (uint32_t x = 0; x < 80 * wsx; ++x) {
    uint32_t value = 0x7FF00000 + x + y * 80 * wsx;
    std::memcpy(static_cast<uint8_t*>(mapped) + prefix + (x + y * 80 * wsx) * 4, &value, 4);
  }
  Check(allocator->Reset()); Check(list->Reset(allocator.Get(), writer.Get())); list->SetComputeRootSignature(wr.Get());
  ID3D12DescriptorHeap* wh[]{heap.Get()}; list->SetDescriptorHeaps(1, wh);
  uint32_t wc[4]{1, (8u << 5) | (wsx << 16) | (wsy << 19), 6u << 7, 2};
  list->SetComputeRoot32BitConstants(0, 4, wc, 0); list->SetComputeRootDescriptorTable(1, gpu);
  auto wg = gpu; wg.ptr += stride; list->SetComputeRootDescriptorTable(2, wg); list->Dispatch(wsx, wsy * 2, 1);
  D3D12_RESOURCE_BARRIER wbar{}; wbar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  wbar.Transition.pResource = linear.Get(); wbar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  wbar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; wbar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &wbar); list->CopyBufferRegion(linear_rb.Get(), 0, linear.Get(), 0, 65536);
  std::swap(wbar.Transition.StateBefore, wbar.Transition.StateAfter); list->ResourceBarrier(1, &wbar); Check(list->Close());
  ID3D12CommandList* writes[]{list.Get()}; queue->ExecuteCommandLists(1, writes); Check(queue->Signal(fence.Get(), ++serial)); Check(fence->SetEventOnCompletion(serial, event));
  if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("Resolve writer timeout");
  void* written; Check(linear_rb->Map(0, nullptr, &written)); uint32_t old_matches = 0, new_matches = 0;
  for (uint32_t y = 0; y < 16 * wsy; ++y) for (uint32_t x = 0; x < 64 * wsx; ++x) {
    uint32_t expected = 0x7FF00000 + x + y * 80 * wsx, actual_old, actual_new;
    uint32_t hx = x >> 4, hy = y >> 1, gx = hx / wsx, gy = hy / wsy;
    uint32_t modern = Tiled(gx << 4, gy << 1, 2, 2) * wsx * wsy +
        (((hx % wsx) * wsy + hy % wsy) << 7) + ((y & 1) << 6) + ((x & 15) << 2);
    std::memcpy(&actual_old, static_cast<uint8_t*>(written) + Scaled(x, y, 2, 2, wsx, wsy), 4);
    std::memcpy(&actual_new, static_cast<uint8_t*>(written) + modern, 4);
    old_matches += actual_old == expected; new_matches += actual_new == expected;
  }
  linear_rb->Unmap(0, &empty);
  std::printf("WRITER LAYOUT: small-group matches=%u/4096 modern-group matches=%u/4096\n", old_matches, new_matches);
  if (old_matches != 4096) throw std::runtime_error("Pinned resolve writer layout has changed");
  // The full resolve shaders were imported from a different shader generation:
  // unlike the fast writer above, they used 128-byte groups. Exercise their
  // actual bytecode, raw EDRAM reads, typed destination and every scale pair.
  D3D12_COMPUTE_PIPELINE_STATE_DESC fpd{}; fpd.pRootSignature = wr.Get();
  fpd.CS = argc >= 3 && std::string(argv[2]) == "--original-full-resolve"
      ? D3D12_SHADER_BYTECODE{original_full_resolve::resolve_full_32bpp_scaled_cs,
                            sizeof(original_full_resolve::resolve_full_32bpp_scaled_cs)}
      : D3D12_SHADER_BYTECODE{resolve_full_32bpp_scaled_cs, sizeof(resolve_full_32bpp_scaled_cs)};
  ComPtr<ID3D12PipelineState> full_writer;
  Check(device->CreateComputePipelineState(&fpd, IID_PPV_ARGS(&full_writer)));
  D3D12_SHADER_RESOURCE_VIEW_DESC raw_srv{}; raw_srv.Format = DXGI_FORMAT_R32_TYPELESS;
  raw_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  raw_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  raw_srv.Buffer.FirstElement = prefix / 4; raw_srv.Buffer.NumElements = max_guest * 49 / 4;
  raw_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device->CreateShaderResourceView(input.Get(), &raw_srv, heap->GetCPUDescriptorHandleForHeapStart());
  wbar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  wbar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  uint32_t full_cases = 0;
  for (uint32_t msaa = 0; msaa <= 2; ++msaa)
  for (uint32_t sample_select : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
    if ((!msaa && sample_select) || (msaa == 1 && sample_select != 0 && sample_select != 1 && sample_select != 4)) continue;
  for (uint32_t sx = 1; sx <= 7; ++sx) for (uint32_t sy = 1; sy <= 7; ++sy) {
    uint32_t w = 64 * sx, h = 32 * sy, tile_w = 80 * sx, tile_h = 16 * sy;
    uint32_t pitch = msaa == 2 ? 2 : 1;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
      uint32_t value = 0xff000000u | ((x ^ y) & 255) << 16 | (y & 255) << 8 | (x & 255);
      for (uint32_t sample = 0; sample < (1u << msaa); ++sample) {
      // The pinned ROV writer expands host pixel coordinates to adjacent
      // samples. Sample bit 0 is vertical, bit 1 horizontal. This independent
      // producer catches newer resolve shaders that assume a different ABI.
      uint32_t px = (x << (msaa == 2)) | (sample >> 1);
      uint32_t py = (y << (msaa != 0)) | (sample & 1);
      uint32_t address = (((py / tile_h) * pitch + px / tile_w) * tile_w * tile_h +
                          (py % tile_h) * tile_w + px % tile_w) * 4;
      std::memcpy(static_cast<uint8_t*>(mapped) + prefix + address, &value, 4);
      }
    }
    Check(allocator->Reset()); Check(list->Reset(allocator.Get(), full_writer.Get()));
    list->SetComputeRootSignature(wr.Get()); list->SetDescriptorHeaps(1, wh);
    uint32_t fc[4]{pitch | (msaa << 10), (8u << 5) | (sx << 16) | (sy << 19), 6u << 7, 2u | (sample_select << 28)};
    list->SetComputeRoot32BitConstants(0, 4, fc, 0);
    list->SetComputeRootDescriptorTable(1, gpu); list->SetComputeRootDescriptorTable(2, wg);
    list->Dispatch((w + 31) / 32, (h + 7) / 8, 1);
    list->ResourceBarrier(1, &wbar); list->CopyBufferRegion(linear_rb.Get(), 0, linear.Get(), 0, w * h * 4);
    std::swap(wbar.Transition.StateBefore, wbar.Transition.StateAfter); list->ResourceBarrier(1, &wbar);
    std::swap(wbar.Transition.StateBefore, wbar.Transition.StateAfter);
    Check(list->Close()); queue->ExecuteCommandLists(1, writes); Check(queue->Signal(fence.Get(), ++serial));
    Check(fence->SetEventOnCompletion(serial, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("Full resolve timeout");
    void* full_actual; Check(linear_rb->Map(0, nullptr, &full_actual));
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
      uint32_t expected = 0xff000000u | ((x ^ y) & 255) << 16 | (y & 255) << 8 | (x & 255), actual;
      std::memcpy(&actual, static_cast<uint8_t*>(full_actual) + Scaled(x, y, 2, 2, sx, sy), 4);
      if (actual != expected) throw std::runtime_error("Full resolve disagrees with loader layout at scale " +
          std::to_string(sx) + "x" + std::to_string(sy) + " MSAA=" + std::to_string(1u << msaa) +
          " sample=" + std::to_string(sample_select) + " pixel " + std::to_string(x) + "," + std::to_string(y));
    }
    linear_rb->Unmap(0, &empty);
    ++full_cases;
  }
  }
  std::printf("PASS: %u full 32bpp resolve cases (1x/2x/4x MSAA, sample selection/averaging) match the pinned ROV and loader layouts\n", full_cases);
  // Compare numeric conversion with the pinned 1x shader, then repeat the
  // same guest values at higher scales. FM4 also resolves signed two-channel
  // surfaces and HDR colour, which an RGBA8 identity test cannot cover.
  uint32_t numeric_cases = 0;
  for (uint32_t destination_b : {2u, 3u, 4u}) {
  const bool dest64 = destination_b == 3, dest128 = destination_b == 4;
  fpd.CS = dest128 ? D3D12_SHADER_BYTECODE{pinned_native_resolve::resolve_full_128bpp_cs, sizeof(pinned_native_resolve::resolve_full_128bpp_cs)} : dest64 ? D3D12_SHADER_BYTECODE{pinned_native_resolve::resolve_full_64bpp_cs, sizeof(pinned_native_resolve::resolve_full_64bpp_cs)} : D3D12_SHADER_BYTECODE{pinned_native_resolve::resolve_full_32bpp_cs,
            sizeof(pinned_native_resolve::resolve_full_32bpp_cs)};
  ComPtr<ID3D12PipelineState> pinned_numeric;
  Check(device->CreateComputePipelineState(&fpd, IID_PPV_ARGS(&pinned_numeric)));
  fpd.CS = dest128 ? D3D12_SHADER_BYTECODE{resolve_full_128bpp_scaled_cs, sizeof(resolve_full_128bpp_scaled_cs)} : dest64 ? D3D12_SHADER_BYTECODE{resolve_full_64bpp_scaled_cs, sizeof(resolve_full_64bpp_scaled_cs)} :
                    D3D12_SHADER_BYTECODE{resolve_full_32bpp_scaled_cs, sizeof(resolve_full_32bpp_scaled_cs)};
  ComPtr<ID3D12PipelineState> scaled_numeric;
  Check(device->CreateComputePipelineState(&fpd, IID_PPV_ARGS(&scaled_numeric)));
  for (uint32_t msaa : {0u, 1u, 2u})
  for (uint32_t sample_select : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
  if ((!msaa && sample_select) || (msaa == 1 && sample_select != 0 && sample_select != 1 && sample_select != 4)) continue;
  for (uint32_t source_format : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 12u, 14u, 15u})
  for (uint32_t destination_format : (dest128 ? std::initializer_list<uint32_t>{38u} : dest64 ? std::initializer_list<uint32_t>{21u, 26u, 32u, 37u} : std::initializer_list<uint32_t>{6u, 7u, 25u, 31u}))
  for (int bias : {-2, -1, 0, 10})
  for (uint32_t endian : {0u, 1u, 2u})
  for (uint32_t number : {0u, 1u, 2u, 3u})
  for (uint32_t swap : {0u, 1u})
  for (uint32_t gamma : {0u, 1u}) {
    if (gamma && source_format != 1) continue;
    // The broader 1x sweep is complemented by FM4's multisampled colour and
    // two-channel moment surfaces. Give each sample different values so a
    // wrong sample index or averaging weight cannot pass as an identity copy.
    if (msaa && !((source_format == 4 && destination_format == 25 && bias == 10 && number == 3) ||
                  ((source_format == 1 || source_format == 3 || source_format == 12) &&
                   destination_format == 7 && bias == -1 && number == 0) ||
                  (source_format == 7 && destination_format == 6 && bias == 0 && number == 0) ||
                  (source_format == 7 && (dest64 || dest128) && bias == 0 && number == 0))) continue;
    std::vector<std::array<uint32_t, 4>> reference(64 * 32);
    for (uint32_t run = 0; run <= 3; ++run) {
      const uint32_t scale = run ? run : 1, w = 64 * scale, h = 32 * scale;
      for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        for (uint32_t sample = 0; sample < (1u << msaa); ++sample) {
        const uint32_t gx = x / scale, gy = y / scale;
        uint32_t value = 0xff000000u | ((gx ^ gy) * 3) << 16 | (gy * 7) << 8 | gx * 3;
        if (source_format == 2 || source_format == 3 || source_format == 12)
          value = (gx * 13) | ((gy * 29) << 10) | (((gx ^ gy) * 11) << 20) | 0xc0000000u;
        if (source_format == 4 || source_format == 5) value = (gx * 449) | ((gy * 997) << 16);
        if (source_format == 6 || source_format == 7) value = (0x2800u + gx * 49) | ((0x3000u + gy * 73) << 16);
        const bool source64 = source_format == 5 || source_format == 7 || source_format == 15;
        uint32_t upper = source_format == 7 ? (0x3100u + gx * 17) | ((0x3800u + gy * 9) << 16)
                                           : (gy * 291) | ((gx * 693) << 16);
        if (source_format == 14 || source_format == 15) {
          value = std::bit_cast<uint32_t>(0.125f + gx * .01f);
          upper = std::bit_cast<uint32_t>(0.25f + gy * .02f);
        }
        if (msaa) value ^= sample * (source64 ? 0x00010001u : 0x11030705u);
        const uint32_t px = (x << (msaa == 2)) | (sample >> 1);
        const uint32_t py = (y << (msaa != 0)) | (sample & 1);
        const uint32_t pitch = (msaa == 2 ? 2 : 1) * (source64 ? 2 : 1);
        const uint32_t tile_width = (source64 ? 40 : 80) * scale;
        const uint32_t address = (((py / (16 * scale)) * pitch + px / tile_width) *
                                 (80 * scale) * (16 * scale) +
                                 (py % (16 * scale)) * (80 * scale) + (px % tile_width) * (source64 ? 2 : 1)) * 4;
        std::memcpy(static_cast<uint8_t*>(mapped) + prefix + address, &value, 4);
        if (source64) std::memcpy(static_cast<uint8_t*>(mapped) + prefix + address + 4, &upper, 4);
        }
      }
      Check(allocator->Reset()); Check(list->Reset(allocator.Get(), run ? scaled_numeric.Get() : pinned_numeric.Get()));
      list->SetComputeRootSignature(wr.Get()); list->SetDescriptorHeaps(1, wh);
      const bool source64 = source_format == 5 || source_format == 7 || source_format == 15;
      uint32_t nc[5]{((msaa == 2 ? 2u : 1u) * (source64 ? 2 : 1)) | (msaa << 10) |
                     (source_format << 24) | (source64 ? 1u << 28 : 0) | (gamma << 30), (8u << 5) | (scale << 16) | (scale << 19),
                     endian | (destination_format << 7) | (number << 13) |
                     ((uint32_t(bias) & 63) << 16) | (swap << 24), 2u | (sample_select << 28), 0};
      list->SetComputeRoot32BitConstants(0, 5, nc, 0);
      list->SetComputeRootDescriptorTable(1, gpu); list->SetComputeRootDescriptorTable(2, wg);
      list->Dispatch((w + (dest128 ? 15 : 31)) / (dest128 ? 16 : 32), (h + 7) / 8, 1);
      list->ResourceBarrier(1, &wbar); list->CopyBufferRegion(linear_rb.Get(), 0, linear.Get(), 0, w * h * (1u << destination_b));
      std::swap(wbar.Transition.StateBefore, wbar.Transition.StateAfter); list->ResourceBarrier(1, &wbar);
      std::swap(wbar.Transition.StateBefore, wbar.Transition.StateAfter);
      Check(list->Close()); queue->ExecuteCommandLists(1, writes); Check(queue->Signal(fence.Get(), ++serial));
      Check(fence->SetEventOnCompletion(serial, event));
      if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("Numeric resolve timeout");
      void* actual; Check(linear_rb->Map(0, nullptr, &actual));
      for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        std::array<uint32_t, 4> value{}; std::memcpy(value.data(), static_cast<uint8_t*>(actual) + (run ? Scaled(x, y, 2, destination_b, scale, scale) : Tiled(x, y, 2, destination_b)), 1u << destination_b);
        if (!run) reference[y * 64 + x] = value;
        else if (value != reference[(y / scale) * 64 + x / scale]) {
          std::printf("Numeric mismatch src=%u dst=%u bias=%d endian=%u number=%u swap=%u gamma=%u scale=%u msaa=%u select=%u xy=%u,%u actual[0]=%08X pinned[0]=%08X\n",
                      source_format,destination_format,bias,endian,number,swap,gamma,scale,1u<<msaa,sample_select,x,y,value[0],reference[(y / scale) * 64 + x / scale][0]);
          throw std::runtime_error("Scaled resolve numeric conversion differs from pinned 1x");
        }
      }
      linear_rb->Unmap(0, &empty); if (run) ++numeric_cases;
    }
  }
  }
  }
  std::printf("PASS: %u numeric 32/64/128bpp resolves match pinned 1x across colour formats, bias, endian and scales 1..3\n", numeric_cases);
  // Odd scales and 4x MSAA also take the legacy fast path in real races.
  // Keep the independent ROV producer above and exercise both actual writers,
  // rather than assuming the one 2x, single-sample layout check covers them.
  device->CreateShaderResourceView(input.Get(), &srv, heap->GetCPUDescriptorHandleForHeapStart());
  wpd.CS = {resolve_fast_32bpp_4xmsaa_scaled_cs, sizeof(resolve_fast_32bpp_4xmsaa_scaled_cs)};
  ComPtr<ID3D12PipelineState> fast_four;
  Check(device->CreateComputePipelineState(&wpd, IID_PPV_ARGS(&fast_four)));
  uint32_t fast_cases = 0;
  for (uint32_t msaa = 0; msaa <= 2; ++msaa)
  for (uint32_t sample = 0; sample < (1u << msaa); ++sample)
  for (uint32_t sx = 1; sx <= 7; ++sx) for (uint32_t sy = 1; sy <= 7; ++sy) {
    uint32_t w = 64 * sx, h = 32 * sy, tile_w = 80 * sx, tile_h = 16 * sy;
    uint32_t pitch = msaa == 2 ? 2 : 1;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x)
    for (uint32_t s = 0; s < (1u << msaa); ++s) {
      uint32_t value = 0xff000000u | ((x ^ y ^ (s * 53)) & 255) << 16 |
                       (y & 255) << 8 | (x & 255);
      uint32_t px = (x << (msaa == 2)) | (s >> 1);
      uint32_t py = (y << (msaa != 0)) | (s & 1);
      uint32_t address = (((py / tile_h) * pitch + px / tile_w) * tile_w * tile_h +
                          (py % tile_h) * tile_w + px % tile_w) * 4;
      std::memcpy(static_cast<uint8_t*>(mapped) + prefix + address, &value, 4);
    }
    Check(allocator->Reset());
    Check(list->Reset(allocator.Get(), msaa == 2 ? fast_four.Get() : writer.Get()));
    list->SetComputeRootSignature(wr.Get()); list->SetDescriptorHeaps(1, wh);
    uint32_t fc[4]{pitch | (msaa << 10), (8u << 5) | (sx << 16) | (sy << 19),
                   6u << 7, 2u | (sample << 28)};
    list->SetComputeRoot32BitConstants(0, 4, fc, 0);
    list->SetComputeRootDescriptorTable(1, gpu); list->SetComputeRootDescriptorTable(2, wg);
    list->Dispatch(w / 64, h / 8, 1);
    list->ResourceBarrier(1, &wbar);
    list->CopyBufferRegion(linear_rb.Get(), 0, linear.Get(), 0, w * h * 4);
    std::swap(wbar.Transition.StateBefore, wbar.Transition.StateAfter);
    list->ResourceBarrier(1, &wbar);
    std::swap(wbar.Transition.StateBefore, wbar.Transition.StateAfter);
    Check(list->Close()); queue->ExecuteCommandLists(1, writes); Check(queue->Signal(fence.Get(), ++serial));
    Check(fence->SetEventOnCompletion(serial, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("Fast resolve timeout");
    void* actual_data; Check(linear_rb->Map(0, nullptr, &actual_data));
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
      uint32_t expected = 0xff000000u | ((x ^ y ^ (sample * 53)) & 255) << 16 |
                          (y & 255) << 8 | (x & 255), actual;
      std::memcpy(&actual, static_cast<uint8_t*>(actual_data) + Scaled(x, y, 2, 2, sx, sy), 4);
      if (actual != expected) throw std::runtime_error("Fast resolve disagrees with ROV at scale " +
          std::to_string(sx) + "x" + std::to_string(sy) + " MSAA=" + std::to_string(1u << msaa) +
          " sample=" + std::to_string(sample) + " pixel " + std::to_string(x) + "," + std::to_string(y));
    }
    linear_rb->Unmap(0, &empty); ++fast_cases;
  }
  std::printf("PASS: %u fast 32bpp resolve cases cover all scales, MSAA modes and individual samples\n", fast_cases);
  input->Unmap(0, &empty);
  // Validate the DDS replacement contract and staging rows using an actual
  // D3D12 allocation, including rejection and memory-budget release.
  auto fixture = std::filesystem::temp_directory_path() / ("fm4-dds-test-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(fixture);
  _putenv_s("FM4_TEXTURE_PACK", fixture.string().c_str());
  for (DXGI_FORMAT tested_format : {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM}) {
  const uint32_t block_width = fm4::textures::BlockWidth(tested_format);
  const uint32_t block_bytes = fm4::textures::BlockBytes(tested_format);
  std::vector<uint8_t> dds(128);
  auto set = [&](size_t at, uint32_t value) { std::memcpy(dds.data() + at, &value, 4); };
  set(0, 0x20534444); set(4, 124); set(8, 0xA1007); set(12, 128); set(16, 128);
  set(28, 8); set(76, 32); set(80, 4); set(84, 0x31545844); set(108, 0x401008);
  if (tested_format == DXGI_FORMAT_R8G8B8A8_UNORM) {
    set(80, 0x41); set(84, 0); set(88, 32); set(92, 255); set(96, 0xFF00); set(100, 0xFF0000); set(104, 0xFF000000);
  }
  for (uint32_t mip = 0; mip < 8; ++mip) {
    uint32_t blocks = (std::max(128u >> mip, 1u) + block_width - 1) / block_width;
    dds.insert(dds.end(), blocks * blocks * block_bytes, uint8_t(19 + mip * 31));
  }
  auto save = [&] { std::ofstream out(fixture / "test.dds", std::ios::binary); out.write(reinterpret_cast<const char*>(dds.data()), dds.size()); };
  save();
  auto replacement = fm4::textures::Load(device.Get(), "test", 64, 64, tested_format);
  if (!replacement || replacement->resource->GetDesc().Width != 128 || replacement->mip_count != 8)
    throw std::runtime_error("Valid DDS mip chain was rejected");
  auto texture_readback = buffer(replacement->upload->GetDesc().Width, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
  void* staging; Check(replacement->upload->Map(0, nullptr, &staging));
  for (uint32_t mip = 0; mip < 8; ++mip) {
    uint32_t blocks = (std::max(128u >> mip, 1u) + block_width - 1) / block_width;
    for (uint32_t row = 0; row < blocks; ++row) for (uint32_t byte = 0; byte < blocks * block_bytes; ++byte) {
      auto offset = replacement->layouts[mip].Offset + row * replacement->layouts[mip].Footprint.RowPitch + byte;
      if (static_cast<uint8_t*>(staging)[offset] != uint8_t(19 + mip * 31)) throw std::runtime_error("DDS staging row mismatch");
    }
  }
  replacement->upload->Unmap(0, &empty);
  Check(allocator->Reset()); Check(list->Reset(allocator.Get(), nullptr));
  for (uint32_t mip = 0; mip < replacement->mip_count; ++mip) {
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = replacement->upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = replacement->layouts[mip];
    dst.pResource = replacement->resource.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = mip;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  D3D12_RESOURCE_BARRIER texture_barrier{}; texture_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  texture_barrier.Transition.pResource = replacement->resource.Get();
  texture_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  texture_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  texture_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &texture_barrier);
  for (uint32_t mip = 0; mip < replacement->mip_count; ++mip) {
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = replacement->resource.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = mip;
    dst.pResource = texture_readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = replacement->layouts[mip];
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  Check(list->Close()); ID3D12CommandList* texture_lists[]{list.Get()}; queue->ExecuteCommandLists(1, texture_lists);
  Check(queue->Signal(fence.Get(), ++serial)); Check(fence->SetEventOnCompletion(serial, event));
  if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("DDS copy GPU timeout");
  Check(device->GetDeviceRemovedReason());
  void* roundtrip; Check(texture_readback->Map(0, nullptr, &roundtrip));
  for (uint32_t mip = 0; mip < 8; ++mip) {
    uint32_t blocks = (std::max(128u >> mip, 1u) + block_width - 1) / block_width;
    for (uint32_t row = 0; row < blocks; ++row) for (uint32_t byte = 0; byte < blocks * block_bytes; ++byte) {
      auto offset = replacement->layouts[mip].Offset + row * replacement->layouts[mip].Footprint.RowPitch + byte;
      if (static_cast<uint8_t*>(roundtrip)[offset] != uint8_t(19 + mip * 31)) throw std::runtime_error("DDS GPU roundtrip mismatch");
    }
  }
  texture_readback->Unmap(0, &empty); replacement.reset();
  if (fm4::textures::allocated_bytes.load() != 0) throw std::runtime_error("Replacement budget not released");
  if (fm4::textures::Load(device.Get(), "test", 64, 64, DXGI_FORMAT_BC3_UNORM)) throw std::runtime_error("Wrong BC format accepted");
  if (fm4::textures::Load(device.Get(), "test", 64, 32, tested_format)) throw std::runtime_error("Wrong aspect accepted");
  set(28, 7); save();
  if (fm4::textures::Load(device.Get(), "test", 64, 64, tested_format)) throw std::runtime_error("Incomplete mip chain accepted");
  set(28, 8); dds.pop_back(); save();
  if (fm4::textures::Load(device.Get(), "test", 64, 64, tested_format)) throw std::runtime_error("Truncated DDS accepted");
  std::filesystem::remove(fixture / "test.dds");
  }
  std::filesystem::remove(fixture); CloseHandle(event);
  std::printf("PASS: BC1 and RGBA8 DDS GPU mip roundtrip, staging rows, format/aspect/mip/truncation checks and budget release\n");
  std::printf("PASS: %u GPU cases, 8/16/32/64/128-bit, scale 1..7 per axis, offsets and packed writes\n", cases); return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
