// Optional current DLSS / FidelityFX API integration. GPL-3.0-only, 2026-10-16.
// Vendor headers and signed runtime DLLs are separately installed, never shipped
// in the source archive. All calls use the host's actual graphics device/queue.
#define FM4_TEMPORAL_BRIDGE_EXPORT
#include "temporal_bridge.h"
#include "scene_camera.h"
#include "../graphics/temporal_capture.h"
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <rex/logging.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>
#include <atomic>
#ifdef FM4_HAS_NEURAL
#include "../neural/neural_rendering.h"
#endif
#ifdef FM4_HAS_STREAMLINE
#include <sl.h>
#include <sl_dlss.h>
#endif
#ifdef FM4_HAS_FSR_API
#ifndef PLATFORM_WINDOWS
#define PLATFORM_WINDOWS 1
#endif
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#endif
namespace {
using Microsoft::WRL::ComPtr;
using fm4::camera::Matrix;
using fm4::camera::Camera;
namespace fs=std::filesystem;
enum class Mode { None, Dlss, Fsr, Spatial };
Mode mode=Mode::None;
bool configured=false,failed=false,gpu_initialized=false,history=false,pending=false;
uint32_t frame_index=0,target_width=0,target_height=0,width=0,height=0,output_width=0,output_height=0;
float jitter_x=0,jitter_y=0,previous_jitter_x=0,previous_jitter_y=0;
Camera previous_camera{};
std::chrono::steady_clock::time_point previous_time{};
struct Vote { Camera camera; uint32_t count; };
std::vector<Vote> votes;
std::vector<Matrix> track_vps;
bool neural_requested=false;
uint32_t neural_passes=1,neural_preset=0,neural_style=0;
float neural_strength=1;
#ifdef FM4_HAS_NEURAL
std::vector<std::unique_ptr<fm4::NeuralRendering>> neural_features;
std::vector<ComPtr<ID3D12Resource>> neural_outputs;
#endif
ComPtr<ID3D12Resource> input,prepared,motion,reactive,upscaled,previous_colour;
ComPtr<ID3D12Resource> queued_input,queued_output,queued_depth;
bool queued_valid_depth=false;
ComPtr<ID3D12RootSignature> signature;
ComPtr<ID3D12PipelineState> prepare_pipeline,finish_pipeline,neural_pipeline,neural_normalize_pipeline;
struct Retired { uint64_t submission; ComPtr<ID3D12DescriptorHeap> heap; std::vector<ComPtr<ID3D12Resource>> resources; std::unique_ptr<fm4::temporal_capture::Pair> capture; };
std::deque<Retired> retired;
std::vector<ComPtr<ID3D12Resource>> resized_resources;
// The unofficial NR runtime is validated with a stable list/allocator and a
// completed frame between evaluations, as in the independent replay host.
ComPtr<ID3D12CommandQueue> neural_queue;
ComPtr<ID3D12CommandAllocator> neural_allocator;
ComPtr<ID3D12GraphicsCommandList> neural_list;
ComPtr<ID3D12Fence> neural_fence;
uint64_t neural_fence_serial=0;
HMODULE sl_module=nullptr,fsr_module=nullptr;
#ifdef FM4_HAS_STREAMLINE
PFun_slShutdown* sl_shutdown=nullptr;
PFun_slSetD3DDevice* sl_device=nullptr;
PFun_slUpgradeInterface* sl_upgrade=nullptr;
PFun_slSetTagForFrame* sl_tag=nullptr;
PFun_slSetConstants* sl_constants=nullptr;
PFun_slEvaluateFeature* sl_evaluate=nullptr;
PFun_slGetNewFrameToken* sl_token=nullptr;
PFun_slGetFeatureFunction* sl_function=nullptr;
PFun_slDLSSSetOptions* sl_options=nullptr;
bool sl_initialized=false;
void Sl(sl::Result result) { if(result!=sl::Result::eOk) throw std::runtime_error("Streamline result "+std::to_string(static_cast<int>(result))); }
template<class T> T* Symbol(const char* name) {
  auto fn=reinterpret_cast<T*>(GetProcAddress(sl_module,name));
  if(!fn) throw std::runtime_error(std::string("Missing Streamline entry point: ")+name);
  return fn;
}
sl::float4x4 SlMatrix(const Matrix& m) {
  sl::float4x4 result;
  for(unsigned r=0;r<4;++r) result[r]=sl::float4(float(m[r*4]),float(m[r*4+1]),float(m[r*4+2]),float(m[r*4+3]));
  return result;
}
#endif
#ifdef FM4_HAS_FSR_API
ffxFunctions fsr_api{};
ffxContext fsr_context{};
bool fsr_created=false;
void Fsr(ffxReturnCode_t result) { if(result!=FFX_API_RETURN_OK) throw std::runtime_error("FidelityFX API error "+std::to_string(result)); }
#endif
void Check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("Temporal D3D12 error "+std::to_string(uint32_t(hr))); }
void Error(const std::exception& e) { failed=true; REXGPU_ERROR("FM4 temporal integration disabled: {}",e.what()); }
uint32_t EnvironmentSize(const char* key) {
  const char* text=std::getenv(key); if(!text) return 0;
  char* end=nullptr; auto n=std::strtoul(text,&end,10);
  if(*end||n<16||n>7680) throw std::runtime_error(std::string("Invalid ")+key);
  return uint32_t(n);
}
void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
  if(before==after) return;
  D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
ComPtr<ID3D12Resource> Texture(ID3D12Device* device,uint32_t w,uint32_t h,DXGI_FORMAT format,D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width=w; d.Height=h; d.DepthOrArraySize=1; d.MipLevels=1; d.Format=format;
  d.SampleDesc.Count=1; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> r; Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r))); return r;
}
void Keep(ComPtr<ID3D12Resource>& r) { if(r) resized_resources.push_back(std::move(r)); }
void CreatePipeline(ID3D12Device* device) {
  const char* path=std::getenv("FM4_TEMPORAL_SHADER"); if(!path) throw std::runtime_error("Missing temporal shader path");
  // Blob implementations live in the compiler DLL. Keep the module loaded
  // until every shader/root-signature blob below has been released.
  struct CompilerLibrary { HMODULE handle; ~CompilerLibrary(){if(handle) FreeLibrary(handle);} };
  CompilerLibrary compiler{LoadLibraryExW(L"D3DCompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)};
  if(!compiler.handle) throw std::runtime_error("System shader compiler unavailable");
  auto compile=reinterpret_cast<decltype(&D3DCompileFromFile)>(GetProcAddress(compiler.handle,"D3DCompileFromFile"));
  ComPtr<ID3DBlob> shaders[4],errors,root;
  HRESULT result=E_FAIL;
  const char* entries[]{"prepare","finish","neural_finish","neural_normalize"};
  for(unsigned i=0;i<4;++i) {
    result=compile?compile(fs::path(path).c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,entries[i],"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shaders[i],&errors):E_FAIL;
    if(FAILED(result)) break;
  }
  if(FAILED(result)) throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Temporal shader compilation failed");
  D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,7,0,0,6}};
  D3D12_ROOT_PARAMETER params[2]{}; params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues=28;
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable={2,ranges};
  D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC desc{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
  Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&root,&errors));
  Check(device->CreateRootSignature(0,root->GetBufferPointer(),root->GetBufferSize(),IID_PPV_ARGS(&signature)));
  for(unsigned i=0;i<4;++i) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{}; p.pRootSignature=signature.Get(); p.CS={shaders[i]->GetBufferPointer(),shaders[i]->GetBufferSize()};
    if(i==3) Check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&neural_normalize_pipeline)));
    else if(i==2) Check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&neural_pipeline)));
    else if(i==1) Check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&finish_pipeline)));
    else Check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&prepare_pipeline)));
  }
}
struct Constants {
  float reprojection[16]; uint32_t w,h,ow,oh;
  float jx,jy,pjx,pjy; uint32_t reset,linear,spatial; float neural_strength;
};
static_assert(sizeof(Constants)==28*4);
float Halton(uint32_t index,uint32_t base) { float value=0,f=1; while(index) { f/=base; value+=f*(index%base); index/=base; } return value; }
void SetJitter() {
  const char* setting=std::getenv("FM4_TEMPORAL_JITTER");
  const bool enabled=mode!=Mode::Spatial&&(!setting || std::string(setting)!="0");
  // Presented pixel offsets. The scene composition flips both raw axes.
  jitter_x=enabled?Halton((frame_index%32)+1,2)-.5f:0;
  jitter_y=enabled?Halton((frame_index%32)+1,3)-.5f:0;
}
}
extern "C" bool Fm4TemporalPreInitialize() {
  if(configured) return !failed;
  configured=true;
  try {
    const char* value=std::getenv("FM4_TEMPORAL_MODE");
    if(!value || !*value) return true;
    const std::string name(value);
    mode=name=="DLSS"?Mode::Dlss:name=="FSR"?Mode::Fsr:name=="SPATIAL"?Mode::Spatial:Mode::None;
    if(mode==Mode::None) throw std::runtime_error("Unknown temporal mode");
    target_width=EnvironmentSize("FM4_TEMPORAL_WIDTH"); target_height=EnvironmentSize("FM4_TEMPORAL_HEIGHT");
    if(!target_width||!target_height) throw std::runtime_error("Temporal output dimensions required");
    neural_requested=std::getenv("FM4_NEURAL_ENABLE")!=nullptr;
    if(neural_requested) {
      auto integer=[](const char* key,uint32_t fallback,uint32_t maximum) {
        const char* value=std::getenv(key); if(!value) return fallback;
        char* end=nullptr; auto n=std::strtoul(value,&end,10);
        if(*end||n>maximum) throw std::runtime_error(std::string("Invalid ")+key);
        return uint32_t(n);
      };
      neural_passes=integer("FM4_NEURAL_PASSES",1,3); neural_preset=integer("FM4_NEURAL_PRESET",0,3); neural_style=integer("FM4_NEURAL_STYLE",0,2);
      if(!neural_passes) throw std::runtime_error("Neural passes must be 1..3");
      if(const char* strength=std::getenv("FM4_NEURAL_STRENGTH")) {
        char* end=nullptr; neural_strength=std::strtof(strength,&end);
        if(*end||!std::isfinite(neural_strength)||neural_strength<0||neural_strength>1) throw std::runtime_error("Invalid neural strength");
      }
#ifndef FM4_HAS_NEURAL
      throw std::runtime_error("Build with separately installed NGX SDK headers/libraries for Neural Rendering");
#endif
    }
    SetJitter();
    if(mode==Mode::Dlss) {
#ifdef FM4_HAS_STREAMLINE
      const char* plugins=std::getenv("FM4_STREAMLINE_DIR"); if(!plugins) throw std::runtime_error("Streamline directory required");
      auto folder=fs::absolute(plugins).wstring(); auto dll=fs::path(folder)/L"sl.interposer.dll";
      sl_module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if(!sl_module) throw std::runtime_error("Signed Streamline loader unavailable");
      auto init=Symbol<PFun_slInit>("slInit"); sl_shutdown=Symbol<PFun_slShutdown>("slShutdown");
      sl_device=Symbol<PFun_slSetD3DDevice>("slSetD3DDevice"); sl_upgrade=Symbol<PFun_slUpgradeInterface>("slUpgradeInterface");
      sl_tag=Symbol<PFun_slSetTagForFrame>("slSetTagForFrame"); sl_constants=Symbol<PFun_slSetConstants>("slSetConstants");
      sl_evaluate=Symbol<PFun_slEvaluateFeature>("slEvaluateFeature"); sl_token=Symbol<PFun_slGetNewFrameToken>("slGetNewFrameToken");
      sl_function=Symbol<PFun_slGetFeatureFunction>("slGetFeatureFunction");
      const sl::Feature features[]{sl::kFeatureDLSS}; const wchar_t* paths[]{folder.c_str()};
      sl::Preferences p{}; p.flags=sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eDisableCLStateTracking|
        sl::PreferenceFlags::eDisableDebugText|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
      p.featuresToLoad=features; p.numFeaturesToLoad=1; p.pathsToPlugins=paths; p.numPathsToPlugins=1;
      p.engine=sl::EngineType::eCustom; p.engineVersion="FM4-Enhanced-1"; p.projectId="2249d991-61bc-4a73-a6c0-746df2973b51";
      p.logMessageCallback=[](sl::LogType,const char* message){ REXGPU_INFO("Streamline: {}",message); };
      Sl(init(p,sl::kSDKVersion)); sl_initialized=true;
#else
      throw std::runtime_error("Build SDK with separately installed Streamline headers");
#endif
    } else if(mode==Mode::Fsr) {
#ifdef FM4_HAS_FSR_API
      const char* bin=std::getenv("FM4_FSR_DIR"); if(!bin) throw std::runtime_error("FidelityFX signed-bin directory required");
      auto dll=fs::absolute(bin)/L"amd_fidelityfx_loader_dx12.dll";
      fsr_module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if(!fsr_module) throw std::runtime_error("Signed FidelityFX loader unavailable");
      ffxLoadFunctions(&fsr_api,fsr_module);
      if(!fsr_api.CreateContext||!fsr_api.Dispatch||!fsr_api.DestroyContext) throw std::runtime_error("Incomplete FidelityFX API");
#else
      throw std::runtime_error("Build SDK with separately installed FidelityFX API headers");
#endif
    }
    REXGPU_INFO("FM4 temporal mode {} requested: {}x{} output",name,target_width,target_height);
    return true;
  } catch(const std::exception& e) { Error(e); return false; }
}
extern "C" bool Fm4TemporalDevice(ID3D12Device* device) {
  if(mode==Mode::None||failed||gpu_initialized) return !failed;
  try {
#ifdef FM4_HAS_STREAMLINE
    if(mode==Mode::Dlss) { Sl(sl_device(device)); void* fn=nullptr; Sl(sl_function(sl::kFeatureDLSS,"slDLSSSetOptions",fn)); sl_options=reinterpret_cast<PFun_slDLSSSetOptions*>(fn); }
#endif
    gpu_initialized=true; return true;
  } catch(const std::exception& e) { Error(e); return false; }
}
extern "C" void Fm4TemporalUpgradeSwapChain(void** chain) {
#ifdef FM4_HAS_STREAMLINE
  if(mode==Mode::Dlss&&!failed) try { Sl(sl_upgrade(chain)); REXGPU_INFO("FM4 Streamline swapchain proxy active for Present lifecycle"); }
  catch(const std::exception& e) { Error(e); }
#endif
}
extern "C" bool Fm4TemporalOutputSize(uint32_t w,uint32_t h,uint32_t* ow,uint32_t* oh) {
  if(mode==Mode::None||failed||w>target_width||h>target_height) return false;
  *ow=target_width; *oh=target_height; return true;
}
extern "C" ID3D12Resource* Fm4TemporalInput(ID3D12Device* device,uint32_t w,uint32_t h) {
  if(mode==Mode::None||failed) return nullptr;
  try {
    if(input&&width==w&&height==h) return input.Get();
    Keep(input); Keep(prepared); Keep(motion); Keep(reactive); Keep(previous_colour); Keep(upscaled);
    width=w; height=h; output_width=target_width; output_height=target_height; history=false;
#ifdef FM4_HAS_NEURAL
    if(!neural_outputs.empty()) throw std::runtime_error("Neural input resizing currently requires restarting the game");
#endif
#ifdef FM4_HAS_FSR_API
    if(fsr_created) { Fsr(fsr_api.DestroyContext(&fsr_context,nullptr)); fsr_created=false; }
#endif
    if(!signature) CreatePipeline(device);
    input=Texture(device,w,h,DXGI_FORMAT_R10G10B10A2_UNORM,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prepared=Texture(device,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    motion=Texture(device,w,h,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    reactive=Texture(device,w,h,DXGI_FORMAT_R8_UNORM,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    previous_colour=Texture(device,w,h,DXGI_FORMAT_R10G10B10A2_UNORM,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    upscaled=Texture(device,target_width,target_height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(neural_requested) {
#ifdef FM4_HAS_NEURAL
      const char* folder=std::getenv("FM4_NEURAL_DIR"),*bridge=std::getenv("FM4_NEURAL_BRIDGE_DIR");
      if(!folder||!bridge) throw std::runtime_error("Supply Neural Rendering model and compatibility bridge directories");
      const auto path=fm4::NeuralPathFromEnvironment();
      for(uint32_t i=0;i<neural_passes;++i) {
        auto feature=std::make_unique<fm4::NeuralRendering>();
        if(neural_features.empty()) feature->Initialize(device,folder,bridge,path);
        else feature->ShareSession(*neural_features.front());
        feature->Configure(neural_preset,neural_style,1);
        neural_features.push_back(std::move(feature));
      }
      REXGPU_INFO("FM4 actual DLSS 5 Neural Rendering initialized: {} pass(es), preset={}, style={}, strength={}, render_thread={}",neural_passes,neural_preset,neural_style,neural_strength,GetCurrentThreadId());
#endif
    }
#ifdef FM4_HAS_NEURAL
    for(auto& feature:neural_features) {
      feature->CreateFeature(w,h);
      neural_outputs.push_back(Texture(device,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }
    if(!neural_features.empty()) REXGPU_INFO("FM4 Neural Rendering path: {}",neural_features.front()->Describe());
#endif
#ifdef FM4_HAS_FSR_API
    if(mode==Mode::Fsr) {
      ffxCreateBackendDX12Desc backend{}; backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device=device;
      ffxCreateContextDescUpscale create{}; create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; create.header.pNext=&backend.header;
      create.flags=FFX_UPSCALE_ENABLE_AUTO_EXPOSURE|FFX_UPSCALE_ENABLE_DEPTH_INVERTED|FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
      create.maxRenderSize={w,h}; create.maxUpscaleSize={target_width,target_height};
      Fsr(fsr_api.CreateContext(&fsr_context,&create.header,nullptr)); fsr_created=true;
      ffxQueryGetProviderVersion version{}; version.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
      Fsr(fsr_api.Query(&fsr_context,&version.header)); REXGPU_INFO("FM4 current FidelityFX provider: {}",version.versionName);
    }
#endif
    REXGPU_INFO("FM4 live temporal resources {}x{} -> {}x{}",w,h,target_width,target_height);
    return input.Get();
  } catch(const std::exception& e) { Error(e); return nullptr; }
}
extern "C" bool Fm4TemporalInputSizeChanged(uint32_t w,uint32_t h) { return input&&(width!=w||height!=h); }
extern "C" void Fm4TemporalObserveDraw(const float* constants,const float* track_constants,uint32_t surface,uint32_t depth,const float* viewport) {
  if(mode==Mode::None||failed||!fm4::camera::SceneDraw(surface,depth,viewport)) return;
  Matrix vp{};
  if(track_vps.size()<32 && fm4::camera::TrackVp(track_constants,vp)) {
    bool duplicate=false;
    for(const auto& other:track_vps) {
      double error=0; for(unsigned i=0;i<16;++i) error=std::max(error,std::abs(vp[i]-other[i]));
      if(error<1e-4) { duplicate=true; break; }
    }
    if(!duplicate) track_vps.push_back(vp);
  }
  Camera c{}; if(!fm4::camera::Candidate(constants,c)) return;
  for(auto& v:votes) if(fm4::camera::SameView(v.camera,c)) { ++v.count; return; }
  if(votes.size()<32) votes.push_back({c,1});
}
extern "C" void Fm4TemporalJitter(uint32_t surface,uint32_t depth,const float* viewport,uint32_t w,uint32_t h,float* x,float* y) {
  if(mode==Mode::None||failed||!w||!h||!fm4::camera::SceneDraw(surface,depth,viewport)) return;
  // Raw-scene X becomes -presented X; D3D Y becomes -raw clip Y.
  *x-=2*jitter_x/w; *y+=2*jitter_y/h;
}
extern "C" void Fm4TemporalQueue(ID3D12Resource* in,ID3D12Resource* out,ID3D12Resource* z,bool valid) {
  queued_input=in; queued_output=out; queued_depth=z; queued_valid_depth=valid; pending=true;
}
extern "C" bool Fm4TemporalHasQueuedFrame() { return pending; }
extern "C" bool Fm4TemporalUsesNeuralQueue() { return neural_requested; }
extern "C" void Fm4TemporalExecuteNeuralQueue(ID3D12Device* device,ID3D12CommandQueue* guest_queue,uint64_t completed,uint64_t serial) {
  if(!pending) return;
  try {
    if(!neural_queue) {
      D3D12_COMMAND_QUEUE_DESC q{};
      Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&neural_queue)));
      Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&neural_allocator)));
      Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,neural_allocator.Get(),nullptr,IID_PPV_ARGS(&neural_list)));
      Check(neural_list->Close());
      neural_list->SetName(L"FM4 isolated neural presentation");
      Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&neural_fence)));
    }
    struct Event { HANDLE handle=CreateEventW(nullptr,FALSE,FALSE,nullptr); ~Event(){if(handle) CloseHandle(handle);} } event;
    if(!event.handle) throw std::runtime_error("Neural queue wait event unavailable");
    auto wait=[&](ID3D12CommandQueue* queue) {
      Check(queue->Signal(neural_fence.Get(),++neural_fence_serial));
      Check(neural_fence->SetEventOnCompletion(neural_fence_serial,event.handle));
      if(WaitForSingleObject(event.handle,15000)!=WAIT_OBJECT_0) throw std::runtime_error("Neural queue GPU timeout");
      Check(device->GetDeviceRemovedReason());
    };
    // Complete guest writes before consuming colour/depth. This also protects
    // the compatibility runtime from outstanding callbacks on recycled lists.
    wait(guest_queue);
    Check(neural_allocator->Reset()); Check(neural_list->Reset(neural_allocator.Get(),nullptr));
    Fm4TemporalExecute(device,neural_list.Get(),completed,serial);
    Check(neural_list->Close()); ID3D12CommandList* lists[]{neural_list.Get()};
    neural_queue->ExecuteCommandLists(1,lists); wait(neural_queue.Get());
    if(!retired.empty()&&retired.back().capture) {
      fm4::temporal_capture::Save(*retired.back().capture);
      retired.back().capture.reset();
    }
  } catch(const std::exception& e) { Error(e); Fm4TemporalDeviceLostDiagnostic(device); }
}
extern "C" void Fm4TemporalExecute(ID3D12Device* device,ID3D12GraphicsCommandList* list,uint64_t completed,uint64_t serial) {
  while(!retired.empty()&&retired.front().submission<=completed) {
    if(retired.front().capture) try { fm4::temporal_capture::Save(*retired.front().capture); }
      catch(const std::exception& e) { REXGPU_WARN("FM4 paired capture failed: {}",e.what()); }
    retired.pop_front();
  }
  if(!pending) return;
  pending=false;
  try {
    Camera camera{}; uint32_t count=0;
    for(const auto& v:votes) if(v.count>count) { count=v.count; camera=v.camera; }
    if(count) for(const auto& vp:track_vps) if(fm4::camera::TrackProjection(vp,camera)) break;
    bool valid=queued_valid_depth&&queued_depth&&count>=2;
    bool reset=!history||!valid;
    if(history&&valid) {
      Matrix a{},b{};
      if(!fm4::camera::Inverse(camera.view,a)||!fm4::camera::Inverse(previous_camera.view,b)) reset=true;
      double distance=0; for(unsigned i=12;i<15;++i) distance+=(a[i]-b[i])*(a[i]-b[i]);
      double facing=0; for(unsigned i=8;i<11;++i) facing+=a[i]*b[i];
      if(distance>400 || facing<.5 || std::abs(camera.fov-previous_camera.fov)>.15) reset=true;
    }
    Matrix inverse{},reprojection{};
    if(valid&&!reset&&fm4::camera::Inverse(fm4::camera::Presented(camera.vp),inverse))
      reprojection=fm4::camera::Multiply(inverse,fm4::camera::Presented(previous_camera.vp));
    else for(unsigned i=0;i<4;++i) reprojection[i*5]=1;
    Constants c{}; for(unsigned i=0;i<16;++i) c.reprojection[i]=float(reprojection[i]);
    const auto now=std::chrono::steady_clock::now();
    const float frame_ms=previous_time.time_since_epoch().count()?
      std::clamp(std::chrono::duration<float,std::milli>(now-previous_time).count(),1.f,250.f):1000.f/60;
    previous_time=now;
    c.w=width; c.h=height; c.ow=output_width; c.oh=output_height;
    c.jx=jitter_x; c.jy=jitter_y; c.pjx=previous_jitter_x; c.pjy=previous_jitter_y;
    c.reset=reset; c.linear=mode==Mode::Dlss; c.spatial=!valid||failed||mode==Mode::Spatial;
    c.neural_strength=neural_requested&&valid?neural_strength:0;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=13; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Retired frame{}; frame.submission=serial; Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&frame.heap)));
    frame.capture=fm4::temporal_capture::Request(frame_index);
    auto cpu=frame.heap->GetCPUDescriptorHandleForHeapStart(); auto gpu=frame.heap->GetGPUDescriptorHandleForHeapStart();
    const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    ID3D12Resource* neural_final=nullptr;
#ifdef FM4_HAS_NEURAL
    if(!neural_outputs.empty()) neural_final=neural_outputs.back().Get();
#endif
    ID3D12Resource* srvs[]{queued_input.Get(),valid?queued_depth.Get():nullptr,previous_colour.Get(),mode==Mode::Spatial&&valid&&neural_requested?prepared.Get():upscaled.Get(),reactive.Get(),neural_final};
    DXGI_FORMAT formats[]{DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT};
    for(unsigned i=0;i<6;++i) {
      D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format=formats[i]; d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels=1;
      device->CreateShaderResourceView(srvs[i],&d,{cpu.ptr+i*stride});
    }
    ID3D12Resource* uavs[]{prepared.Get(),motion.Get(),reactive.Get(),queued_output.Get(),nullptr,nullptr,nullptr};
#ifdef FM4_HAS_NEURAL
    for(unsigned i=0;i<neural_outputs.size();++i) uavs[4+i]=neural_outputs[i].Get();
#endif
    for(unsigned i=0;i<7;++i) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.Format=uavs[i]?uavs[i]->GetDesc().Format:DXGI_FORMAT_R16G16B16A16_FLOAT; d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
      device->CreateUnorderedAccessView(uavs[i],nullptr,&d,{cpu.ptr+(i+6)*stride});
    }
    Transition(list,queued_input.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto bind=[&](ID3D12PipelineState* p) { ID3D12DescriptorHeap* heaps[]{frame.heap.Get()}; list->SetDescriptorHeaps(1,heaps); list->SetComputeRootSignature(signature.Get()); list->SetPipelineState(p); list->SetComputeRoot32BitConstants(0,28,&c,0); list->SetComputeRootDescriptorTable(1,gpu); };
    if(valid&&!failed) {
      for(auto* r:{prepared.Get(),motion.Get(),reactive.Get()}) Transition(list,r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      bind(prepare_pipeline.Get()); list->Dispatch((width+7)/8,(height+7)/8,1);
      for(auto* r:{prepared.Get(),motion.Get(),reactive.Get()}) Transition(list,r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
#ifdef FM4_HAS_NEURAL
      if(c.neural_strength>0) {
        ID3D12Resource* colour=prepared.Get();
        for(uint32_t i=0;i<neural_features.size();++i) {
          auto* output=neural_outputs[i].Get();
          Transition(list,output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          neural_features[i]->Dispatch(list,colour,queued_depth.Get(),motion.Get(),output,reset);
          D3D12_RESOURCE_BARRIER order{}; order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; order.UAV.pResource=output; list->ResourceBarrier(1,&order);
          c.pjx=float(i); bind(neural_normalize_pipeline.Get()); list->Dispatch((width+7)/8,(height+7)/8,1);
          Transition(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          colour=output;
        }
        Transition(list,prepared.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bind(neural_pipeline.Get()); list->Dispatch((width+7)/8,(height+7)/8,1);
        Transition(list,prepared.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
#endif
      Transition(list,upscaled.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#ifdef FM4_HAS_FSR_API
      if(mode==Mode::Fsr) {
        ffxDispatchDescUpscale d{}; d.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE; d.commandList=list;
        d.color=ffxApiGetResourceDX12(prepared.Get()); d.depth=ffxApiGetResourceDX12(queued_depth.Get()); d.motionVectors=ffxApiGetResourceDX12(motion.Get());
        d.reactive=ffxApiGetResourceDX12(reactive.Get()); d.output=ffxApiGetResourceDX12(upscaled.Get(),FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset={jitter_x,jitter_y}; d.motionVectorScale={1,1}; d.renderSize={width,height}; d.upscaleSize={output_width,output_height};
        d.enableSharpening=true; d.sharpness=.2f; d.frameTimeDelta=frame_ms; d.preExposure=1; d.reset=reset;
        d.cameraNear=camera.near_plane; d.cameraFar=camera.far_plane; d.cameraFovAngleVertical=camera.fov; d.viewSpaceToMetersFactor=1;
        d.flags=FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB; Fsr(fsr_api.Dispatch(&fsr_context,&d.header));
      }
#endif
#ifdef FM4_HAS_STREAMLINE
      if(mode==Mode::Dlss) {
        sl::ViewportHandle viewport(0); sl::DLSSOptions options{};
        double scale=double(output_width)/width;
        options.mode=scale>=2.8?sl::DLSSMode::eUltraPerformance:scale>=1.9?sl::DLSSMode::eMaxPerformance:scale>1.1?sl::DLSSMode::eMaxQuality:sl::DLSSMode::eDLAA;
        options.outputWidth=output_width; options.outputHeight=output_height; options.colorBuffersHDR=sl::Boolean::eFalse; options.useAutoExposure=sl::Boolean::eTrue;
        Sl(sl_options(viewport,options)); sl::FrameToken* token=nullptr; Sl(sl_token(token,&frame_index));
        auto projection=fm4::camera::Presented(camera.projection); Matrix inv_projection{},camera_world{},inv_reprojection{};
        fm4::camera::Inverse(projection,inv_projection); fm4::camera::Inverse(camera.view,camera_world); fm4::camera::Inverse(reprojection,inv_reprojection);
        sl::Constants common{}; common.cameraViewToClip=SlMatrix(projection); common.clipToCameraView=SlMatrix(inv_projection);
        common.clipToPrevClip=SlMatrix(reprojection); common.prevClipToClip=SlMatrix(inv_reprojection);
        common.jitterOffset={jitter_x,jitter_y}; common.mvecScale={1.f/width,1.f/height}; common.cameraPinholeOffset={0,0};
        common.cameraPos={float(camera_world[12]),float(camera_world[13]),float(camera_world[14])};
        common.cameraRight={float(camera_world[0]),float(camera_world[1]),float(camera_world[2])}; common.cameraUp={float(camera_world[4]),float(camera_world[5]),float(camera_world[6])}; common.cameraFwd={float(camera_world[8]),float(camera_world[9]),float(camera_world[10])};
        common.cameraNear=camera.near_plane; common.cameraFar=camera.far_plane; common.cameraFOV=camera.fov; common.cameraAspectRatio=float(width)/height;
        common.depthInverted=sl::Boolean::eTrue; common.cameraMotionIncluded=sl::Boolean::eTrue; common.motionVectors3D=sl::Boolean::eFalse; common.reset=reset?sl::Boolean::eTrue:sl::Boolean::eFalse;
        Sl(sl_constants(common,*token,viewport)); sl::Extent in_extent{0,0,width,height},out_extent{0,0,output_width,output_height};
        sl::Resource colour(sl::ResourceType::eTex2d,prepared.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),z(sl::ResourceType::eTex2d,queued_depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),mv(sl::ResourceType::eTex2d,motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),out(sl::ResourceType::eTex2d,upscaled.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sl::Resource bias(sl::ResourceType::eTex2d,reactive.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::ResourceTag tags[]{{&colour,sl::kBufferTypeScalingInputColor,sl::ResourceLifecycle::eOnlyValidNow,&in_extent},{&z,sl::kBufferTypeDepth,sl::ResourceLifecycle::eOnlyValidNow,&in_extent},{&mv,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eOnlyValidNow,&in_extent},{&out,sl::kBufferTypeScalingOutputColor,sl::ResourceLifecycle::eOnlyValidNow,&out_extent},{&bias,sl::kBufferTypeBiasCurrentColorHint,sl::ResourceLifecycle::eOnlyValidNow,&in_extent}};
        Sl(sl_tag(*token,viewport,tags,5,list)); const sl::BaseStructure* inputs[]{&viewport}; Sl(sl_evaluate(sl::kFeatureDLSS,*token,inputs,1,list));
      }
#endif
      Transition(list,upscaled.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Transition(list,queued_output.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bind(finish_pipeline.Get()); list->Dispatch((output_width+7)/8,(output_height+7)/8,1);
    Transition(list,queued_output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if(frame.capture) {
      Transition(list,queued_output.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
      fm4::temporal_capture::Copy(device,list,queued_output.Get(),frame.capture->output);
      Transition(list,queued_output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    Transition(list,queued_input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    if(frame.capture) fm4::temporal_capture::Copy(device,list,queued_input.Get(),frame.capture->input);
    Transition(list,previous_colour.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(previous_colour.Get(),queued_input.Get());
    Transition(list,previous_colour.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list,queued_input.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    frame.resources=std::move(resized_resources); frame.resources.push_back(queued_input); frame.resources.push_back(queued_output); frame.resources.push_back(queued_depth);
    retired.push_back(std::move(frame));
    history=valid&&!failed; if(valid) previous_camera=camera;
    previous_jitter_x=jitter_x; previous_jitter_y=jitter_y;
    if(frame_index%120==0 || (valid&&reset)) REXGPU_INFO("FM4 temporal frame={} mode={} input={}x{} output={}x{} camera_draws={} scene_depth={} reset={} dispatch={} neural_passes={} near={} far={}",frame_index,mode==Mode::Dlss?"DLSS":mode==Mode::Fsr?"FSR":"Bilinear",width,height,output_width,output_height,count,queued_valid_depth,reset,valid?"scene":"spatial-menu",c.neural_strength>0?neural_passes:0,camera.near_plane,camera.far_plane);
    queued_input.Reset(); queued_output.Reset(); queued_depth.Reset();
  } catch(const std::exception& e) { Error(e); }
}
extern "C" void Fm4TemporalNextFrame() { votes.clear(); track_vps.clear(); ++frame_index; SetJitter(); }
extern "C" void Fm4TemporalShutdownGpu() {
  pending=false; queued_input.Reset(); queued_output.Reset(); queued_depth.Reset();
#ifdef FM4_HAS_NEURAL
  for(auto i=neural_features.rbegin();i!=neural_features.rend();++i) (*i)->Shutdown();
  neural_features.clear(); neural_outputs.clear();
#endif
  neural_list.Reset(); neural_allocator.Reset(); neural_queue.Reset(); neural_fence.Reset(); neural_fence_serial=0;
#ifdef FM4_HAS_FSR_API
  if(fsr_created) { fsr_api.DestroyContext(&fsr_context,nullptr); fsr_created=false; }
#endif
  input.Reset(); prepared.Reset(); motion.Reset(); reactive.Reset(); upscaled.Reset(); previous_colour.Reset();
  retired.clear(); resized_resources.clear(); signature.Reset(); prepare_pipeline.Reset(); finish_pipeline.Reset(); neural_pipeline.Reset(); neural_normalize_pipeline.Reset(); history=false; width=height=0;
}
extern "C" void Fm4TemporalShutdown() {
  Fm4TemporalShutdownGpu();
#ifdef FM4_HAS_STREAMLINE
  if(sl_initialized) { sl_shutdown(); sl_initialized=false; }
#endif
  if(sl_module) { FreeLibrary(sl_module); sl_module=nullptr; }
  if(fsr_module) { FreeLibrary(fsr_module); fsr_module=nullptr; }
  mode=Mode::None; configured=false; failed=false; gpu_initialized=false; votes.clear(); frame_index=0;
}
extern "C" void Fm4TemporalDeviceLostDiagnostic(ID3D12Device* device) {
  static std::atomic_flag recorded=ATOMIC_FLAG_INIT;
  const char* folder=std::getenv("FM4_CAPTURE_DIR");
  if(!folder||recorded.test_and_set()) return;
  try {
    std::ofstream out(fs::path(folder)/"device-removed.txt");
    out<<"FM4 owned-run GPU diagnostic; no desktop capture\nReason: 0x"<<std::hex<<uint32_t(device->GetDeviceRemovedReason())<<std::dec<<"\n";
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) { out<<"DRED unavailable\n"; return; }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    if(SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
      unsigned nodes=0;
      for(auto* node=breadcrumbs.pHeadAutoBreadcrumbNode;node&&nodes<256;node=node->pNext,++nodes) {
        uint32_t last=node->pLastBreadcrumbValue?std::min(*node->pLastBreadcrumbValue,node->BreadcrumbCount):0;
        if(last==node->BreadcrumbCount) continue;
        std::string name="unnamed";
        if(node->pCommandListDebugNameA) name=node->pCommandListDebugNameA;
        else if(node->pCommandListDebugNameW) {
          char utf8[1024]{};
          if(WideCharToMultiByte(CP_UTF8,0,node->pCommandListDebugNameW,-1,utf8,sizeof(utf8),nullptr,nullptr)>0) name=utf8;
        }
        out<<"Command list: "<<name<<" completed "<<last<<"/"<<node->BreadcrumbCount<<"\n";
        if(node->pCommandHistory) for(uint32_t i=last>8?last-8:0;i<std::min(last+4,node->BreadcrumbCount);++i)
          out<<"  op "<<i<<": "<<int(node->pCommandHistory[i])<<(i==last?" NEXT":"")<<"\n";
      }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if(SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) out<<"Page fault: 0x"<<std::hex<<fault.PageFaultVA<<"\n";
    out.flush();
  } catch(...) { /* Never replace the original GPU-loss error. */ }
}
