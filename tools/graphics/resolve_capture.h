#pragma once
// One-frame, opt-in resolve readback evidence. No desktop capture is involved.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <fstream>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/register_file.h>
namespace fm4::resolve_capture {
inline bool active = false;
inline uint32_t serial = 0, number = 0;
inline uint64_t bytes_written = 0;
inline uint32_t draw_number = 0;
inline uint32_t pending_frames = 0;
inline std::filesystem::path folder;
inline std::filesystem::path completed_folder;
template<class Shader>
inline void RecordDraw(const rex::graphics::RegisterFile& regs, const Shader& vertex,
                       const Shader* pixel, uint32_t count) {
  if (!active || !std::getenv("FM4_CAPTURE_ROV_DEPTH") || draw_number >= 2048) return;
  using namespace rex::graphics;
  const auto index = draw_number++;
  const std::string constants_name = "draw-" + std::to_string(index) + ".f32";
  std::ofstream constants(folder / constants_name, std::ios::binary);
  constants.write(reinterpret_cast<const char*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]), 512 * 4 * 4);
  const std::string fetch_name = "draw-" + std::to_string(index) + ".fetch";
  std::ofstream fetches(folder / fetch_name, std::ios::binary);
  fetches.write(reinterpret_cast<const char*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0]), 32 * 6 * 4);
  std::ofstream metadata(folder / "scene-draws.jsonl", std::ios::app);
  metadata << "{\"draw\":" << index << ",\"vertex_shader\":\"" << vertex.ucode_data_hash()
      << "\",\"pixel_shader\":\"" << (pixel ? pixel->ucode_data_hash() : 0)
      << "\",\"count\":" << count << ",\"surface\":" << regs[XE_GPU_REG_RB_SURFACE_INFO]
      << ",\"depth_control\":" << regs[XE_GPU_REG_RB_DEPTHCONTROL]
      << ",\"depth_info\":" << regs[XE_GPU_REG_RB_DEPTH_INFO]
      << ",\"colour_info\":" << regs[XE_GPU_REG_RB_COLOR_INFO]
      << ",\"fetches\":\"" << fetch_name << "\",\"viewport\":[";
  for (unsigned i = 0; i < 6; ++i) {
    if (i) metadata << ',';
    metadata << regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE + i);
  }
  metadata << "],\"boolean_constants\":[";
  for (unsigned i = 0; i < 8; ++i) {
    if (i) metadata << ',';
    metadata << regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i];
  }
  metadata << "],\"constants\":\"" << constants_name << "\"}\n";
  const auto shader_path = folder / ("vertex-" + std::to_string(vertex.ucode_data_hash()) + ".txt");
  if (!std::filesystem::exists(shader_path)) {
    std::ofstream disassembly(shader_path); disassembly << vertex.ucode_disassembly();
  }
  if (pixel) {
    const auto pixel_path = folder / ("pixel-" + std::to_string(pixel->ucode_data_hash()) + ".txt");
    if (!std::filesystem::exists(pixel_path)) {
      std::ofstream disassembly(pixel_path); disassembly << pixel->ucode_disassembly();
    }
  }
}
struct RawDepth {
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  rex::graphics::draw_util::ResolveInfo info;
  uint64_t size;
};
inline std::vector<RawDepth> raw_depth;
struct ColourTexture {
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  Microsoft::WRL::ComPtr<ID3D12Resource> resolved;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
  uint64_t size;
  uint32_t width, height, mips, faces, bytes_per_pixel;
  ID3D12Resource* source = nullptr;
  std::string name, format;
};
inline std::vector<ColourTexture> colour_textures;
template<class Processor>
inline void RecordColourTexture(Processor& cp, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES resource_state) {
  const bool rtv = std::getenv("FM4_CAPTURE_RTV_COLOR") != nullptr;
  if (!active || (!rtv && !std::getenv("FM4_CAPTURE_ROV_DEPTH")) || !resource || colour_textures.size() >= 5) return;
  const auto desc = resource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Width < 256 || desc.Width > 2048 || desc.Height > 2048) return;
  for (const auto& prior : colour_textures) if (prior.source == resource) return;
  if (desc.SampleDesc.Count != 1) {
    // Only standard hardware-resolvable typed colour formats. Do not average
    // integer/raw or depth surfaces, or reinterpret their bit representations.
    if (!rtv || desc.DepthOrArraySize != 1 ||
        (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
         desc.Format != DXGI_FORMAT_R16G16B16A16_UNORM &&
         desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)) return;
    auto single = desc; single.SampleDesc = {1, 0}; single.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resolved;
    if (FAILED(cp.GetD3D12Provider().GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &single, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)))) return;
    cp.PushTransitionBarrier(resource, resource_state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE); cp.SubmitBarriers();
    cp.GetDeferredCommandList().D3DResolveSubresource(resolved.Get(), 0, resource, 0, desc.Format);
    cp.PushTransitionBarrier(resource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, resource_state);
    cp.PushTransitionBarrier(resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cp.SubmitBarriers();
    const size_t count = colour_textures.size();
    RecordColourTexture(cp, resolved.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (colour_textures.size() != count) {
      colour_textures.back().resolved = std::move(resolved);
      colour_textures.back().source = resource;
    } else {
      // Even a failed readback allocation must retain the already recorded
      // resolve destination until IssueSwap waits for this diagnostic frame.
      ColourTexture retained{}; retained.resolved = std::move(resolved);
      retained.source = resource; colour_textures.push_back(std::move(retained));
    }
    return;
  }
  const bool cube = desc.DepthOrArraySize == 6 &&
      (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
  const bool hdr = desc.DepthOrArraySize == 1 &&
      (desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS || desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
       (rtv && desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM));
  const bool flat_colour = rtv && desc.DepthOrArraySize == 1 &&
      (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
       desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
  if (!cube && !hdr && !flat_colour) return;
  if (!rtv && hdr && std::count_if(colour_textures.begin(), colour_textures.end(),
                         [](const auto& prior) { return prior.faces == 1; }) >= 2) return;
  for (const auto& prior : colour_textures)
    if (prior.source == resource || (cube && prior.faces == 6)) return;
  auto* device = cp.GetD3D12Provider().GetDevice();
  ColourTexture snapshot{}; snapshot.width = uint32_t(desc.Width); snapshot.height = desc.Height; snapshot.mips = desc.MipLevels;
  snapshot.faces = desc.DepthOrArraySize; snapshot.bytes_per_pixel = cube || flat_colour ? 4 : 8;
  snapshot.source = resource; snapshot.name = rtv ? "colour-rtv-" + std::to_string(colour_textures.size()) : cube ? "colour-cube" : "colour-hdr-" + std::to_string(colour_textures.size());
  snapshot.format = cube || flat_colour ? "RGBA8" : "RGBA16F";
  if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) snapshot.format = "RGB10A2";
  if (desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM) snapshot.format = "RGBA16_UNORM";
  snapshot.footprints.resize(snapshot.faces * desc.MipLevels);
  for (unsigned subresource = 0; subresource < snapshot.footprints.size(); ++subresource) {
    uint64_t bytes;
    const auto offset = (snapshot.size + 511) & ~uint64_t(511);
    device->GetCopyableFootprints(&desc, subresource, 1, offset,
                                 &snapshot.footprints[subresource], nullptr, nullptr, &bytes);
    snapshot.size = snapshot.footprints[subresource].Offset +
                    uint64_t(snapshot.footprints[subresource].Footprint.RowPitch) * snapshot.footprints[subresource].Footprint.Height;
  }
  if (snapshot.size > (64ull << 20)) return;
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = snapshot.size; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&snapshot.readback)))) return;
  cp.PushTransitionBarrier(resource, resource_state, D3D12_RESOURCE_STATE_COPY_SOURCE); cp.SubmitBarriers();
  for (unsigned subresource = 0; subresource < snapshot.footprints.size(); ++subresource) {
    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
    source.pResource = resource; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;
    target.pResource = snapshot.readback.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = snapshot.footprints[subresource];
    cp.GetDeferredCommandList().D3DCopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
  }
  cp.PushTransitionBarrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, resource_state); cp.SubmitBarriers();
  colour_textures.push_back(std::move(snapshot));
}
inline bool flushed_raw_depth = false;
inline void FlushRawDepth() {
  for (auto& texture : colour_textures) {
    if (!texture.readback) continue;
    void* data = nullptr; D3D12_RANGE range{0, size_t(texture.size)};
    if (FAILED(texture.readback->Map(0, &range, &data))) continue;
    std::ofstream output(folder / (texture.name + (texture.bytes_per_pixel == 4 ? ".rgba8" : ".rgba16f")), std::ios::binary);
    for (const auto& part : texture.footprints) for (unsigned y = 0; y < part.Footprint.Height; ++y)
      output.write(static_cast<const char*>(data) + part.Offset + y * part.Footprint.RowPitch, part.Footprint.Width * texture.bytes_per_pixel);
    D3D12_RANGE empty{0, 0}; texture.readback->Unmap(0, &empty);
    std::ofstream metadata(folder / (texture.name + ".json"));
    metadata << "{\"width\":" << texture.width << ",\"height\":" << texture.height << ",\"faces\":" << texture.faces << ",\"mips\":" << texture.mips << ",\"order\":\"face-then-mip\",\"format\":\"" << texture.format << "\"}\n";
  }
  colour_textures.clear();
  unsigned index = 0;
  for (auto& snapshot : raw_depth) {
    void* mapped = nullptr;
    D3D12_RANGE range{0, size_t(snapshot.size)};
    if (FAILED(snapshot.readback->Map(0, &range, &mapped))) continue;
    auto name = "edram-depth-" + std::to_string(index++) + ".bin";
    std::ofstream output(folder / name, std::ios::binary);
    output.write(static_cast<const char*>(mapped), snapshot.size);
    D3D12_RANGE empty{0, 0}; snapshot.readback->Unmap(0, &empty);
    const auto& info = snapshot.info;
    const auto& depth = info.depth_edram_info;
    std::ofstream meta(folder / "edram-depth.jsonl", std::ios::app);
    meta << "{\"file\":\"" << name << "\",\"length\":" << snapshot.size <<
        ",\"width\":" << info.coordinate_info.width_div_8 * 8 <<
        ",\"height\":" << info.height_div_8 * 8 <<
        ",\"scale_x\":" << info.coordinate_info.draw_resolution_scale_x <<
        ",\"scale_y\":" << info.coordinate_info.draw_resolution_scale_y <<
        ",\"source_x\":" << info.coordinate_info.edram_offset_x_div_8 * 8 <<
        ",\"source_y\":" << info.coordinate_info.edram_offset_y_div_8 * 8 <<
        ",\"base_tiles\":" << depth.base_tiles << ",\"pitch_tiles\":" << depth.pitch_tiles <<
        ",\"msaa\":" << uint32_t(depth.msaa_samples) << ",\"format\":" << depth.format <<
        ",\"colour_dest_base\":" << info.copy_dest_base <<
        ",\"colour_edram_info\":" << info.color_edram_info.packed << "}\n";
  }
  raw_depth.clear();
}
inline bool WantsRawDepth(const rex::graphics::draw_util::ResolveInfo& info) {
  return active && std::getenv("FM4_CAPTURE_ROV_DEPTH") && info.IsClearingDepth() &&
      !info.IsCopyingDepth() && info.coordinate_info.width_div_8 == 160 &&
      info.height_div_8 >= 16 && info.height_div_8 <= 90 && raw_depth.size() < 4;
}
template<class CommandList>
inline void RecordRawDepth(ID3D12Device* device, CommandList& list,
                           ID3D12Resource* edram, const rex::graphics::draw_util::ResolveInfo& info) {
  if (!WantsRawDepth(info) || !edram) return;
  const auto size = edram->GetDesc().Width;
  if (!size || size > (128ull << 20)) return;
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1;
  desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  RawDepth snapshot{{}, info, size};
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&snapshot.readback)))) return;
  list.D3DCopyBufferRegion(snapshot.readback.Get(), 0, edram, 0, size);
  raw_depth.push_back(std::move(snapshot));
}
inline void FrameBoundary() {
  // Caller has completed GPU submission before mapping these readback buffers.
  flushed_raw_depth = !raw_depth.empty() || !colour_textures.empty();
  completed_folder = folder;
  FlushRawDepth();
  active = false;
  const char* root = std::getenv("FM4_CAPTURE_DIR");
  if (!root || !std::getenv("FM4_BACKGROUND")) return;
  std::error_code error;
  auto request = std::filesystem::path(root) / "resolve.request";
  if (std::filesystem::exists(request, error)) {
    // A small explicit integer requests consecutive frames for motion tests.
    // Legacy request text still captures one frame. Never run an unbounded dump.
    unsigned count = 1;
    std::ifstream input(request);
    if (!(input >> count) || count < 1 || count > 8) count = 1;
    input.close();
    if (std::filesystem::remove(request, error)) pending_frames = count;
  }
  if (!pending_frames) return;
  --pending_frames;
  folder = std::filesystem::path(root) / ("depth-resolves-" + std::to_string(++serial));
  std::filesystem::create_directories(folder);
  active = true; number = 0; bytes_written = 0; draw_number = 0;
}
inline bool Wants(const rex::graphics::draw_util::ResolveInfo& info) {
  return active && info.coordinate_info.width_div_8 >= 16 &&
         info.height_div_8 >= 16 && bytes_written < (32ull << 20);
}
inline void Write(const rex::graphics::draw_util::ResolveInfo& info,
                  const uint8_t* bytes, uint32_t address, uint32_t length) {
  if (!Wants(info) || !bytes || !length || length > (8u << 20)) return;
  auto name = "resolve-" + std::to_string(number++) + ".bin";
  std::ofstream output(folder / name, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes), length);
  if (!output) return;
  bytes_written += length;
  std::ofstream meta(folder / "metadata.jsonl", std::ios::app);
  meta << "{\"file\":\"" << name << "\",\"address\":" << address <<
      ",\"length\":" << length << ",\"base\":" << info.copy_dest_base <<
      ",\"width\":" << info.coordinate_info.width_div_8 * 8 <<
      ",\"height\":" << info.height_div_8 * 8 <<
      ",\"pitch\":" << info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32 <<
      ",\"texture_height\":" << info.copy_dest_coordinate_info.height_aligned_div_32 * 32 <<
      ",\"offset_x\":" << info.copy_dest_coordinate_info.offset_x_div_8 * 8 <<
      ",\"offset_y\":" << info.copy_dest_coordinate_info.offset_y_div_8 * 8 <<
      ",\"dest_info\":" << info.copy_dest_info.value <<
      ",\"is_depth\":" << (info.IsCopyingDepth() ? "true" : "false") <<
      ",\"edram_info\":" << info.depth_edram_info.packed <<
      ",\"colour_edram_info\":" << info.color_edram_info.packed <<
      ",\"coordinate_info\":" << info.coordinate_info.packed <<
      ",\"dest_coordinate_info\":" << info.copy_dest_coordinate_info.packed << "}\n";
}
}  // namespace fm4::resolve_capture
