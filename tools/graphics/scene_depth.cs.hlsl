// Decode genuine pinned-ReXGlue ROV depth, before scene-bin clears.
// Address convention: dxbc_translator_om.cpp StartPixelShader_LoadROVParameters.
// Host pixels precede adjacent MSAA samples; depth swaps 40-sample halves per
// row of each 80x16 tile. No image-based depth estimation is performed.
cbuffer Constants : register(b0) {
  uint width, height, scale_x, scale_y;
  uint source_x, source_y, base_tiles, pitch_tiles;
  uint msaa, depth_format;
  uint output_row, output_height, presented_orientation;
};
Buffer<uint> edram : register(t0);
RWTexture2D<float> scene_depth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  uint x = (id.x + source_x * scale_x) << uint(msaa >= 2);
  uint y = (id.y + source_y * scale_y) << uint(msaa >= 1);
  uint tw = 80 * scale_x, th = 16 * scale_y;
  uint tile = ((y / th) * pitch_tiles + x / tw + base_tiles) & 2047;
  uint address = tile * (tw * th) + (y % th) * tw + (x % tw + tw / 2) % tw;
  uint d24 = edram[address] >> 8;
  float depth;
  if (depth_format == 0) {
    depth = float(d24) / 16777215.0;
  } else {
    uint e = d24 >> 20, m = d24 & 0xFFFFF;
    depth = e == 0 ? float(m) * (1.0 / 17179869184.0)
        : asfloat(((e + 112) << 23) | (m << 3));
  }
  uint2 destination = uint2(id.x, id.y + output_row);
  if (presented_orientation != 0) {
    destination = uint2(width - 1, output_height - 1) - destination;
  }
  scene_depth[destination] = depth;
}
