// FM4 guest output resample with an area filter for downscaling. Modified 2026-10-21.
// GPL-3.0-only. Drop-in replacement for the SDK's guest_output_bilinear(_dither) pixel
// shaders: same root constants, texture and sampler. Upscaling and 1:1 copies take the
// original single bilinear tap. When the source is larger than the output (supersampling,
// e.g. 3840x2160 shown at 2560x1440) one tap per pixel skips source texels and makes fine
// texture such as tarmac shimmer, so bilinear taps are spread evenly over the source area
// each output pixel covers and averaged (2-4 taps per axis).
// Build: fxc /nologo /T ps_5_1 /E main /O3 [/D DITHER=1] /Vn <name> /Fh <header> this file

cbuffer push_consts_xe : register(b0) {
  int2 xe_bilinear_output_offset;
  float2 xe_bilinear_output_size_inv;
};
SamplerState xe_bilinear_sampler : register(s0);
Texture2D<float4> xe_bilinear_source : register(t0);

#if DITHER
// The SDK's 16x16 dither pattern: (k - 127.5) / (255 * 256) for a permutation k of 0..255.
static const uint kDither[256] = {
    62, 185, 238, 26, 211, 88, 197, 37, 216, 161, 207, 23, 166, 109, 200, 35,
    222, 92, 136, 49, 156, 59, 168, 96, 250, 56, 121, 93, 221, 1, 82, 130,
    174, 42, 199, 108, 242, 18, 224, 125, 179, 8, 147, 192, 51, 139, 235, 24,
    150, 249, 12, 71, 186, 138, 67, 28, 85, 212, 75, 32, 245, 72, 183, 98,
    58, 118, 143, 220, 89, 115, 232, 194, 154, 114, 231, 170, 123, 155, 20, 217,
    188, 84, 205, 33, 165, 4, 177, 43, 253, 19, 60, 95, 6, 206, 106, 38,
    234, 7, 175, 68, 246, 53, 103, 134, 79, 204, 132, 225, 191, 65, 254, 162,
    131, 46, 110, 228, 127, 148, 215, 13, 160, 50, 178, 40, 145, 31, 119, 77,
    213, 153, 190, 97, 27, 195, 83, 237, 193, 100, 244, 112, 87, 236, 182, 15,
    57, 243, 76, 11, 172, 63, 41, 116, 29, 149, 0, 171, 22, 203, 157, 99,
    140, 30, 202, 133, 251, 219, 164, 135, 210, 90, 64, 214, 142, 78, 39, 223,
    184, 104, 163, 54, 111, 91, 9, 240, 52, 176, 230, 120, 48, 247, 129, 2,
    241, 81, 229, 16, 208, 141, 196, 80, 21, 107, 159, 10, 189, 105, 173, 61,
    25, 126, 44, 180, 69, 34, 167, 122, 187, 255, 36, 86, 226, 74, 209, 144,
    227, 201, 151, 102, 252, 218, 55, 233, 70, 137, 198, 124, 146, 17, 45, 94,
    117, 5, 73, 169, 128, 3, 113, 152, 14, 101, 47, 239, 66, 181, 248, 158};
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  int2 pixel = int2(position.xy) - xe_bilinear_output_offset;
  float2 uv = (float2(pixel) + 0.5) * xe_bilinear_output_size_inv;
  float source_width, source_height;
  xe_bilinear_source.GetDimensions(source_width, source_height);
  // Source texels covered by one output pixel along each axis.
  float2 ratio = float2(source_width, source_height) * xe_bilinear_output_size_inv;
  float3 color;
  [branch] if (max(ratio.x, ratio.y) > 1.01) {
    uint2 taps = min(uint2(ceil(ratio)) + 1u, uint2(4u, 4u));
    float2 step = xe_bilinear_output_size_inv / float2(taps);
    float2 start = uv - 0.5 * xe_bilinear_output_size_inv + 0.5 * step;
    color = 0.0;
    [loop] for (uint y = 0u; y < taps.y; ++y) {
      [loop] for (uint x = 0u; x < taps.x; ++x) {
        color += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, start + step * float2(x, y), 0.0).rgb;
      }
    }
    color /= float(taps.x * taps.y);
  } else {
    color = xe_bilinear_source.SampleLevel(xe_bilinear_sampler, uv, 0.0).rgb;
  }
#if DITHER
  uint index = (uint(pixel.x) & 15u) | ((uint(pixel.y) & 15u) << 4u);
  color = saturate(color + (float(kDither[index]) - 127.5) / 65280.0);
#endif
  return float4(color, 1.0);
}
