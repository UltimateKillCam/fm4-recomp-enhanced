// Real-scene FSR API dispatch validation, 2026-10-16. GPL-3.0-only.
// This is an offline GPU replay, not the in-game temporal integration.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINDOWS
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#if !defined(FM4_REPLAY_DLSS) && !defined(FM4_REPLAY_NR)
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
#ifdef FM4_GRAPHICS_AGILITY_VERSION
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion=FM4_GRAPHICS_AGILITY_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath=".\\D3D12\\";
}
#endif
static void Check(HRESULT result,const char* operation="D3D12 call") {
  if (FAILED(result)) {
    char code[16]; std::snprintf(code,sizeof(code),"0x%08X",unsigned(result));
    throw std::runtime_error(std::string(operation)+" failed: "+code);
  }
}
#if !defined(FM4_REPLAY_DLSS) && !defined(FM4_REPLAY_NR)
static void Api(ffxReturnCode_t result) {
  if (result != FFX_API_RETURN_OK) throw std::runtime_error("FSR API error " + std::to_string(result));
}
#endif
static std::vector<char> Read(const fs::path& path, size_t expected) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() != std::streamoff(expected)) throw std::runtime_error("Invalid replay resource: " + path.string());
  std::vector<char> bytes(expected); file.seekg(0); file.read(bytes.data(), expected);
  if (!file) throw std::runtime_error("Truncated replay resource"); return bytes;
}
#ifdef FM4_REPLAY_DLSS
#include "dlss_replay.h"
#endif
#ifdef FM4_REPLAY_NR
#include "../neural/neural_rendering.h"
#endif
static D3D12_RESOURCE_DESC TextureDesc(uint32_t width, uint32_t height, DXGI_FORMAT format) {
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1;
  desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; return desc;
}
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
  desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> resource;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&resource))); return resource;
}
static ComPtr<ID3D12Resource> Texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                                    D3D12_RESOURCE_STATES initial) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ComPtr<ID3D12Resource> resource;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&resource)));
  return resource;
}
static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after; list->ResourceBarrier(1, &barrier);
}
static ComPtr<ID3D12Resource> Upload(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                                   const D3D12_RESOURCE_DESC& desc, const std::vector<char>& data,
                                   uint32_t pixel_bytes, std::vector<ComPtr<ID3D12Resource>>& staging,
                                   ID3D12Resource* existing = nullptr) {
  ComPtr<ID3D12Resource> texture = existing;
  if (texture) Transition(list, texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COPY_DEST);
  else texture = Texture(device, desc, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint64_t size;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
  auto upload = Buffer(device, size, D3D12_HEAP_TYPE_UPLOAD);
  void* mapping; D3D12_RANGE empty{0, 0}; Check(upload->Map(0, &empty, &mapping));
  for (uint32_t y = 0; y < desc.Height; ++y)
    std::memcpy(static_cast<char*>(mapping) + footprint.Offset + uint64_t(y) * footprint.Footprint.RowPitch,
                data.data() + uint64_t(y) * desc.Width * pixel_bytes, size_t(desc.Width) * pixel_bytes);
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint;
  dst.pResource = texture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Transition(list, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  staging.push_back(std::move(upload)); return texture;
}
static float Half(uint16_t value) {
  float sign = value & 0x8000 ? -1.0f : 1.0f;
  unsigned exponent = (value >> 10) & 31, mantissa = value & 1023;
  if (exponent == 31) return mantissa ? NAN : sign * INFINITY;
  return sign * (exponent ? std::ldexp(1.0f + mantissa / 1024.0f, int(exponent) - 15)
                         : std::ldexp(float(mantissa), -24));
}
int wmain(int argc, wchar_t** argv) try {
  if (argc < 6) {
    std::puts("fsr-scene-replay <signed-loader-bin> <output-width> <output-height> <output-prefix> <frame> [next-frame ...]"); return 2;
  }
  const uint32_t out_width = std::stoul(argv[2]), out_height = std::stoul(argv[3]);
  if (out_width < 16 || out_height < 16 || out_width > 7680 || out_height > 4320) throw std::runtime_error("Invalid output size");
  const fs::path output_prefix(argv[4]);
  ComPtr<ID3D12DeviceFactory> device_factory;
#ifdef FM4_GRAPHICS_AGILITY_VERSION
  ComPtr<ID3D12SDKConfiguration1> sdk;
  Check(D3D12GetInterface(CLSID_D3D12SDKConfiguration,IID_PPV_ARGS(&sdk)),"Agility configuration interface");
  Check(sdk->CreateDeviceFactory(FM4_GRAPHICS_AGILITY_VERSION,".\\D3D12\\",IID_PPV_ARGS(&device_factory)),"Agility device factory");
  Check(device_factory->ApplyToGlobalState(),"Agility global configuration");
#endif
  const bool debug=std::getenv("FM4_REPLAY_DEBUG")!=nullptr;
  if(debug) {
    ComPtr<ID3D12Debug> layer;
    Check(device_factory?device_factory->GetConfigurationInterface(CLSID_D3D12Debug,IID_PPV_ARGS(&layer)):
                         D3D12GetDebugInterface(IID_PPV_ARGS(&layer)),"Graphics debug layer");
    layer->EnableDebugLayer();
    if(std::getenv("FM4_REPLAY_GPU_VALIDATION")) {
      ComPtr<ID3D12Debug1> validation; Check(layer.As(&validation));
      validation->SetEnableGPUBasedValidation(TRUE);
    }
  }
  if(std::getenv("FM4_REPLAY_DRED")) {
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    Check(device_factory?device_factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData,IID_PPV_ARGS(&settings)):
                         D3D12GetDebugInterface(IID_PPV_ARGS(&settings)));
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  }
  // These COM holders precede Streamline so its shutdown runs while the
  // graphics device/factory are still alive, including error paths.
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
#ifdef FM4_REPLAY_DLSS
  DlssReplay dlss(argv[1]);
  const char* backend_name = "DLSS";
#elif defined(FM4_REPLAY_NR)
  fm4::NeuralRendering neural;
  std::vector<std::unique_ptr<fm4::NeuralRendering>> extra_neural;
  std::vector<fm4::NeuralRendering*> neural_passes{&neural};
  uint32_t neural_count=1;
  if(const char* count=std::getenv("FM4_NEURAL_PASSES")) neural_count=std::stoul(count);
  if(neural_count<1||neural_count>3) throw std::runtime_error("Neural replay passes must be 1..3");
  const auto neural_path=fm4::NeuralPathFromEnvironment();
  const char* backend_name = neural_path==fm4::NeuralPath::Official?"DLSS 5 Neural Rendering (official path requested)":"DLSS 5 Neural Rendering (community Ada compatibility)";
#else
  const char* backend_name = "FSR";
  auto loader = fs::absolute(argv[1]) / L"amd_fidelityfx_loader_dx12.dll";
  HMODULE module = LoadLibraryExW(loader.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) throw std::runtime_error("Signed FSR loader unavailable");
  ffxFunctions api{}; ffxLoadFunctions(&api, module);
  if (!api.CreateContext || !api.Dispatch || !api.Query || !api.DestroyContext) throw std::runtime_error("Incomplete FSR API");
#endif
  Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
    if (desc.VendorId == 0x10de && std::wstring(desc.Description).find(L"RTX 4080") != std::wstring::npos) break;
    adapter.Reset();
  }
  if (!adapter) throw std::runtime_error("RTX 4080 unavailable for this diagnostic");
  Check(device_factory?device_factory->CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)):
                       D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12InfoQueue> messages;
  if(debug) Check(device.As(&messages));
#ifdef FM4_REPLAY_DLSS
  dlss.Device(device.Get());
#elif defined(FM4_REPLAY_NR)
  neural.Initialize(device.Get(),argv[1],fs::absolute(argv[0]).parent_path(),neural_path);
  neural.Configure(0,0,1);
  for(uint32_t i=1;i<neural_count;++i) {
    auto next=std::make_unique<fm4::NeuralRendering>(); next->ShareSession(neural); next->Configure(0,0,1);
    neural_passes.push_back(next.get()); extra_neural.push_back(std::move(next));
  }
#endif
  D3D12_COMMAND_QUEUE_DESC queue_desc{}; ComPtr<ID3D12CommandQueue> queue;
  Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list; Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  Check(list->Close());
  ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) throw std::runtime_error("GPU wait event failed");
  uint64_t serial = 0;
#if !defined(FM4_REPLAY_DLSS) && !defined(FM4_REPLAY_NR)
  ffxContext context{};
#endif
  uint32_t render_width = 0, render_height = 0;
  uint32_t iterations=1;
  if(const char* count=std::getenv("FM4_REPLAY_ITERATIONS")) iterations=std::stoul(count);
  if(iterations<1||iterations>600) throw std::runtime_error("Replay iterations must be 1..600");
  const int total_frames=(argc-5)*int(iterations);
  uint32_t reset_interval=0;
  if(const char* interval=std::getenv("FM4_REPLAY_RESET_INTERVAL")) reset_interval=std::stoul(interval);
  if(reset_interval>600) throw std::runtime_error("Replay reset interval must be 0..600");
#ifdef FM4_REPLAY_NR
  // Match a live renderer: stable resource identities, updated only after the
  // previous submission has completed. Reallocating the guides each frame can
  // exercise undocumented runtime descriptor caching rather than scene history.
  ComPtr<ID3D12Resource> nr_colour,nr_depth,nr_motion,nr_output;
  std::vector<ComPtr<ID3D12Resource>> nr_intermediates;
#endif
  for (int frame = 5; frame < 5+total_frames; ++frame) {
    const fs::path folder(argv[5+(frame-5)%(argc-5)]); uint32_t width, height; float near_plane, far_plane, fov;
    std::ifstream info(folder / "frame.txt");
    if (!(info >> width >> height >> near_plane >> far_plane >> fov) || width < 16 || height < 16 ||
        width > out_width || height > out_height || !std::isfinite(near_plane) || !std::isfinite(far_plane) ||
        near_plane <= 0 || far_plane <= near_plane || !(fov > 0 && fov < 3.14f)) throw std::runtime_error("Invalid captured camera");
    if (!render_width) {
      render_width = width; render_height = height;
#ifdef FM4_REPLAY_NR
      if(out_width!=width||out_height!=height) throw std::runtime_error("Neural Rendering is a same-resolution pass; replay output must match input");
      for(auto* pass:neural_passes) pass->CreateFeature(width,height);
      std::printf("Actual Neural Rendering feature 18 created on the RTX 4080: %zu pass(es), one shared session; path: %s\n",neural_passes.size(),neural.Describe().c_str());
#elif !defined(FM4_REPLAY_DLSS)
      ffxCreateBackendDX12Desc backend{}; backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device = device.Get();
      ffxCreateContextDescUpscale create{}; create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
      create.header.pNext = &backend.header;
      create.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
      create.maxRenderSize = {width, height}; create.maxUpscaleSize = {out_width, out_height};
      Api(api.CreateContext(&context, &create.header, nullptr));
      ffxQueryGetProviderVersion provider{}; provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
      Api(api.Query(&context, &provider.header)); std::printf("Selected signed provider: %s\n", provider.versionName);
#endif
    }
    if (width != render_width || height != render_height) throw std::runtime_error("Replay render dimensions changed");
    Check(allocator->Reset()); Check(list->Reset(allocator.Get(), nullptr));
    std::vector<ComPtr<ID3D12Resource>> staging;
#ifdef FM4_REPLAY_DLSS
    auto colour = Upload(device.Get(), list.Get(), TextureDesc(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT), Read(folder / "colour.rgba32f", size_t(width) * height * 16), 16, staging);
#elif defined(FM4_REPLAY_NR)
    auto colour=Upload(device.Get(),list.Get(),TextureDesc(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT),
        Read(folder/"colour.rgba16f",size_t(width)*height*8),8,staging,nr_colour.Get());
    nr_colour=colour;
#else
    auto colour = Upload(device.Get(), list.Get(), TextureDesc(width, height, DXGI_FORMAT_R8G8B8A8_UNORM), Read(folder / "colour.rgba8", size_t(width) * height * 4), 4, staging);
#endif
#ifdef FM4_REPLAY_NR
    auto depth = Upload(device.Get(), list.Get(), TextureDesc(width, height, DXGI_FORMAT_R32_FLOAT), Read(folder / "depth.f32", size_t(width) * height * 4), 4, staging,nr_depth.Get());
    nr_depth=depth;
#else
    auto depth = Upload(device.Get(), list.Get(), TextureDesc(width, height, DXGI_FORMAT_R32_FLOAT), Read(folder / "depth.f32", size_t(width) * height * 4), 4, staging);
#endif
#ifdef FM4_REPLAY_NR
    auto motion = Upload(device.Get(), list.Get(), TextureDesc(width, height, DXGI_FORMAT_R16G16_FLOAT), Read(folder / "motion.rg16f", size_t(width) * height * 4), 4, staging,nr_motion.Get());
    nr_motion=motion;
#else
    auto motion = Upload(device.Get(), list.Get(), TextureDesc(width, height, DXGI_FORMAT_R32G32_FLOAT), Read(folder / "motion.rg32f", size_t(width) * height * 8), 8, staging);
#endif
#if !defined(FM4_REPLAY_DLSS) && !defined(FM4_REPLAY_NR)
    auto reactive = Upload(device.Get(), list.Get(), TextureDesc(width, height, DXGI_FORMAT_R8_UNORM), Read(folder / "reactive.r8", size_t(width) * height), 1, staging);
#endif
    auto desc = TextureDesc(out_width, out_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
#ifdef FM4_REPLAY_NR
    auto output=nr_output;
    if(output) Transition(list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    else { output=Texture(device.Get(),desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); nr_output=output; }
#else
    auto output = Texture(device.Get(), desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#endif
#ifdef FM4_REPLAY_DLSS
    dlss.Dispatch(list.Get(), colour.Get(), depth.Get(), motion.Get(), output.Get(), folder,
                  width, height, out_width, out_height, near_plane, far_plane, fov, frame - 5);
#elif defined(FM4_REPLAY_NR)
    auto* nr_input=colour.Get();
    for(size_t i=0;i<neural_passes.size();++i) {
      ComPtr<ID3D12Resource> target=output;
      if(i+1<neural_passes.size()) {
        if(i==nr_intermediates.size()) nr_intermediates.push_back(Texture(device.Get(),desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        else Transition(list.Get(),nr_intermediates[i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        target=nr_intermediates[i];
      }
      const bool reset=frame==5||(reset_interval&&uint32_t(frame-5)%reset_interval==0);
      neural_passes[i]->Dispatch(list.Get(),nr_input,depth.Get(),motion.Get(),target.Get(),reset);
      if(i+1<neural_passes.size()) Transition(list.Get(),target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      nr_input=target.Get();
    }
#else
    ffxDispatchDescUpscale dispatch{}; dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = list.Get(); dispatch.color = ffxApiGetResourceDX12(colour.Get());
    dispatch.depth = ffxApiGetResourceDX12(depth.Get()); dispatch.motionVectors = ffxApiGetResourceDX12(motion.Get());
    dispatch.reactive = ffxApiGetResourceDX12(reactive.Get());
    dispatch.output = ffxApiGetResourceDX12(output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {0, 0}; dispatch.motionVectorScale = {1, 1};
    dispatch.renderSize = {width, height}; dispatch.upscaleSize = {out_width, out_height};
    dispatch.enableSharpening = true; dispatch.sharpness = .2f; dispatch.frameTimeDelta = 1000.0f / 60.0f;
    dispatch.preExposure = 1; dispatch.reset = frame == 5;
    dispatch.cameraNear = near_plane; dispatch.cameraFar = far_plane;
    dispatch.cameraFovAngleVertical = fov; dispatch.viewSpaceToMetersFactor = 1;
    dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    Api(api.Dispatch(&context, &dispatch.header));
#endif
    Transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint64_t readback_size;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
    auto readback = Buffer(device.Get(), readback_size, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); Check(list->Close());
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence.Get(), ++serial)); Check(fence->SetEventOnCompletion(serial, event));
    if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) throw std::runtime_error("FSR replay GPU timeout");
    Check(device->GetDeviceRemovedReason());
    if(messages) {
      unsigned errors=0;
      for(UINT64 i=0;i<messages->GetNumStoredMessages();++i) {
        SIZE_T bytes=0; Check(messages->GetMessage(i,nullptr,&bytes));
        std::vector<char> storage(bytes); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(messages->GetMessage(i,message,&bytes));
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
          if(errors++<8) std::fprintf(stderr,"D3D12 validation %u: %s\n",message->ID,message->pDescription);
        }
      }
      messages->ClearStoredMessages();
      if(errors) throw std::runtime_error("D3D12 validation errors in replay");
    }
    D3D12_RANGE range{0, size_t(readback_size)}; void* mapping; Check(readback->Map(0, &range, &mapping));
    std::vector<uint8_t> rgb(size_t(out_width) * out_height * 3); double sum = 0; uint64_t invalid = 0;
    float raw_min=INFINITY,raw_max=-INFINITY;
    for (uint32_t y = 0; y < out_height; ++y) for (uint32_t x = 0; x < out_width; ++x) for (unsigned channel = 0; channel < 3; ++channel) {
      uint16_t half; std::memcpy(&half, static_cast<const char*>(mapping) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch + x * 8 + channel * 2, 2);
      float value = Half(half); if (!std::isfinite(value)) { ++invalid; value = 0; }
      raw_min=std::min(raw_min,value); raw_max=std::max(raw_max,value);
      value = std::clamp(value, 0.0f, 1.0f);
#ifdef FM4_REPLAY_DLSS
      value = value <= .0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - .055f;
#endif
      sum += value;
      rgb[(size_t(y) * out_width + x) * 3 + channel] = uint8_t(std::lround(value * 255));
    }
    D3D12_RANGE empty{0, 0}; readback->Unmap(0, &empty);
    if (invalid || sum < out_width * out_height * .001) {
      std::fprintf(stderr,"Invalid replay frame %d: %llu nonfinite components, mean %.8f, raw [%.8f,%.8f]\n",frame-5,static_cast<unsigned long long>(invalid),sum/rgb.size(),raw_min,raw_max);
      throw std::runtime_error("Replay output is invalid or empty");
    }
    if(iterations==1||frame<7||frame==4+total_frames) {
    fs::path path = output_prefix.wstring() + L"-" + std::to_wstring(frame - 5) + L".ppm";
    std::ofstream file(path, std::ios::binary); file << "P6\n" << out_width << ' ' << out_height << "\n255\n";
    file.write(reinterpret_cast<const char*>(rgb.data()), rgb.size()); if (!file) throw std::runtime_error("Output write failed");
    }
    if(iterations==1||frame<7||(frame-5)%60==0||frame==4+total_frames)
      std::printf("PASS: actual %s dispatch frame %d, %ux%u -> %ux%u, finite RGB, mean %.6f, raw range [%.6f,%.6f]\n", backend_name, frame - 5, width, height, out_width, out_height, sum / rgb.size(),raw_min,raw_max);
  }
#ifdef FM4_REPLAY_DLSS
  dlss.Shutdown();
#elif defined(FM4_REPLAY_NR)
  for(auto it=extra_neural.rbegin();it!=extra_neural.rend();++it) (*it)->Shutdown();
  neural.Shutdown();
#else
  Api(api.DestroyContext(&context, nullptr)); FreeLibrary(module);
#endif
  CloseHandle(event);
  std::puts("Offline captured-gameplay replay only; object motion, jitter and HUD integration remain unfinished."); return 0;
} catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
