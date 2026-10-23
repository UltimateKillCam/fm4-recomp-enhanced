// Verify the current signed FSR loader and actual hardware-selected provider.
// This is a context/API check, not evidence that FM4 supplies temporal inputs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINDOWS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
int wmain(int argc, wchar_t** argv) {
  if (argc != 2) return 2;
  auto path = std::filesystem::absolute(argv[1]) / L"amd_fidelityfx_loader_dx12.dll";
  auto module = LoadLibraryExW(path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) { std::printf("FSR loader failed: %lu\n", GetLastError()); return 1; }
  ffxFunctions api{}; ffxLoadFunctions(&api, module);
  if (!api.CreateContext || !api.Query || !api.Dispatch || !api.DestroyContext) return 1;
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
    if (desc.VendorId == 0x10de && std::wstring(desc.Description).find(L"RTX 4080") != std::wstring::npos) break;
    adapter.Reset();
  }
  if (!adapter) return 1;
  ComPtr<ID3D12Device> device;
  if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return 1;
  uint64_t count = 0;
  ffxQueryDescGetVersions query{}; query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  query.device = device.Get(); query.outputCount = &count;
  auto result = api.Query(nullptr, &query.header);
  std::printf("FSR provider enumeration: %u, providers=%llu\n", unsigned(result), count);
  if (result != FFX_API_RETURN_OK || count > 32) return 1;
  std::vector<uint64_t> versions(count); std::vector<const char*> names(count);
  query.versionIds = versions.data(); query.versionNames = names.data();
  result = api.Query(nullptr, &query.header);
  if (result != FFX_API_RETURN_OK) return 1;
  for (size_t i = 0; i < count; ++i) std::printf("Available: %s (%llu)\n", names[i], versions[i]);
  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device = device.Get();
  ffxCreateContextDescUpscale create{}; create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  create.header.pNext = &backend.header; create.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  create.maxRenderSize = {1280, 720}; create.maxUpscaleSize = {1920, 1080};
  ffxContext context{}; result = api.CreateContext(&context, &create.header, nullptr);
  std::printf("FSR context creation on RTX 4080: %u\n", unsigned(result));
  if (result != FFX_API_RETURN_OK) return 1;
  ffxQueryGetProviderVersion provider{}; provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  result = api.Query(&context, &provider.header);
  std::printf("Selected provider: %s\n", provider.versionName ? provider.versionName : "unknown");
  api.DestroyContext(&context, nullptr);
  device.Reset(); adapter.Reset(); factory.Reset(); FreeLibrary(module);
  std::printf("API/context check only; FM4 depth, motion, jitter and HUD integration remain required.\n");
  return result == FFX_API_RETURN_OK ? 0 : 1;
}
