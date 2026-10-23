#pragma once
// FM4 host adapter for experimental DLSS 5 Neural Rendering (NGX feature 18).
// GPL-3.0-only, 2026-10-16; official RTX 50 path added 2026-10-20.
// Separately supplied runtimes are not redistributed.
//
// Two runtime paths, chosen by the launcher from the GPU generation (FM4_NEURAL_PATH):
// - Community (RTX 40 / Ada, tested on an RTX 4080): an Ada-compatible community
//   runtime is driven through the source-built direct bridge nvngx.dll_fm4.dll.
// - Official (RTX 50 / Blackwell, not yet tested on RTX 50 hardware): the driver's
//   NGX core creates and evaluates feature 18 with NVIDIA's own nvngx_dlssnr.dll,
//   found through the runtime folder in the NGX feature path list. Parameters go
//   through the SDK's typed C setters; no vtable slots are guessed. If the driver
//   declines the feature for this application, the same runtime is driven through
//   the direct bridge when that bridge has been built.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include "nvngx_forwarder_abi.h"
#pragma comment(lib, "dxgi.lib")

namespace fm4 {
enum class NeuralPath { Community, Official };
// Unset keeps the original community behaviour for older launchers and replay tools.
inline NeuralPath NeuralPathFromEnvironment() {
  const char* value=std::getenv("FM4_NEURAL_PATH");
  if(!value||!*value||std::string(value)=="community") return NeuralPath::Community;
  if(std::string(value)=="official") return NeuralPath::Official;
  throw std::runtime_error("FM4_NEURAL_PATH must be official or community");
}
class NeuralRendering {
  using Init=int(__cdecl*)(const wchar_t*,const wchar_t*,unsigned long long,ID3D12Device*,void*);
  using Create=int(__cdecl*)(ID3D12GraphicsCommandList*,void*,unsigned,unsigned,const SidecarNrTuning*,void**);
  using Evaluate=int(__cdecl*)(ID3D12GraphicsCommandList*,void*,void*,const SidecarNrEval*,const SidecarNrTuning*);
  using Release=int(__cdecl*)(void*);
  static constexpr NVSDK_NGX_Feature kFeature=NVSDK_NGX_Feature(18);
  HMODULE bridge_=nullptr;
  ID3D12Device* device_=nullptr;
  NVSDK_NGX_Parameter* parameters_=nullptr;
  void* feature_=nullptr;
  NVSDK_NGX_Handle* driver_feature_=nullptr;
  Init init_=nullptr; Create create_=nullptr; Evaluate evaluate_=nullptr; Release release_=nullptr;
  bool owns_core_=false;
  bool owns_parameters_=false;
  // Official path: true until the driver declines feature 18 and the bridge takes over.
  bool driver_managed_=false;
  bool bridge_ready_=false;
  NeuralPath path_=NeuralPath::Community;
  NVSDK_NGX_Result driver_result_=NVSDK_NGX_Result_Success;
  const NeuralRendering* session_owner_=nullptr;
  std::filesystem::path folder_,model_,bridge_directory_;
  uint32_t width_=0,height_=0;
  DWORD owner_thread_=0;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> initialization_queue_;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> initialization_allocator_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> initialization_list_;
  Microsoft::WRL::ComPtr<ID3D12Fence> initialization_fence_;
  // Match the working sidecar's neutral default: skin synthesis is disabled
  // unless a host explicitly requests it. FM4's normal view is cars/scenery.
  SidecarNrTuning tuning_{0,0,1,1,1,-1,1,1};
  template<class T> T Symbol(const char* name) {
    auto p=reinterpret_cast<T>(GetProcAddress(bridge_,name));
    if(!p) throw std::runtime_error(std::string("Missing Neural Rendering bridge entry point: ")+name);
    return p;
  }
  static void Check(int result,const char* operation) {
    if(result!=1) throw std::runtime_error(std::string("Neural Rendering ")+operation+" result "+std::to_string(uint32_t(result)));
  }
  static void Hr(HRESULT result) { if(FAILED(result)) throw std::runtime_error("Neural Rendering D3D12 failure "+std::to_string(uint32_t(result))); }
  static bool Trace() { return std::getenv("FM4_NEURAL_TRACE_INIT")!=nullptr; }
  static int InitGuard(Init fn,const wchar_t* model,const wchar_t* data,uint64_t id,ID3D12Device* device,void* params) {
    __try { return fn(model,data,id,device,params); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return int(GetExceptionCode()); }
  }
  static int CreateGuard(Create fn,ID3D12GraphicsCommandList* list,void* params,unsigned w,unsigned h,const SidecarNrTuning* tuning,void** feature) {
    __try { return fn(list,params,w,h,tuning,feature); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return int(GetExceptionCode()); }
  }
  static int EvaluateGuard(Evaluate fn,ID3D12GraphicsCommandList* list,void* feature,void* params,const SidecarNrEval* eval,const SidecarNrTuning* tuning) {
    __try { return fn(list,feature,params,eval,tuning); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return int(GetExceptionCode()); }
  }
  static NVSDK_NGX_Result DriverCreateGuard(ID3D12GraphicsCommandList* list,NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** feature) {
    __try { return NVSDK_NGX_D3D12_CreateFeature(list,kFeature,params,feature); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result(GetExceptionCode()); }
  }
  static NVSDK_NGX_Result DriverEvaluateGuard(ID3D12GraphicsCommandList* list,NVSDK_NGX_Handle* feature,NVSDK_NGX_Parameter* params) {
    __try { return NVSDK_NGX_D3D12_EvaluateFeature(list,feature,params,nullptr); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result(GetExceptionCode()); }
  }
  // Loads the source-built bridge and initializes the runtime through it.
  void StartBridge() {
    const auto bridge=bridge_directory_/L"nvngx.dll_fm4.dll";
    bridge_=LoadLibraryExW(bridge.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!bridge_) throw std::runtime_error("Build the source-only FM4 neural compatibility bridge first");
    auto abi=Symbol<int(*)()>("sidecar_nr_abi_version");
    if(abi()!=SIDECAR_NR_ABI_VERSION) throw std::runtime_error("Neural compatibility bridge ABI mismatch");
    init_=Symbol<Init>("sidecar_nr_init"); create_=Symbol<Create>("sidecar_nr_create"); evaluate_=Symbol<Evaluate>("sidecar_nr_evaluate"); release_=Symbol<Release>("sidecar_nr_release");
    int initialized=InitGuard(init_,model_.c_str(),folder_.c_str(),0x464d344e52ull,device_,parameters_);
    // Public compatibility projects use this generic NGX application ID when
    // the runtime declines an unregistered host's own identifier.
    if(initialized!=1) initialized=InitGuard(init_,model_.c_str(),folder_.c_str(),0x24480451ull,device_,parameters_);
    Check(initialized,"model initialization");
    bridge_ready_=true;
  }
  // Records what the driver reports for feature 18 on this adapter (official path only).
  void LogDriverRequirements() const {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||FAILED(factory->EnumAdapterByLuid(device_->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))) return;
    const auto data=folder_.wstring(); const wchar_t* paths[]{data.c_str()};
    NVSDK_NGX_FeatureCommonInfo common{}; common.PathListInfo={paths,1};
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    discovery.SDKVersion=NVSDK_NGX_Version_API; discovery.FeatureID=kFeature;
    discovery.Identifier.IdentifierType=NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc={"2249d991-61bc-4a73-a6c0-746df2973b51",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"FM4-Enhanced-1"};
    discovery.ApplicationDataPath=data.c_str(); discovery.FeatureInfo=&common;
    NVSDK_NGX_FeatureRequirement requirement{};
    const auto result=NVSDK_NGX_D3D12_GetFeatureRequirements(adapter.Get(),&discovery,&requirement);
    std::fprintf(stderr,"NR official path: feature 18 requirements 0x%08X, support bits 0x%X, minimum architecture 0x%X\n",
      unsigned(result),unsigned(requirement.FeatureSupported),requirement.MinHWArchitecture);
  }
  void SetUint(const char* key,unsigned value) { NVSDK_NGX_Parameter_SetUI(parameters_,key,value); NVSDK_NGX_Parameter_SetI(parameters_,key,int(value)); }
  void SetRect(const std::string& prefix,unsigned w,unsigned h) {
    SetUint((prefix+"SubrectBaseX").c_str(),0); SetUint((prefix+"SubrectBaseY").c_str(),0);
    SetUint((prefix+"SubrectWidth").c_str(),w); SetUint((prefix+"SubrectHeight").c_str(),h);
  }
  // Same keys and values the bridge writes, through the SDK's own setters.
  void WriteDriverTuning(unsigned w,unsigned h) {
    SetUint("DLSSNR.Enabled",1); SetUint("DLSSNR.Width",w); SetUint("DLSSNR.Height",h);
    SetUint("CreationNodeMask",1); SetUint("VisibilityNodeMask",1);
    SetUint("DLSSNR.Hint.Render.Preset",tuning_.preset); SetUint("DLSSNR.Style",tuning_.style);
    NVSDK_NGX_Parameter_SetF(parameters_,"DLSSNR.Intensity",tuning_.intensity);
    NVSDK_NGX_Parameter_SetF(parameters_,"DLSSNR.LocalStructureStrength",tuning_.localStructure);
    NVSDK_NGX_Parameter_SetF(parameters_,"DLSSNR.LocalToneStrength",tuning_.localTone);
    NVSDK_NGX_Parameter_SetF(parameters_,"DLSSNR.SkinStructureStrength",tuning_.skinStructure);
    SetUint("DLSSNR.UseAutoMask",tuning_.useAutoMask); SetUint("DLSSNR.UICorrection",tuning_.uiCorrection);
  }
  struct CreationContext {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  };
  CreationContext NewCreationContext() {
    CreationContext c; D3D12_COMMAND_QUEUE_DESC q{};
    Hr(device_->CreateCommandQueue(&q,IID_PPV_ARGS(&c.queue)));
    Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c.allocator)));
    Hr(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c.allocator.Get(),nullptr,IID_PPV_ARGS(&c.list)));
    return c;
  }
  // Creation is completed on a private queue before evaluation. Mixing
  // create/evaluate in one list is known to hang the experimental runtime.
  void FinishCreation(CreationContext& c) {
    using Microsoft::WRL::ComPtr;
    Hr(c.list->Close()); ID3D12CommandList* lists[]{c.list.Get()}; c.queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; Hr(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!event) throw std::runtime_error("Neural feature wait event unavailable");
    HRESULT signaled=c.queue->Signal(fence.Get(),1),armed=SUCCEEDED(signaled)?fence->SetEventOnCompletion(1,event):signaled;
    DWORD wait=SUCCEEDED(armed)?WaitForSingleObject(event,30000):WAIT_FAILED; CloseHandle(event);
    Hr(armed); if(wait!=WAIT_OBJECT_0) throw std::runtime_error("Neural feature initialization GPU timeout");
    // Validate the known float setter through the SDK's typed getter. Do not
    // probe unrelated virtual methods with guessed signatures in a live host.
    float intensity=0;
    Check(int(parameters_->Get("DLSSNR.Intensity",&intensity)),"typed parameter readback");
    if(intensity!=tuning_.intensity) throw std::runtime_error("Neural intensity parameter layout mismatch");
    // Preserve creation objects for the complete lifetime of the feature. The
    // experimental runtime may retain driver callbacks associated with them.
    initialization_queue_=c.queue; initialization_allocator_=c.allocator;
    initialization_list_=c.list; initialization_fence_=fence;
  }
  // Official path: asks the driver's NGX core for feature 18. Returns false when the
  // driver declines; the caller then falls back to the bridge.
  bool CreateDriverFeature(uint32_t w,uint32_t h) {
    WriteDriverTuning(w,h);
    size_t scratch=0;
    const auto scratch_result=NVSDK_NGX_D3D12_GetScratchBufferSize(kFeature,parameters_,&scratch);
    auto c=NewCreationContext();
    NVSDK_NGX_Handle* handle=nullptr;
    driver_result_=DriverCreateGuard(c.list.Get(),parameters_,&handle);
    std::fprintf(stderr,"NR official path: scratch query 0x%08X (%zu bytes), driver feature 18 creation 0x%08X\n",
      unsigned(scratch_result),scratch,unsigned(driver_result_));
    if(driver_result_!=NVSDK_NGX_Result_Success||!handle) {
      // Nothing from the declined creation is submitted.
      if(handle) NVSDK_NGX_D3D12_ReleaseFeature(handle);
      c.list->Close();
      return false;
    }
    driver_feature_=handle;
    FinishCreation(c);
    return true;
  }
public:
  NeuralRendering()=default;
  NeuralRendering(const NeuralRendering&)=delete;
  NeuralRendering& operator=(const NeuralRendering&)=delete;
  ~NeuralRendering() { Shutdown(); }
  void Initialize(ID3D12Device* device,const std::filesystem::path& directory,const std::filesystem::path& bridge_directory,NeuralPath path=NeuralPath::Community) {
    namespace fs=std::filesystem;
    if(device_) throw std::runtime_error("Neural Rendering already initialized");
    folder_=fs::absolute(directory); model_=folder_/L"nvngx_dlssnr.dll";
    if(!fs::is_regular_file(model_)) throw std::runtime_error("Supply an architecture-compatible nvngx_dlssnr.dll");
    bridge_directory_=fs::absolute(bridge_directory);
    path_=path;
    // The community path cannot run without the bridge; check it before touching NGX.
    if(path_==NeuralPath::Community&&!fs::is_regular_file(bridge_directory_/L"nvngx.dll_fm4.dll"))
      throw std::runtime_error("Build the source-only FM4 neural compatibility bridge first");
    device_=device;
    owner_thread_=GetCurrentThreadId();
    // Reuse an existing NGX core (for example Streamline's) when available.
    auto result=NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters_);
    if(Trace()) std::fprintf(stderr,"NR initial capabilities: 0x%08X, pointer=%p\n",unsigned(result),static_cast<void*>(parameters_));
    if(result!=NVSDK_NGX_Result_Success||!parameters_) {
      // The runtime folder is in the feature path list, so the driver core can
      // also find nvngx_dlssnr.dll there on the official path.
      const auto data=folder_.wstring(); const wchar_t* paths[]{data.c_str()};
      NVSDK_NGX_FeatureCommonInfo common{}; common.PathListInfo={paths,1};
      result=NVSDK_NGX_D3D12_Init_with_ProjectID("2249d991-61bc-4a73-a6c0-746df2973b51",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"FM4-Enhanced-1",data.c_str(),device,&common,NVSDK_NGX_Version_API);
      Check(int(result),"NGX initialization"); owns_core_=true;
      Check(int(NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters_)),"NGX capability parameters");
    }
    int available=0;
    Check(int(parameters_->Get(NVSDK_NGX_Parameter_SuperSampling_Available,&available)),"internal DLSS capability query");
    if(!available) throw std::runtime_error("Neural Rendering requires an available compatible DLSS runtime for its internal temporal pass");
    if(Trace()) std::fprintf(stderr,"NR initialized NGX core=%d; internal DLSS available=%d; path=%s\n",int(owns_core_),available,path_==NeuralPath::Official?"official":"community");
    owns_parameters_=true;
    if(path_==NeuralPath::Official) { driver_managed_=true; LogDriverRequirements(); }
    else StartBridge();
  }
  void ShareSession(const NeuralRendering& owner) {
    if(device_||!owner.device_||!owner.parameters_||(!owner.bridge_&&!owner.driver_managed_)) throw std::runtime_error("Invalid shared Neural Rendering session");
    // The runtime was initialized against this exact capability block. Every
    // chained feature must use that same block, as the working upstream hosts
    // do. A fresh NGX capability block is not a second initialized NR session.
    bridge_=owner.bridge_; device_=owner.device_; parameters_=owner.parameters_;
    owner_thread_=owner.owner_thread_;
    init_=owner.init_; create_=owner.create_; evaluate_=owner.evaluate_; release_=owner.release_;
    path_=owner.path_; driver_managed_=owner.driver_managed_; bridge_ready_=owner.bridge_ready_;
    folder_=owner.folder_; model_=owner.model_; bridge_directory_=owner.bridge_directory_;
    session_owner_=&owner;
    owns_parameters_=false; owns_core_=false;
  }
  void Configure(uint32_t preset,uint32_t style,float strength) {
    if(feature_||driver_feature_) throw std::runtime_error("Neural options require feature recreation after GPU completion");
    if(preset>3||style>2||!std::isfinite(strength)||strength<0||strength>2) throw std::runtime_error("Invalid Neural Rendering options");
    tuning_.preset=preset; tuning_.style=style; tuning_.intensity=strength;
  }
  // Create the session owner's feature first: chained passes follow its path.
  void CreateFeature(uint32_t w,uint32_t h) {
    if(GetCurrentThreadId()!=owner_thread_) throw std::runtime_error("Create Neural Rendering on its initialization thread");
    if(feature_||driver_feature_||!device_||w<16||h<16||w>7680||h>4320) throw std::runtime_error("Invalid Neural Rendering feature dimensions");
    if(session_owner_&&session_owner_->bridge_ready_&&!bridge_ready_) {
      // The owner fell back to the bridge after the driver declined; follow it.
      bridge_=session_owner_->bridge_; init_=session_owner_->init_; create_=session_owner_->create_;
      evaluate_=session_owner_->evaluate_; release_=session_owner_->release_;
      driver_managed_=false; bridge_ready_=true; driver_result_=session_owner_->driver_result_;
    }
    if(driver_managed_) {
      if(CreateDriverFeature(w,h)) { width_=w; height_=h; return; }
      driver_managed_=false;
      if(session_owner_) throw std::runtime_error("Neural Rendering driver path declined a chained pass after the first pass was created");
      if(!std::filesystem::is_regular_file(bridge_directory_/L"nvngx.dll_fm4.dll"))
        throw std::runtime_error("The NVIDIA driver declined official Neural Rendering (result "+std::to_string(uint32_t(driver_result_))+
          "); build the direct bridge with tools/Build-Graphics.cmd or choose the community path");
      std::fprintf(stderr,"NR official path: driver declined feature 18 (0x%08X); using the direct bridge with the same runtime\n",unsigned(driver_result_));
      StartBridge();
    }
    auto c=NewCreationContext();
    Check(CreateGuard(create_,c.list.Get(),parameters_,w,h,&tuning_,&feature_),"feature creation");
    FinishCreation(c);
    width_=w; height_=h;
  }
  void Dispatch(ID3D12GraphicsCommandList* list,ID3D12Resource* colour,ID3D12Resource* depth,ID3D12Resource* motion,ID3D12Resource* output,bool reset) {
    if(GetCurrentThreadId()!=owner_thread_) throw std::runtime_error("Evaluate Neural Rendering on its initialization thread");
    if(!feature_&&!driver_feature_) throw std::runtime_error("Neural feature is not created");
    if(colour->GetDesc().Format!=DXGI_FORMAT_R16G16B16A16_FLOAT ||
       depth->GetDesc().Format!=DXGI_FORMAT_R32_FLOAT ||
       motion->GetDesc().Format!=DXGI_FORMAT_R16G16_FLOAT ||
       output->GetDesc().Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)
      throw std::runtime_error("Neural Rendering requires RGBA16F colour/output, R32F depth and RG16F motion");
    SidecarNrEval eval{colour,depth,motion,output,width_,height_,uint32_t(depth->GetDesc().Width),depth->GetDesc().Height,1,reset?1u:0u,1,1};
    if(driver_feature_) {
      // The parameter block is shared with other NGX users; write every input each frame.
      WriteDriverTuning(width_,height_);
      NVSDK_NGX_Parameter_SetVoidPointer(parameters_,"DLSSNR.Color",colour);
      NVSDK_NGX_Parameter_SetVoidPointer(parameters_,"DLSSNR.Depth",depth);
      NVSDK_NGX_Parameter_SetVoidPointer(parameters_,"DLSSNR.MVec",motion);
      NVSDK_NGX_Parameter_SetVoidPointer(parameters_,"DLSSNR.Output",output);
      SetUint("DLSSNR.DepthInverted",eval.depthInverted); SetUint("DLSSNR.Reset",eval.reset);
      SetRect("DLSSNR.Color",eval.width,eval.height); SetRect("DLSSNR.Output",eval.width,eval.height);
      SetRect("DLSSNR.Depth",eval.guideWidth,eval.guideHeight); SetRect("DLSSNR.MVec",eval.guideWidth,eval.guideHeight);
      NVSDK_NGX_Parameter_SetF(parameters_,"DLSSNR.MVecScaleX",eval.mvScaleX);
      NVSDK_NGX_Parameter_SetF(parameters_,"DLSSNR.MVecScaleY",eval.mvScaleY);
      Check(int(DriverEvaluateGuard(list,driver_feature_,parameters_)),"driver feature 18 evaluation");
      return;
    }
    Check(EvaluateGuard(evaluate_,list,feature_,parameters_,&eval,&tuning_),"feature 18 evaluation");
  }
  // Which path is actually running, for logs and receipts.
  std::string Describe() const {
    if(driver_feature_) return "official NVIDIA path (driver-managed feature 18)";
    if(path_==NeuralPath::Official) return "official runtime through the direct bridge (driver result "+std::to_string(uint32_t(driver_result_))+")";
    return "community compatibility path (direct bridge)";
  }
  void Shutdown() {
    // The host must have completed GPU work before destroying the adapter.
    if(driver_feature_) { NVSDK_NGX_D3D12_ReleaseFeature(driver_feature_); driver_feature_=nullptr; }
    if(feature_&&release_) { release_(feature_); feature_=nullptr; }
    if(parameters_&&owns_parameters_) NVSDK_NGX_D3D12_DestroyParameters(parameters_);
    parameters_=nullptr; owns_parameters_=false;
    if(owns_core_&&device_) NVSDK_NGX_D3D12_Shutdown1(device_);
    device_=nullptr; owns_core_=false; session_owner_=nullptr;
    initialization_list_.Reset(); initialization_allocator_.Reset();
    initialization_fence_.Reset(); initialization_queue_.Reset();
    // Keep the tiny bridge/model module loaded until process teardown: the
    // driver may retain callbacks even after releasing a feature.
  }
};
} // namespace fm4
