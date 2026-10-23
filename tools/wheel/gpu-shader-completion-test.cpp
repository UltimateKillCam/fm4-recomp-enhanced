// GPU-to-guest-memory shader completion ordering regression.
// Modified 2026-10-18. GPL-3.0-only. No game data, window or physical feedback.
#define NOMINMAX
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <stdexcept>
#include <source_location>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static ComPtr<ID3D12InfoQueue> diagnostics;
static void Check(HRESULT hr, std::source_location where=std::source_location::current()) {
  if (FAILED(hr)) {
    if (diagnostics) for (UINT64 n=0;n<diagnostics->GetNumStoredMessages();++n) {
      SIZE_T size=0;diagnostics->GetMessage(n,nullptr,&size);
      std::vector<uint8_t> storage(size);
      auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      if (SUCCEEDED(diagnostics->GetMessage(n,message,&size)))
        std::fprintf(stderr,"D3D12: %s\n",message->pDescription);
    }
    throw std::runtime_error("D3D failure " + std::to_string(uint32_t(hr)) + " at line " + std::to_string(where.line()));
  }
}
struct Mapping {
  HANDLE file = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, 1 << 20, nullptr);
  void *cpu = nullptr;
  void *gpu = nullptr;
  Mapping() {
    if (!file)
      throw std::runtime_error("Mapping creation failed");
    cpu = MapViewOfFile(file, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    gpu = MapViewOfFile(file, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!cpu || !gpu)
      throw std::runtime_error("Mapping view failed");
  }
  ~Mapping() {
    if (gpu)
      UnmapViewOfFile(gpu);
    if (cpu)
      UnmapViewOfFile(cpu);
    if (file)
      CloseHandle(file);
  }
};
static D3D12_RESOURCE_DESC
Buffer(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  return d;
}
int main() try {
  ComPtr<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
  ComPtr<IDXGIFactory6> factory;
  Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT n = 0; factory->EnumAdapterByGpuPreference(
                       n, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                       IID_PPV_ARGS(&adapter)) == S_OK;
       ++n) {
    DXGI_ADAPTER_DESC1 d{};
    Check(adapter->GetDesc1(&d));
    if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      break;
    adapter.Reset();
  }
  if (!adapter) {
    std::puts("SKIP: No hardware adapter");
    return 77;
  }
  ComPtr<ID3D12Device3> device;
  Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                          IID_PPV_ARGS(&device)));
  device.As(&diagnostics);
  D3D12_FEATURE_DATA_EXISTING_HEAPS supported{};
  Check(device->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &supported,
                                    sizeof(supported)));
  if (!supported.Supported) {
    std::puts("SKIP: Existing-heap import unsupported; renderer uses "
              "synchronous fallback");
    return 77;
  }
  D3D12_COMMAND_QUEUE_DESC q{};
  ComPtr<ID3D12CommandQueue> queue;
  Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  unsigned cases = 0;
  for (unsigned repeat = 0; repeat < 8; ++repeat)
  for (bool inline_barriers : {false, true})
  for (UINT size : {4u, 32u})
    for (UINT address : {0x2000u, 0x2fe0u, 0x3fc0u}) {
      Mapping mapping;
      std::memset(mapping.cpu, 0xCD, 1 << 20);
      ComPtr<ID3D12Heap> heap;
      Check(device->OpenExistingHeapFromAddress(mapping.gpu,
                                                IID_PPV_ARGS(&heap)));
      auto desc = Buffer(1 << 20, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
      ComPtr<ID3D12Resource> destination;
      Check(device->CreatePlacedResource(heap.Get(), 0, &desc,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         IID_PPV_ARGS(&destination)));
      D3D12_HEAP_PROPERTIES upload_properties{};
      upload_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
      auto upload_desc = Buffer(8192);
      ComPtr<ID3D12Resource> upload;
      Check(device->CreateCommittedResource(
          &upload_properties, D3D12_HEAP_FLAG_NONE, &upload_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
      uint8_t *source = nullptr;
      D3D12_RANGE empty{};
      Check(upload->Map(0, &empty, reinterpret_cast<void **>(&source)));
      for (UINT n = 0; n < 8192; ++n)
        source[n] = uint8_t(n * 73 + cases + 1);
      const uint32_t marker = 0x12340000u + cases;
      std::memcpy(source + 4096, &marker, 4);
      const uint32_t counter_marker = 0x56780000u + cases;
      if (size == 32) std::memcpy(source + 6144, &counter_marker, 4);
      ComPtr<ID3D12CommandAllocator> allocator;
      Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocator)));
      ComPtr<ID3D12GraphicsCommandList> list;
      Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&list)));
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = destination.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      list->ResourceBarrier(1, &barrier);
      list->CopyBufferRegion(destination.Get(), 0x10000, upload.Get(), 0, 4096);
      if (size == 32) {
        for (UINT word = 1; word < 8; ++word)
          list->CopyBufferRegion(destination.Get(), address + word * 4,
                                 upload.Get(), 6144 + word * 4, 4);
      }
      ComPtr<ID3D12GraphicsCommandList> completion;
      if (!inline_barriers) {
      Check(list->Close());
      Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&completion)));
      }
      auto* writer = inline_barriers ? list.Get() : completion.Get();
      D3D12_RESOURCE_BARRIER order[2]{};
      order[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      order[0].Transition.pResource = destination.Get();
      order[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      order[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      order[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      order[1] = order[0];
      order[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      order[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      if (inline_barriers) writer->ResourceBarrier(2, order);
      if (size == 32) {
        // Stress a marker-last32byte payload as well as ordinary shader words.
        // Synthetic guest visibility queries use the separate CPU path.
        writer->CopyBufferRegion(destination.Get(), address, upload.Get(), 6144, 4);
      } else {
        writer->CopyBufferRegion(destination.Get(), address, upload.Get(), 6144, size);
      }
      writer->CopyBufferRegion(destination.Get(), 0x8000, upload.Get(),
                                   4096, 4);
      if (inline_barriers) writer->ResourceBarrier(2, order);
      if (inline_barriers) Check(list->Close());
      else Check(completion->Close());
      ComPtr<ID3D12Fence> gate, done;
      Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
      Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
      struct ReleaseGate {
        ID3D12Fence *gate;
        ~ReleaseGate() { gate->Signal(1); }
      } release{gate.Get()};
      Check(queue->Wait(gate.Get(), 1));
      ID3D12CommandList *lists[]{list.Get()};
      queue->ExecuteCommandLists(1, lists);
      ID3D12CommandList *completion_lists[]{completion.Get()};
      if (!inline_barriers) queue->ExecuteCommandLists(1, completion_lists);
      Check(queue->Signal(done.Get(), 1));
      auto *cpu = static_cast<uint8_t *>(mapping.cpu);
      MemoryBarrier();
      if (*reinterpret_cast<volatile uint32_t *>(cpu + 0x8000) != 0xCDCDCDCDu ||
          cpu[address] != 0xCD || cpu[0x10000] != 0xCD)
        throw std::runtime_error(
            "Completion/data changed before GPU gate release");
      Check(gate->Signal(1));
      const UINT observed_address = size == 32 ? address : 0x8000;
      const uint32_t observed_marker = size == 32 ? counter_marker : marker;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(8);
      while (*reinterpret_cast<volatile uint32_t *>(cpu + observed_address) != observed_marker &&
             std::chrono::steady_clock::now() < deadline) {
        MemoryBarrier();
        Sleep(1);
      }
      MemoryBarrier();
      if (*reinterpret_cast<volatile uint32_t *>(cpu + observed_address) != observed_marker)
        throw std::runtime_error(
            "GPU completion not visible through independent CPU mapping");
      // Deliberately inspect before waiting for the D3D submission fence, as
      // the guest polls its completion word instead of the host D3D12 fence
      // object.
      if (std::memcmp(cpu + 0x10000, source, 4096) ||
          std::memcmp(cpu + address, source + 6144, size))
        throw std::runtime_error(
            "Completion became visible before preceding data");
      for (UINT n = 1; n <= 16; ++n)
        if (cpu[address - n] != 0xCD || cpu[address + size + n - 1] != 0xCD)
          throw std::runtime_error("Completion payload corrupted guard bytes");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event)
        throw std::runtime_error("Event creation failed");
      Check(done->SetEventOnCompletion(1, event));
      DWORD waited = WaitForSingleObject(event, 8000);
      CloseHandle(event);
      if (waited != WAIT_OBJECT_0)
        throw std::runtime_error("Submission did not finish");
      upload->Unmap(0, nullptr);
      ++cases;
    }
  std::printf(
      "PASS: %u imported-heap cases; gated execution, CPU-polled completion, "
      "prior data visibility, shader words, marker-last payloads, separate/inline ordering and untouched guards.\n",
      cases);
  return 0;
} catch (const std::exception &e) {
  std::fprintf(stderr, "FAIL: %s\n", e.what());
  return 1;
}
