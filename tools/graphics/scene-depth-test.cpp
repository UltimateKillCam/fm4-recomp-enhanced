// Replay real opt-in game ROV snapshots against independently decoded depth.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <regex>
#include <cstring>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 error " + std::to_string(uint32_t(hr))); }
static std::vector<char> Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Missing replay input: " + path.string());
  auto size = input.tellg(); if (size < 0 || size > (128ll << 20)) throw std::runtime_error("Oversized replay input");
  std::vector<char> result(size_t(size), char{}); input.seekg(0); input.read(result.data(), result.size());
  if (!input) throw std::runtime_error("Truncated replay input"); return result;
}
int wmain(int argc, wchar_t** argv) try {
  if (argc != 3) { std::puts("scene-depth-test <scene_depth.cs.hlsl> <decoded-capture-folder>"); return 2; }
  std::filesystem::path folder(argv[2]);
  ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
    if (desc.VendorId == 0x10de && std::wstring(desc.Description).find(L"RTX 4080") != std::wstring::npos) break;
    adapter.Reset();
  }
  if (!adapter) throw std::runtime_error("RTX 4080 unavailable");
  ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants.Num32BitValues=13;
  for (unsigned i=0;i<2;++i) { params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[i+1].DescriptorTable={1,&ranges[i]}; }
  D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters=3; rsd.pParameters=params;
  ComPtr<ID3DBlob> root_blob, errors, shader;
  Check(D3D12SerializeRootSignature(&rsd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&errors));
  ComPtr<ID3D12RootSignature> signature; Check(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&signature)));
  auto compiled=D3DCompileFromFile(argv[1],nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&errors);
  if(FAILED(compiled)&&errors) std::fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stderr); Check(compiled);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=signature.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline;Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
  ComPtr<ID3D12GraphicsCommandList> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)));Check(list->Close());
  ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
  HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr); if(!event) throw std::runtime_error("Fence event failed");
  uint64_t serial=0,pixels=0; unsigned index=0;
  std::ifstream metadata(folder/"edram-depth.jsonl");if(!metadata)throw std::runtime_error("Missing raw-depth metadata");
  for(std::string line;std::getline(metadata,line);++index) {
    auto number=[&](const char* key)->uint32_t {
      std::smatch value;
      if(!std::regex_search(line,value,std::regex(std::string("\"")+key+"\"\\s*:\\s*([0-9]+)")))throw std::runtime_error("Missing numeric metadata");
      auto parsed=std::stoull(value[1].str());if(parsed>UINT32_MAX)throw std::runtime_error("Metadata overflow");return uint32_t(parsed);
    };
    std::smatch file;
    if(!std::regex_search(line,file,std::regex(R"q("file"\s*:\s*"(edram-depth-[0-9]+\.bin)")q")))throw std::runtime_error("Invalid snapshot filename");
    auto raw=Read(folder/file[1].str());
    auto expected=Read(folder/("scene-depth-bin-"+std::to_string(index)+".f32"));
    std::array<uint32_t,13> c{number("width")*number("scale_x"),number("height")*number("scale_y"),
      number("scale_x"),number("scale_y"),number("source_x"),number("source_y"),number("base_tiles"),number("pitch_tiles"),number("msaa"),number("format")};
    if(c[9]>1||c[8]>2||c[2]<1||c[2]>7||c[3]<1||c[3]>7||c[0]>16384||c[1]>16384||expected.size()!=uint64_t(c[0])*c[1]*4||raw.empty()||raw.size()%4)throw std::runtime_error("Invalid replay dimensions/format");
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=raw.size();bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
    D3D12_RANGE empty{0,0};void* mapped;Check(upload->Map(0,&empty,&mapped));std::memcpy(mapped,raw.data(),raw.size());upload->Unmap(0,nullptr);
    D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=c[0];td.Height=c[1];td.DepthOrArraySize=1;td.MipLevels=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> output;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;uint64_t total;device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&total);
    bd.Width=total;hp.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();auto gpu=heap->GetGPUDescriptorHandleForHeapStart();auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_UINT;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=UINT(raw.size()/4);device->CreateShaderResourceView(upload.Get(),&srv,cpu);
    auto ucpu=cpu;ucpu.ptr+=stride;auto ugpu=gpu;ugpu.ptr+=stride;D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,ucpu);
    Check(allocator->Reset());Check(list->Reset(allocator.Get(),pipeline.Get()));list->SetComputeRootSignature(signature.Get());ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRoot32BitConstants(0,13,c.data(),0);list->SetComputeRootDescriptorTable(1,gpu);list->SetComputeRootDescriptorTable(2,ugpu);list->Dispatch((c[0]+7)/8,(c[1]+7)/8,1);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=output.Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Check(list->Close());ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);Check(queue->Signal(fence.Get(),++serial));Check(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("Depth replay timeout");
    Check(readback->Map(0,nullptr,&mapped));uint64_t mismatches=0;
    for(uint32_t y=0;y<c[1];++y)for(uint32_t x=0;x<c[0];++x){float actual,reference;std::memcpy(&actual,static_cast<char*>(mapped)+footprint.Offset+uint64_t(y)*footprint.Footprint.RowPitch+x*4,4);std::memcpy(&reference,expected.data()+(uint64_t(y)*c[0]+x)*4,4);if(!std::isfinite(actual)||std::abs(actual-reference)>1e-7f)++mismatches;}
    readback->Unmap(0,&empty);if(mismatches)throw std::runtime_error("Depth replay disagrees with independent decoder at "+std::to_string(mismatches)+" pixels");pixels+=uint64_t(c[0])*c[1];std::printf("PASS: real scene bin %u, %ux%u, MSAA enum %u\n",index,c[0],c[1],c[8]);
  }
  CloseHandle(event);if(!index)throw std::runtime_error("No depth bins");std::printf("PASS: %u real ROV buffers, %llu decoded depth pixels on RTX 4080\n",index,static_cast<unsigned long long>(pixels));return 0;
} catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
