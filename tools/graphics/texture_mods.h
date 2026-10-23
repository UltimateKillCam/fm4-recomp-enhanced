#pragma once
// Optional, content-addressed BC1/2/3 and RGBA8 colour replacements. Guest game
// data and original GPU resources are retained, including on streaming reuse.
#include <d3d12.h>
#include <wrl/client.h>
#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
namespace fm4::textures {
using Microsoft::WRL::ComPtr;
inline std::atomic<uint64_t> allocated_bytes{};
inline uint32_t BlockBytes(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : format == DXGI_FORMAT_BC1_UNORM ? 8 :
      (format == DXGI_FORMAT_BC2_UNORM || format == DXGI_FORMAT_BC3_UNORM ? 16 : 0);
}
inline uint32_t BlockWidth(DXGI_FORMAT format) { return format == DXGI_FORMAT_R8G8B8A8_UNORM ? 1 : 4; }
inline bool Enabled() {
  static bool enabled = std::getenv("FM4_TEXTURE_DUMP") || std::getenv("FM4_TEXTURE_PACK");
  return enabled;
}
inline bool RgbaEnabled() {
  // FM4 also uses mipped RGBA8 resources for frequently changing GPU data.
  // Keep those out of colour dumps and the normal replacement lookup path.
  static bool enabled = std::getenv("FM4_TEXTURE_RGBA8") != nullptr;
  return enabled;
}
inline std::string Identity(uint32_t w, uint32_t h, DXGI_FORMAT format,
                             const std::vector<uint8_t>& base) {
  std::ostringstream out;
  const char* name = format == DXGI_FORMAT_R8G8B8A8_UNORM ? "rgba8" : format == DXGI_FORMAT_BC1_UNORM ? "bc1" : format == DXGI_FORMAT_BC2_UNORM ? "bc2" : "bc3";
  out << name << '-' << w << 'x' << h << '-' << std::hex << std::setw(16) << std::setfill('0')
      << XXH64(base.data(), base.size(), 0x464D3401);
  return out.str();
}
// Capture the full original chain so material masks can be retained at every
// distance. The caller appends tightly packed native mips only when a new dump
// is requested; normal gameplay and previously seen textures do no extra work.
template <typename AppendMips>
inline uint32_t Dump(const std::string& id, uint32_t w, uint32_t h, DXGI_FORMAT format,
                     const std::vector<uint8_t>& base, AppendMips&& append_mips) {
  static const char* folder = std::getenv("FM4_TEXTURE_DUMP");
  if (!folder || !*folder) return 0;
  static std::unordered_set<std::string> seen;
  if (seen.size() >= 512 || !seen.insert(id).second) return 0;
  try {
    std::filesystem::create_directories(folder);
    auto path = std::filesystem::path(folder) / (id + ".dds");
    if (std::filesystem::exists(path)) return 0;
    std::vector<uint8_t> data(base);
    const uint32_t mip_count = append_mips(data);
    if (!mip_count || mip_count > std::bit_width(std::max(w, h))) return 0;
    size_t expected = 0;
    for (uint32_t mip = 0; mip < mip_count; ++mip) {
      const uint32_t block = BlockWidth(format);
      expected += size_t((std::max(w >> mip, 1u) + block - 1) / block) *
          ((std::max(h >> mip, 1u) + block - 1) / block) * BlockBytes(format);
    }
    if (data.size() != expected) return 0;
    uint32_t header[32]{};
    header[0] = 0x20534444; header[1] = 124; header[2] = 0x81007;
    header[3] = h; header[4] = w; header[5] = uint32_t(base.size()); header[7] = mip_count;
    header[19] = 32; header[20] = 4;
    header[21] = format == DXGI_FORMAT_BC1_UNORM ? 0x31545844 : format == DXGI_FORMAT_BC2_UNORM ? 0x33545844 : 0x35545844;
    header[27] = 0x1000;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
      header[2] = 0x100F; header[5] = w * 4; header[20] = 0x41; header[21] = 0;
      header[22] = 32; header[23] = 255; header[24] = 0xFF00;
      header[25] = 0xFF0000; header[26] = 0xFF000000;
    }
    if (mip_count > 1) { header[2] |= 0x20000; header[27] |= 0x400008; }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(header), sizeof(header));
    output.write(reinterpret_cast<const char*>(data.data()), data.size());
    return output ? mip_count : 0;
  } catch (...) { /* A missing dump directory must never stop the game. */ }
  return 0;
}
struct Replacement {
  ComPtr<ID3D12Resource> resource, upload;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
  std::string identity;
  uint32_t mip_count{};
  uint64_t accounted_bytes{};
  ~Replacement() {
    resource.Reset(); upload.Reset();
    if (accounted_bytes) allocated_bytes.fetch_sub(accounted_bytes);
  }
};
inline std::unique_ptr<Replacement> Load(ID3D12Device* device, const std::filesystem::path& path,
                                         const std::string& id,
                                         uint32_t original_w, uint32_t original_h,
                                         DXGI_FORMAT format) {
  try {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    auto length = input.tellg();
    if (length < 128 || length > 64 * 1024 * 1024) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input) return {};
    auto u32 = [&](size_t offset) { uint32_t v; std::memcpy(&v, bytes.data() + offset, 4); return v; };
    if (u32(0) != 0x20534444 || u32(4) != 124 || u32(76) != 32) return {};
    uint32_t w = u32(16), h = u32(12), mips = std::max(u32(28), 1u), data_offset = 128;
    uint32_t fourcc = u32(84);
    DXGI_FORMAT actual = fourcc == 0x31545844 ? DXGI_FORMAT_BC1_UNORM :
        fourcc == 0x33545844 ? DXGI_FORMAT_BC2_UNORM : fourcc == 0x35545844 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_UNKNOWN;
    if ((u32(80) & 0x41) == 0x41 && u32(88) == 32 && u32(92) == 255 &&
        u32(96) == 0xFF00 && u32(100) == 0xFF0000 && u32(104) == 0xFF000000)
      actual = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (fourcc == 0x30315844) {
      if (bytes.size() < 148 || u32(132) != 3 || u32(136) != 0 || u32(140) != 1) return {};
      actual = DXGI_FORMAT(u32(128)); data_offset = 148;
    }
    if (actual != format || !BlockBytes(format) || !original_w || !original_h ||
        !w || !h || w > 4096 || h > 4096 ||
        w % original_w || h % original_h || w / original_w != h / original_h ||
        (w / original_w != 2 && w / original_w != 4) || mips != std::bit_width(std::max(w, h))) return {};
    uint64_t total = 0;
    uint32_t block = BlockWidth(format);
    for (uint32_t mip = 0; mip < mips; ++mip)
      total += uint64_t((std::max(w >> mip, 1u) + block - 1) / block) *
               ((std::max(h >> mip, 1u) + block - 1) / block) * BlockBytes(format);
    if (data_offset + total != bytes.size()) return {};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = w; desc.Height = h;
    desc.DepthOrArraySize = 1; desc.MipLevels = uint16_t(mips); desc.Format = format; desc.SampleDesc.Count = 1;
    auto result = std::make_unique<Replacement>(); result->layouts.resize(mips); result->mip_count = mips; result->identity = id;
    std::vector<UINT> rows(mips); std::vector<UINT64> row_bytes(mips); UINT64 upload_size;
    device->GetCopyableFootprints(&desc, 0, mips, 0, result->layouts.data(), rows.data(), row_bytes.data(), &upload_size);
    auto allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    D3D12_RESOURCE_DESC upload_desc{}; upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_size; upload_desc.Height = 1; upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1; upload_desc.SampleDesc.Count = 1; upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto upload_allocation = device->GetResourceAllocationInfo(0, 1, &upload_desc).SizeInBytes;
    constexpr uint64_t budget = 512ull << 20;
    if (!upload_size || allocation > budget || upload_allocation > budget) return {};
    uint64_t cost = allocation + upload_allocation;
    if (cost > budget) return {};
    // Reserve before allocating so other cache workers cannot oversubscribe.
    uint64_t used = allocated_bytes.load();
    do {
      if (used > budget - cost) return {};
    } while (!allocated_bytes.compare_exchange_weak(used, used + cost));
    result->accounted_bytes = cost;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result->resource)))) return {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&result->upload)))) return {};
    void* mapped; D3D12_RANGE empty{0, 0};
    if (FAILED(result->upload->Map(0, &empty, &mapped))) return {};
    size_t source_offset = data_offset;
    for (uint32_t mip = 0; mip < mips; ++mip) for (uint32_t row = 0; row < rows[mip]; ++row) {
      std::memcpy(static_cast<uint8_t*>(mapped) + result->layouts[mip].Offset + size_t(row) * result->layouts[mip].Footprint.RowPitch,
                  bytes.data() + source_offset, row_bytes[mip]); source_offset += row_bytes[mip];
    }
    result->upload->Unmap(0, nullptr);
    return result;
  } catch (...) { return {}; }
}

class Loader {
 public:
  struct Completed {
    std::string identity;
    std::shared_ptr<Replacement> replacement;
  };

  Loader(ID3D12Device* device, const char* folder) : device_(device) {
    // Snapshot the pack at cache initialization, never probe misses during play.
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
      auto extension = entry.path().extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (extension == ".dds" && entry.is_regular_file())
        paths_.emplace(entry.path().stem().string(), entry.path());
    }
    if (!paths_.empty()) worker_ = std::thread([this] { Run(); });
  }
  ~Loader() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      requests_.clear();
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
  }

  bool Contains(const std::string& identity) const { return paths_.contains(identity); }
  bool Queue(const std::string& identity, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    auto path = paths_.find(identity);
    if (path == paths_.end()) return false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return false;
      requests_.push_back({identity, path->second, width, height, format});
    }
    wake_.notify_one();
    return true;
  }
  bool TryPop(Completed& completed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_.empty()) return false;
    completed = std::move(completed_.front());
    completed_.pop_front();
    return true;
  }

 private:
  struct Request {
    std::string identity;
    std::filesystem::path path;
    uint32_t width, height;
    DXGI_FORMAT format;
  };
  void Run() {
    try {
      for (;;) {
        Request request;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          wake_.wait(lock, [this] { return stopping_ || !requests_.empty(); });
          if (stopping_) return;
          request = std::move(requests_.front());
          requests_.pop_front();
        }
        auto replacement = Load(device_.Get(), request.path, request.identity,
                                request.width, request.height, request.format);
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        completed_.push_back({std::move(request.identity), std::move(replacement)});
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      requests_.clear();
    }
  }
  ComPtr<ID3D12Device> device_;
  std::unordered_map<std::string, std::filesystem::path> paths_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Request> requests_;
  std::deque<Completed> completed_;
  bool stopping_ = false;
  std::thread worker_;
};
} // namespace fm4::textures
