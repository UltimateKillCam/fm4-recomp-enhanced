// Opt-in guest frame trace. GPL-3.0-only.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>

namespace fm4::frame_trace {
// Only this immutable branch is touched by event hooks when unset. No register
// snapshots, TLS access, allocations, clocks, file I/O or GPU work in that case.
inline const bool enabled = std::getenv("FM4_FRAME_TRACE") != nullptr;

struct Trace {
  uint64_t remaining = 0, frame = 0, sequence = 0, next_draw = 0;
  int64_t draw = -1;
  bool active = false, attempted = false, failed = false;
  std::string phase;
  std::ofstream output;

  Trace() {
    const char* count = std::getenv("FM4_FRAME_TRACE");
    const char* target = std::getenv("FM4_FRAME_TRACE_PHASE");
    if (!count || !target || !*target) return;
    // Reject signs, whitespace, trailing junk and overflow (not an unbounded run).
    for (const char* p = count; *p; ++p) if (*p < '0' || *p > '9') return;
    char* end = nullptr;
    errno = 0;
    remaining = std::strtoull(count, &end, 10);
    if (errno || end == count || *end) remaining = 0;
    phase = target;
    if (phase.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
        std::string::npos) remaining = 0;
  }

  std::ostream& Event(const char* kind) {
    output << "{\"event\":\"" << kind << "\",\"frame\":" << frame
           << ",\"seq\":" << sequence++;
    return output;
  }
  void Check() {
    if (!output && !failed) {
      failed = true;
      active = false;
      remaining = 0;
      REXGPU_ERROR("FM4 frame trace write failed");
    }
  }
  // Called after the metrics phase update at each guest swap. The first matching
  // boundary arms the *following* whole frame, never a phase's partial prefix.
  void Boundary(const std::string& current_phase) {
    if (active) {
      bool complete = current_phase == phase;
      Event("frame_end") << ",\"complete\":" << complete << "}\n";
      output.flush();
      Check();
      if (complete && remaining) --remaining;
      active = false;
    }
    ++frame;
    draw = -1;
    next_draw = 0;
    if (!remaining || current_phase != phase || failed) return;
    if (!attempted) {
      attempted = true;
      const char* directory = std::getenv("FM4_CAPTURE_DIR");
      if (!directory || !*directory) {
        remaining = 0;
        REXGPU_ERROR("FM4 frame trace requires FM4_CAPTURE_DIR");
        return;
      }
      std::error_code error;
      std::filesystem::create_directories(directory, error);
      if (!error) output.open(std::filesystem::path(directory) / "frame-trace.jsonl",
                              std::ios::out | std::ios::trunc);
      output.imbue(std::locale::classic());
      output << std::setprecision(9) << std::noboolalpha;
      if (error || !output) {
        remaining = 0;
        failed = true;
        REXGPU_ERROR("FM4 frame trace could not open capture file");
        return;
      }
    }
    active = true;
    Event("frame_begin") << ",\"schema\":1,\"phase\":\"" << phase << "\"}\n";
  }
};
inline Trace& Get() { static thread_local Trace trace; return trace; }
inline bool Active() { return enabled && Get().active; }
inline void Boundary(const std::string& phase) { if (enabled) Get().Boundary(phase); }
inline void Number(std::ostream& out, float value) {
  if (std::isfinite(value)) out << value; else out << "null";
}

template<class Shader>
void Textures(std::ostream& out, const rex::graphics::RegisterFile& regs,
              const Shader& vertex, const Shader* pixel) {
  using namespace rex::graphics;
  uint32_t masks[32] = {};
  for (const auto& binding : vertex.texture_bindings()) masks[binding.fetch_constant] |= 1;
  if (pixel) for (const auto& binding : pixel->texture_bindings()) masks[binding.fetch_constant] |= 2;
  bool first = true;
  for (uint32_t slot = 0; slot < 32; ++slot) {
    if (!masks[slot]) continue;
    auto fetch = regs.GetTextureFetch(slot);
    uint32_t width = 1, height = 1, depth = 1;
    switch (fetch.dimension) {
      case xenos::DataDimension::k1D: width = fetch.size_1d.width + 1; break;
      case xenos::DataDimension::k2DOrStacked:
      case xenos::DataDimension::kCube:
        width = fetch.size_2d.width + 1;
        height = fetch.size_2d.height + 1;
        depth = fetch.dimension == xenos::DataDimension::kCube ? 6 :
            (fetch.stacked ? fetch.size_2d.stack_depth + 1 : 1);
        break;
      case xenos::DataDimension::k3D:
        width = fetch.size_3d.width + 1;
        height = fetch.size_3d.height + 1;
        depth = fetch.size_3d.depth + 1;
        break;
    }
    uint32_t base_bytes = 0, mip_bytes = 0;
    uint32_t base_page = 0, mip_page = 0, min_level = 0, max_level = 0;
    if (fetch.type == xenos::FetchConstantType::kTexture) {
      texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr,
          &base_page, &mip_page, &min_level, &max_level);
      auto layout = texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch,
          width, height, depth, fetch.tiled, GetBaseFormat(fetch.format), fetch.packed_mips,
          base_page != 0, max_level);
      base_bytes = base_page ? layout.base.level_data_extent_bytes : 0;
      mip_bytes = mip_page ? layout.mips_total_extent_bytes : 0;
    }
    if (!first) out << ',';
    first = false;
    out << "{\"slot\":" << slot << ",\"stages\":" << masks[slot]
        << ",\"type\":" << uint32_t(fetch.type)
        << ",\"base\":" << (uint32_t(fetch.base_address) << 12)
        << ",\"mip\":" << (uint32_t(fetch.mip_address) << 12)
        << ",\"base_physical\":" << (base_page << 12)
        << ",\"mip_physical\":" << (mip_page << 12)
        << ",\"base_bytes\":" << base_bytes << ",\"mip_bytes\":" << mip_bytes
        << ",\"format\":" << uint32_t(fetch.format) << ",\"width\":" << width
        << ",\"height\":" << height << ",\"depth\":" << depth
        << ",\"pitch\":" << (fetch.pitch << 5) << ",\"tiled\":" << fetch.tiled
        << ",\"dimension\":" << uint32_t(fetch.dimension)
        << ",\"mip_min\":" << fetch.mip_min_level << ",\"mip_max\":" << fetch.mip_max_level
        << ",\"packed_mips\":" << fetch.packed_mips << '}';
  }
}

// Lifetime encloses RT Update and submission: transfers still have a draw index
// if the pipeline isn't ready or a later allocation fails. draw_end disambiguates.
struct DrawScope {
  Trace* trace = nullptr;
  bool submitted = false;
  template<class Shader>
  DrawScope(const rex::graphics::RegisterFile& regs, const Shader& vertex,
            const Shader* pixel, const Shader* guest_pixel,
            rex::graphics::xenos::PrimitiveType primitive,
            uint32_t count, uint32_t host_count,
            rex::graphics::reg::RB_DEPTHCONTROL depth_control, uint32_t color_mask) {
    if (!Active()) return;
    using namespace rex::graphics;
    trace = &Get();
    trace->draw = int64_t(trace->next_draw++);
    auto& out = trace->Event("draw");
    out << ",\"index\":" << trace->draw << ",\"primitive\":" << uint32_t(primitive)
        << ",\"count\":" << count << ",\"host_count\":" << host_count
        << ",\"vs\":\"" << vertex.ucode_data_hash() << "\",\"ps\":\""
        << (guest_pixel ? guest_pixel->ucode_data_hash() : 0)
        << "\",\"effective_ps\":\"" << (pixel ? pixel->ucode_data_hash() : 0)
        << "\",\"rt\":{";
    auto surface = regs.Get<reg::RB_SURFACE_INFO>();
    auto depth = regs.Get<reg::RB_DEPTH_INFO>();
    out << "\"pitch\":" << surface.surface_pitch << ",\"msaa\":"
        << (1u << uint32_t(surface.msaa_samples)) << ",\"mode\":"
        << uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode) << ",\"colors\":[";
    for (uint32_t i = 0; i < 4; ++i) {
      if (i) out << ',';
      auto color = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      out << "{\"base\":" << color.color_base << ",\"format\":" << uint32_t(color.color_format)
          << ",\"mask\":" << ((regs[XE_GPU_REG_RB_COLOR_MASK] >> (i * 4)) & 15)
          << ",\"effective_mask\":" << ((color_mask >> (i * 4)) & 15)
          << ",\"raw\":" << color.value << '}';
    }
    out << "],\"depth\":{\"base\":" << depth.depth_base << ",\"format\":"
        << uint32_t(depth.depth_format) << "},\"depth_control\":" << depth_control.value
        << ",\"guest_depth_control\":" << regs[XE_GPU_REG_RB_DEPTHCONTROL]
        << ",\"z_enable\":" << depth_control.z_enable << ",\"z_write\":"
        << depth_control.z_write_enable << ",\"z_func\":" << uint32_t(depth_control.zfunc)
        << ",\"stencil_enable\":" << depth_control.stencil_enable
        << ",\"stencil_backface\":" << depth_control.backface_enable
        << ",\"stencil_front\":[" << uint32_t(depth_control.stencilfunc) << ','
        << uint32_t(depth_control.stencilfail) << ',' << uint32_t(depth_control.stencilzpass)
        << ',' << uint32_t(depth_control.stencilzfail) << "],\"stencil_back\":["
        << uint32_t(depth_control.stencilfunc_bf) << ',' << uint32_t(depth_control.stencilfail_bf)
        << ',' << uint32_t(depth_control.stencilzpass_bf) << ',' << uint32_t(depth_control.stencilzfail_bf)
        << "],\"stencil_mask\":" << regs[XE_GPU_REG_RB_STENCILREFMASK]
        << ",\"stencil_mask_back\":" << regs[XE_GPU_REG_RB_STENCILREFMASK_BF] << "},\"viewport\":[";
    for (uint32_t i = 0; i < 6; ++i) {
      if (i) out << ',';
      Number(out, regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE + i));
    }
    draw_util::Scissor scissor;
    draw_util::GetScissor(regs, scissor);
    out << "],\"viewport_control\":" << regs[XE_GPU_REG_PA_CL_VTE_CNTL]
        << ",\"scissor\":[" << scissor.offset[0] << ',' << scissor.offset[1] << ','
        << scissor.extent[0] << ',' << scissor.extent[1] << "],\"blend\":{";
    out << "\"color_control\":" << regs[XE_GPU_REG_RB_COLORCONTROL] << ",\"targets\":[";
    for (uint32_t i = 0; i < 4; ++i) {
      if (i) out << ',';
      auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
      out << '[' << uint32_t(blend.color_srcblend) << ',' << uint32_t(blend.color_destblend)
          << ',' << uint32_t(blend.color_comb_fcn) << ',' << uint32_t(blend.alpha_srcblend)
          << ',' << uint32_t(blend.alpha_destblend) << ',' << uint32_t(blend.alpha_comb_fcn) << ']';
    }
    out << "],\"constant\":[";
    for (uint32_t i = 0; i < 4; ++i) {
      if (i) out << ',';
      Number(out, regs.Get<float>(XE_GPU_REG_RB_BLEND_RED + i));
    }
    out << "]},\"textures\":[";
    Textures(out, regs, vertex, pixel);
    out << "]}\n";
    trace->Check();
  }
  ~DrawScope() {
    if (!trace) return;
    trace->Event("draw_end") << ",\"index\":" << trace->draw
                             << ",\"submitted\":" << submitted << "}\n";
    trace->draw = -1;
    trace->Check();
  }
};

template<class Key>
std::string Surface(const Key& key) {
  std::ostringstream out;
  out << "{\"key\":" << key.key << ",\"base\":" << key.base_tiles
      << ",\"pitch_tiles\":" << key.GetPitchTiles() << ",\"msaa\":"
      << (1u << uint32_t(key.msaa_samples)) << ",\"depth\":" << key.is_depth
      << ",\"format\":" << key.resource_format << '}';
  return out.str();
}

struct TransferBatch {
  struct Summary {
    std::string source, dest, host_depth;
    uint64_t samples = 0, portions = 0;
    std::set<std::pair<uint32_t, uint32_t>> tiles;
  };
  std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, Summary> summaries;
  bool active = Active();
  bool resolve_clear;
  explicit TransferBatch(bool clear) : resolve_clear(clear) {}
  template<class Key>
  void Add(const Key& source, const Key& dest, uint32_t mode, uint64_t samples,
           uint32_t start_tiles, uint32_t end_tiles, const Key& host_depth,
           bool has_host_depth) {
    auto& summary = summaries[{source.key, dest.key, mode, has_host_depth ? host_depth.key : 0}];
    if (summary.source.empty()) {
      summary.source = Surface(source);
      summary.dest = Surface(dest);
      summary.host_depth = has_host_depth ? Surface(host_depth) : "null";
    }
    summary.samples += samples;
    ++summary.portions;
    summary.tiles.emplace(start_tiles, end_tiles);
  }
  ~TransferBatch() {
    if (!active) return;
    auto& trace = Get();
    for (const auto& entry : summaries) {
      const auto& summary = entry.second;
      auto& out = trace.Event("ownership");
      out << ",\"draw\":";
      if (trace.draw < 0) out << "null"; else out << trace.draw;
      out << ",\"resolve_clear\":" << resolve_clear << ",\"mode\":" << std::get<2>(entry.first)
          << ",\"source\":" << summary.source << ",\"dest\":" << summary.dest
          << ",\"host_depth_source\":" << summary.host_depth
          << ",\"host_samples\":" << summary.samples
          << ",\"source_portions\":" << summary.portions << ",\"tiles\":[";
      bool first = true;
      for (const auto& tiles : summary.tiles) {
        if (!first) out << ',';
        first = false;
        out << '[' << tiles.first << ',' << tiles.second << ']';
      }
      out << "]}\n";
    }
    trace.Check();
  }
};

inline void Resolve(const rex::graphics::draw_util::ResolveInfo& info,
                    const rex::graphics::RegisterFile& regs, bool copied, bool cleared) {
  if (!Active()) return;
  using namespace rex::graphics;
  auto& trace = Get();
  const auto edram = info.IsCopyingDepth() ? info.depth_edram_info : info.color_edram_info;
  uint32_t base, width, rows, pitch;
  info.GetCopyEdramTileSpan(base, width, rows, pitch);
  auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  auto& out = trace.Event("resolve");
  out << ",\"copied\":" << copied << ",\"cleared\":" << cleared
      << ",\"depth\":" << info.IsCopyingDepth() << ",\"source\":{\"base\":" << edram.base_tiles
      << ",\"format\":" << edram.format << ",\"msaa\":" << (1u << uint32_t(edram.msaa_samples))
      << ",\"pitch_tiles\":" << edram.pitch_tiles << ",\"rect\":["
      << info.coordinate_info.edram_offset_x_div_8 * 8 << ','
      << info.coordinate_info.edram_offset_y_div_8 * 8 << ','
      << info.coordinate_info.width_div_8 * 8 << ',' << info.height_div_8 * 8
      << "],\"tile_span\":[" << base << ',' << width << ',' << rows << ',' << pitch
      << "]},\"dest\":{\"base\":" << info.copy_dest_base
      << ",\"address\":" << info.copy_dest_extent_start << ",\"length\":" << info.copy_dest_extent_length
      << ",\"pitch\":" << dest_pitch.copy_dest_pitch << ",\"height\":" << dest_pitch.copy_dest_height
      << ",\"format\":" << uint32_t(info.copy_dest_info.copy_dest_format)
      << ",\"offset\":[" << info.copy_dest_coordinate_info.offset_x_div_8 * 8 << ','
      << info.copy_dest_coordinate_info.offset_y_div_8 * 8
      << "]},\"clear\":{\"depth\":" << info.IsClearingDepth() << ",\"color\":" << info.IsClearingColor()
      << ",\"depth_value\":" << info.rb_depth_clear << ",\"color_value\":" << info.rb_color_clear
      << ",\"color_value_lo\":" << info.rb_color_clear_lo
      << ",\"depth_base\":" << info.depth_original_base
      << ",\"color_base\":" << info.color_original_base;
  for (uint32_t i = 0; i < 2; ++i) {
    if (!(i ? info.IsClearingColor() : info.IsClearingDepth())) continue;
    const auto clear_edram = i ? info.color_edram_info : info.depth_edram_info;
    uint32_t clear_base, clear_width, clear_rows;
    draw_util::GetResolveEdramTileSpan(clear_edram, info.coordinate_info,
        info.height_div_8, clear_base, clear_width, clear_rows);
    out << (i ? ",\"color_tile_span\":[" : ",\"depth_tile_span\":[")
        << clear_base << ',' << clear_width << ',' << clear_rows << ','
        << clear_edram.pitch_tiles << ']';
  }
  out << "},\"copy_control\":" << info.rb_copy_control.value
      << ",\"dest_info\":" << info.copy_dest_info.value << "}\n";
  trace.Check();
}

// Presentation reads fetch 0, rather than necessarily frontbuffer_ptr. Record it
// before Boundary closes the producing frame, so it isn't a shadow candidate.
inline void Present(const rex::graphics::RegisterFile& regs) {
  if (!Active()) return;
  using namespace rex::graphics;
  const auto fetch = regs.GetTextureFetch(0);
  uint32_t base_size = 0, mip_size = 0;
  uint32_t width = 0, height = 0, depth = 0, base = 0, mip = 0, lo = 0, hi = 0;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, &width, &height, &depth, &base, &mip, &lo, &hi);
  // Approximate base level size: 32bpp rows at the fetch pitch (enough to match ranges).
  base_size = uint32_t(fetch.pitch) * 32u * 4u * (height + 1);
  (void)mip_size; (void)depth; (void)lo; (void)hi; (void)mip;
  auto& trace = Get();
  trace.Event("present") << ",\"address\":" << (base << 12)
      << ",\"length\":" << (base ? base_size : 0) << "}\n";
  trace.Check();
}
}  // namespace fm4::frame_trace
