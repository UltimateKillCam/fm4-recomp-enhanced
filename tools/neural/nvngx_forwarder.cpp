// Copyright (c) 2026 xilla. MIT licensed; see licenses/dlss5-sidecar-mit.txt.
// Copied from Randolin/dlss5-wow-sidecar-revised, source attribution in UPSTREAM.md.
// nvngx.dll_sidecar.dll -- the module the neural-rendering runtime is willing
// to take calls from.
//
// The M3 spikes (docs/spikes/2026-08-30-ngx-feature-probe.md) found that
// nvngx_dlssnr.dll answers FAIL_PlatformError to every call from the sidecar,
// and concluded the runtime only accepts the NGX core as a caller. That was
// half right. The OptiScaler neural-rendering fork (github.com/Dagherbou/
// OptiScaler_DLSSNR) later established the actual rule: the runtime resolves
// the module that owns the return address of the call and requires that
// module's *path* to contain "nvngx.dll" -- which the driver core, _nvngx.dll,
// satisfies by name. Nothing else about the caller is inspected. So a library
// whose filename happens to contain that substring is an acceptable caller,
// and this is that library. Credit for the finding is theirs; the code here
// is written fresh against it, because their project is GPL-3 and this one
// is MIT.
//
// Two consequences shape everything below.
//
// First, every call into the runtime must *return through this module*. A
// `return fn(...)` at the end of a function is a tail call; the compiler
// emits a jump, this module's frame is gone, and the runtime resolves the
// caller to whoever called us. Every result is therefore parked in a
// volatile local before it is returned. That is not paranoia -- it is the
// only reason the file works.
//
// Second, the runtime is driven with the NGX core's own capability parameter
// block, obtained by the host through the SDK. This DLL does not link the
// SDK: it takes the block as an opaque pointer and drives its vtable, which
// keeps the forwarder a few kilobytes with no imports beyond kernel32.
//
// Vtable layout of NVSDK_NGX_Parameter, from the public header: eight Set
// overloads then eight Get overloads, each set declared in the order ULL,
// float, double, uint, int, ID3D11Resource*, ID3D12Resource*, void*. MSVC lays
// out consecutively declared overloads in *reverse* declaration order, so the
// actual slots are:
//
//   Set: void* 0, ID3D12 1, ID3D11 2, int 3, uint 4, double 5, float 6, ULL 7
//   Get: void** 8, ID3D12** 9, ID3D11** 10, int* 11, uint* 12, double* 13,
//        float* 14, ULL* 15
//
// That is why the OptiScaler fork found floats "at slot 6, not the header's
// 1", and why resources land through slot 0 (Set(void*), a pointer either
// way). The host still confirms the float slot by round-tripping a value
// through sidecar_nr_probe_float_slot before anything else is written, so a
// block built by a different compiler would be discovered rather than
// silently misdriven.
//
// Import-table invariants (ci/check_imports.py) apply to this binary like any
// other in the project. It uses LoadLibraryExW and GetProcAddress and nothing
// else from the OS.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "nvngx_forwarder_abi.h"

namespace {

// ---- the parameter block, driven through its vtable ------------------------

constexpr int kSlotSetUll = 0;    // Set(void*) under MSVC's reversed layout; a pointer either way
constexpr int kSlotSetUint = 4;   // Set(unsigned int)
constexpr int kSlotSetInt = 3;    // Set(int); what the first build used for uints, and it worked
// Where floats can be read back. MSVC's layout puts Get(float*) at 14; the
// header's naive reading says 9. Both are tried by the probe.
constexpr int kSlotGetFloatMsvc = 14;
constexpr int kSlotGetFloatHeader = 9;

int g_floatSlot = 6;              // MSVC's answer until probed

using PfnSetUll = void(*)(void*, const char*, unsigned long long);
using PfnSetFloat = void(*)(void*, const char*, float);
using PfnSetUint = void(*)(void*, const char*, unsigned int);
using PfnGetFloat = int(*)(void*, const char*, float*);

void** Vtable(void* params) { return *reinterpret_cast<void***>(params); }

void SetUint(void* params, const char* name, unsigned int value) {
  // Through the uint setter, and through the int setter as well. The first
  // working build drove every uint through slot 3 -- Set(int) under the real
  // layout -- and the model read them, so that path is kept; writing both
  // costs nothing and a block that stores typed values sees the right type.
  reinterpret_cast<PfnSetUint>(Vtable(params)[kSlotSetUint])(params, name, value);
  reinterpret_cast<PfnSetUint>(Vtable(params)[kSlotSetInt])(params, name, value);
}

void SetFloat(void* params, const char* name, float value) {
  reinterpret_cast<PfnSetFloat>(Vtable(params)[g_floatSlot])(params, name, value);
}

// Resources go through the 64-bit setter, not the typed ID3D12Resource one:
// a handle is a 64-bit value, and the typed setter on the driver's block left
// the key unset.
void SetResource(void* params, const char* name, void* resource) {
  reinterpret_cast<PfnSetUll>(Vtable(params)[kSlotSetUll])(
      params, name, reinterpret_cast<unsigned long long>(resource));
}

// ---- the runtime's D3D12 entry points -------------------------------------

using PfnInitExt = int(__cdecl*)(unsigned long long appId, const wchar_t* dataPath,
                                 ID3D12Device* device, int sdkVersion, const void* params);
using PfnCreate = int(__cdecl*)(ID3D12GraphicsCommandList* cl, int featureId,
                                const void* params, void** outHandle);
using PfnEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList* cl, void* handle,
                                  const void* params, void* callback);
using PfnRelease = int(__cdecl*)(void* handle);

constexpr int kNeuralRenderingFeatureId = 18;
constexpr int kSdkVersion = 0x15;

struct Runtime {
  HMODULE module = nullptr;
  PfnInitExt init = nullptr;
  PfnCreate create = nullptr;
  PfnEvaluate evaluate = nullptr;
  PfnRelease release = nullptr;
  bool initialised = false;
} g_runtime;

bool LoadRuntime(const wchar_t* path) {
  if (g_runtime.module) return g_runtime.create != nullptr;
  g_runtime.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!g_runtime.module) return false;
  g_runtime.init = reinterpret_cast<PfnInitExt>(
      GetProcAddress(g_runtime.module, "NVSDK_NGX_D3D12_Init_Ext"));
  g_runtime.create = reinterpret_cast<PfnCreate>(
      GetProcAddress(g_runtime.module, "NVSDK_NGX_D3D12_CreateFeature"));
  g_runtime.evaluate = reinterpret_cast<PfnEvaluate>(
      GetProcAddress(g_runtime.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
  g_runtime.release = reinterpret_cast<PfnRelease>(
      GetProcAddress(g_runtime.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
  return g_runtime.init && g_runtime.create && g_runtime.evaluate;
}

// The knobs the model reads when the feature is built. Written at create, and
// written again at every evaluate because the block is the driver's and is
// shared with whatever else uses NGX in this process.
void WriteTuning(void* params, const SidecarNrTuning& t) {
  SetUint(params, "DLSSNR.Hint.Render.Preset", t.preset);
  SetUint(params, "DLSSNR.Style", t.style);
  SetFloat(params, "DLSSNR.Intensity", t.intensity);
  SetFloat(params, "DLSSNR.LocalStructureStrength", t.localStructure);
  SetFloat(params, "DLSSNR.LocalToneStrength", t.localTone);
  SetFloat(params, "DLSSNR.SkinStructureStrength", t.skinStructure);
  SetUint(params, "DLSSNR.UseAutoMask", t.useAutoMask);
  SetUint(params, "DLSSNR.UICorrection", t.uiCorrection);
}

void WriteRect(void* params, const char* prefix, unsigned int w, unsigned int h) {
  char key[64];
  auto put = [&](const char* suffix, unsigned int value) {
    int i = 0;
    for (const char* p = prefix; *p && i < 48; ++p) key[i++] = *p;
    for (const char* p = suffix; *p && i < 63; ++p) key[i++] = *p;
    key[i] = '\0';
    SetUint(params, key, value);
  };
  put("SubrectBaseX", 0);
  put("SubrectBaseY", 0);
  put("SubrectWidth", w);
  put("SubrectHeight", h);
}

}  // namespace

extern "C" {

__declspec(dllexport) int sidecar_nr_abi_version() { return SIDECAR_NR_ABI_VERSION; }

__declspec(dllexport) int sidecar_nr_probe_float_slot(void* params) {
  if (!params) return -1;
  // MSVC's reversed layout first, then the header's naive order, then the
  // rest. 0.375 is exact in binary, so the round trip is exact or it is wrong.
  // Read back through both plausible float getters: the value only has to
  // come back through one of them.
  static const int kSetters[] = {6, 1, 5, 2, 7, 0, 4, 3};
  static const int kGetters[] = {kSlotGetFloatMsvc, kSlotGetFloatHeader};
  const float expected = 0.375f;
  const char* key = "DLSSNR.SidecarFloatProbe";
  for (const int slot : kSetters) {
    reinterpret_cast<PfnSetFloat>(Vtable(params)[slot])(params, key, expected);
    for (const int getter : kGetters) {
      float read = 0.0f;
      auto get = reinterpret_cast<PfnGetFloat>(Vtable(params)[getter]);
      if (get(params, key, &read) == 1 && read == expected) {
        g_floatSlot = slot;
        return slot;
      }
    }
  }
  return -1;
}

__declspec(dllexport) int sidecar_nr_init(const wchar_t* runtimePath, const wchar_t* dataPath,
                                          unsigned long long appId, ID3D12Device* device,
                                          void* params) {
  if (!LoadRuntime(runtimePath)) return SIDECAR_NR_ERR_RUNTIME_NOT_LOADED;
  if (!device || !params) return SIDECAR_NR_ERR_BAD_ARGUMENT;
  if (g_runtime.initialised) return 1;
  volatile int result = g_runtime.init(appId, dataPath, device, kSdkVersion, params);
  g_runtime.initialised = (result == 1);
  return result;
}

__declspec(dllexport) int sidecar_nr_create(ID3D12GraphicsCommandList* cl, void* params,
                                            unsigned int width, unsigned int height,
                                            const SidecarNrTuning* tuning, void** outHandle) {
  if (!g_runtime.initialised) return SIDECAR_NR_ERR_NOT_INITIALISED;
  if (!cl || !params || !tuning || !outHandle || width == 0 || height == 0) {
    return SIDECAR_NR_ERR_BAD_ARGUMENT;
  }
  SetUint(params, "DLSSNR.Enabled", 1);
  SetUint(params, "DLSSNR.Width", width);
  SetUint(params, "DLSSNR.Height", height);
  SetUint(params, "CreationNodeMask", 1);
  SetUint(params, "VisibilityNodeMask", 1);
  WriteTuning(params, *tuning);

  void* handle = nullptr;
  volatile int result = g_runtime.create(cl, kNeuralRenderingFeatureId, params, &handle);
  *outHandle = (result == 1) ? handle : nullptr;
  return result;
}

__declspec(dllexport) int sidecar_nr_evaluate(ID3D12GraphicsCommandList* cl, void* handle,
                                              void* params, const SidecarNrEval* eval,
                                              const SidecarNrTuning* tuning) {
  if (!g_runtime.initialised) return SIDECAR_NR_ERR_NOT_INITIALISED;
  if (!cl || !handle || !params || !eval || !tuning) return SIDECAR_NR_ERR_BAD_ARGUMENT;

  SetResource(params, "DLSSNR.Color", eval->color);
  SetResource(params, "DLSSNR.Depth", eval->depth);
  SetResource(params, "DLSSNR.MVec", eval->motion);
  SetResource(params, "DLSSNR.Output", eval->output);

  SetUint(params, "DLSSNR.Enabled", 1);
  SetUint(params, "DLSSNR.Width", eval->width);
  SetUint(params, "DLSSNR.Height", eval->height);
  SetUint(params, "DLSSNR.DepthInverted", eval->depthInverted);
  SetUint(params, "DLSSNR.Reset", eval->reset);
  WriteRect(params, "DLSSNR.Color", eval->width, eval->height);
  WriteRect(params, "DLSSNR.Output", eval->width, eval->height);
  WriteRect(params, "DLSSNR.Depth", eval->guideWidth, eval->guideHeight);
  WriteRect(params, "DLSSNR.MVec", eval->guideWidth, eval->guideHeight);
  SetFloat(params, "DLSSNR.MVecScaleX", eval->mvScaleX);
  SetFloat(params, "DLSSNR.MVecScaleY", eval->mvScaleY);
  WriteTuning(params, *tuning);

  volatile int result = g_runtime.evaluate(cl, handle, params, nullptr);
  return result;
}

__declspec(dllexport) int sidecar_nr_release(void* handle) {
  if (!handle || !g_runtime.release) return SIDECAR_NR_ERR_BAD_ARGUMENT;
  volatile int result = g_runtime.release(handle);
  return result;
}

}  // extern "C"
