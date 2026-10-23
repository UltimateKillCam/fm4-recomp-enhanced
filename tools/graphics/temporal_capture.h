// Opt-in, paired renderer readback for HUD/scene comparisons.
// Modified 2026-10-17. GPL-3.0-only. Never captures the desktop.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace fm4::temporal_capture {
using Microsoft::WRL::ComPtr;
struct Image {
  ComPtr<ID3D12Resource> buffer;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 size=0;
};
struct Pair {
  std::filesystem::path folder;
  Image input,output;
};
inline std::unique_ptr<Pair> Request(uint32_t frame) {
  const char* root=std::getenv("FM4_CAPTURE_DIR");
  if(!root||!std::getenv("FM4_BACKGROUND")) return {};
  std::error_code error;
  if(!std::filesystem::remove(std::filesystem::path(root)/"temporal-capture.request",error)) return {};
  auto pair=std::make_unique<Pair>();
  pair->folder=std::filesystem::path(root)/("temporal-pair-"+std::to_string(frame));
  std::filesystem::create_directories(pair->folder);
  return pair;
}
// The caller supplies COPY_SOURCE state and retains Image until its submission
// fence completes. This helper neither submits work nor waits for the GPU.
inline void Copy(ID3D12Device* device,ID3D12GraphicsCommandList* list,
                 ID3D12Resource* source,Image& image) {
  auto source_desc=source->GetDesc();
  device->GetCopyableFootprints(&source_desc,0,1,0,&image.footprint,nullptr,nullptr,&image.size);
  D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width=image.size; desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1;
  desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
      D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image.buffer))))
    throw std::runtime_error("Temporal diagnostic readback allocation failed");
  D3D12_TEXTURE_COPY_LOCATION from{},to{};
  from.pResource=source; from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource=image.buffer.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint=image.footprint;
  list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
}
inline void SaveImage(const Image& image,const std::filesystem::path& path) {
  void* pixels=nullptr; D3D12_RANGE range{0,size_t(image.size)};
  if(!image.buffer||FAILED(image.buffer->Map(0,&range,&pixels)))
    throw std::runtime_error("Temporal diagnostic readback map failed");
  std::ofstream output(path,std::ios::binary);
  output.write(static_cast<const char*>(pixels),image.size);
  D3D12_RANGE empty{0,0}; image.buffer->Unmap(0,&empty);
  if(!output) throw std::runtime_error("Temporal diagnostic readback write failed");
}
inline void Save(const Pair& pair) {
  SaveImage(pair.input,pair.folder/"input.bin");
  SaveImage(pair.output,pair.folder/"output.bin");
  std::ofstream meta(pair.folder/"pair.json");
  auto describe=[&](const char* name,const Image& image) {
    const auto& f=image.footprint;
    meta<<'"'<<name<<"\":{\"width\":"<<f.Footprint.Width
        <<",\"height\":"<<f.Footprint.Height<<",\"format\":"<<uint32_t(f.Footprint.Format)
        <<",\"row_pitch\":"<<f.Footprint.RowPitch<<",\"offset\":"<<f.Offset<<'}';
  };
  meta<<"{\"same_frame\":true,"; describe("input",pair.input); meta<<',';
  describe("output",pair.output); meta<<"}\n";
  if(!meta) throw std::runtime_error("Temporal diagnostic metadata write failed");
}
}
