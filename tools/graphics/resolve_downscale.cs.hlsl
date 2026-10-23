// FM4 enhanced renderer: convert the pinned SDK's grouped scaled resolves
// back to guest tiled bytes. Its precompiled shaders use 8-byte groups for
// 8-bit pixels and 16-byte groups for other formats, with one guest row per
// group. This differs from current upstream Xenia's larger 2D groups.
// Each thread writes one complete dword, including 8/16-bit packed texels.
cbuffer Constants : register(b0) {
  uint scale_x;
  uint scale_y;
  uint pixel_size_log2;
  uint byte_count;
  uint half_pixel_offset;
};
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer destination : register(u0);

uint SourceAddress(uint guest_byte) {
  uint b = pixel_size_log2;
  uint group_bytes_log2 = b == 0 ? 3 : 4;
  uint group_x_log2 = group_bytes_log2 - b;
  uint in_group = guest_byte & ((1u << group_bytes_log2) - 1u);
  uint hx = (in_group >> b) * scale_x;
  uint hy = 0;
  if (half_pixel_offset != 0) {
    hx += scale_x >> 1;
    hy += scale_y >> 1;
  }
  uint host_group = (hx >> group_x_log2) * scale_y + hy;
  uint offset = (host_group << group_bytes_log2)
      + ((hx & ((1u << group_x_log2) - 1u)) << b);
  uint component = in_group & ((1u << b) - 1u);
  return (guest_byte & ~((1u << group_bytes_log2) - 1u)) * (scale_x * scale_y)
      + offset + component;
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint byte_offset = id.x * 4;
  if (byte_offset >= byte_count) return;
  uint value = 0;
  [unroll] for (uint i = 0; i < 4; ++i) {
    uint address = SourceAddress(byte_offset + i);
    uint v = (source.Load(address & ~3u) >> ((address & 3u) * 8)) & 255;
    value |= v << (i * 8);
  }
  destination.Store(byte_offset, value);
}
