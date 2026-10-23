// Standalone diagnostic. This does not run a DLSS effect or change the game.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_helpers.h>
#include <filesystem>
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  std::filesystem::path plugins = std::filesystem::absolute(argv[1]);
  std::wstring dll = (plugins / L"sl.interposer.dll").wstring();
  HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) { std::printf("Cannot load signed Streamline SDK: %lu\n", GetLastError()); return 1; }
  auto init = reinterpret_cast<PFun_slInit*>(GetProcAddress(module, "slInit"));
  auto shutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(module, "slShutdown"));
  auto set_device = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(module, "slSetD3DDevice"));
  auto supported = reinterpret_cast<PFun_slIsFeatureSupported*>(GetProcAddress(module, "slIsFeatureSupported"));
  if (!init || !shutdown || !set_device || !supported) return 1;
  sl::Feature features[]{sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureDLSS_NR, sl::kFeatureReflex, sl::kFeaturePCL};
  auto plugin_path = plugins.wstring(); const wchar_t* paths[]{plugin_path.c_str()};
  sl::Preferences pref{};
  pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eDisableDebugText;
  pref.featuresToLoad = features; pref.numFeaturesToLoad = 5;
  pref.pathsToPlugins = paths; pref.numPathsToPlugins = 1;
  pref.pathToLogsAndData = argv[2]; pref.logLevel = sl::LogLevel::eVerbose;
  pref.engine = sl::EngineType::eCustom; pref.engineVersion = "FM4-ReXGlue-Enhanced-1";
  pref.projectId = "2249d991-61bc-4a73-a6c0-746df2973b51";
  auto result = init(pref, sl::kSDKVersion);
  std::printf("Streamline init: %s\n", sl::getResultAsStr(result));
  if (result != sl::Result::eOk) { FreeLibrary(module); return 1; }
  ComPtr<IDXGIFactory6> factory; if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
    if (d.VendorId == 0x10DE && std::wstring(d.Description).find(L"RTX 4080") != std::wstring::npos) break;
    adapter.Reset();
  }
  if (!adapter) { shutdown(); FreeLibrary(module); return 1; }
  DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
  ComPtr<ID3D12Device> device;
  if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 1;
  std::wprintf(L"Adapter: %ls\n", desc.Description);
  result = set_device(device.Get()); std::printf("Streamline device: %s\n", sl::getResultAsStr(result));
  sl::AdapterInfo info{}; info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid); info.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
  const char* names[]{"DLSS Super Resolution", "DLSS Frame Generation", "DLSS 5 Neural Rendering"};
  for (int i = 0; i < 3; ++i) std::printf("%s: %s\n", names[i], sl::getResultAsStr(supported(features[i], info)));
  std::printf("NR plugin supplied: %s\n", std::filesystem::exists(plugins / L"sl.dlss_nr.dll") ? "yes" : "no");
  std::printf("Capability probe only; no DLSS effects are integrated in FM4.\n");
  shutdown(); device.Reset(); adapter.Reset(); factory.Reset(); FreeLibrary(module); return 0;
}
