"""Prepare full-resolve HLSL matching the pinned runtime's small-group loaders.

Downloads hash-locked BSD-licensed Xenia sources. Retains the current gamma,
sample and numeric-format fixes while restoring the older scaled-buffer ABI.
"""
import hashlib
import json
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
lock = json.loads((ROOT / 'tools/graphics/resolve-shaders-lock.json').read_text())
upstream = ROOT / '.local/resolve-shaders-upstream'
prepared = ROOT / '.local/resolve-shaders'


def fetch(item):
    path = upstream / item['path']
    content = path.read_bytes() if path.exists() else b''
    if hashlib.sha256(content).hexdigest() != item['sha256']:
        url = (f"https://raw.githubusercontent.com/xenia-canary/xenia-canary/"
               f"{lock['revision']}/{item['path']}")
        with urllib.request.urlopen(url, timeout=30) as response:
            content = response.read()
        if hashlib.sha256(content).hexdigest() != item['sha256']:
            raise ValueError(f"Shader source hash mismatch: {item['path']}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    text = content.decode('utf-8')
    if item['path'].endswith('/edram.xesli'):
        start = text.index('  // Map the pixel and its sample index to canonical sample coordinates.')
        end = text.index('  // For now, while the actual storage of 64bpp render targets', start)
        # The pinned ROV translator shifts HOST pixel coordinates before adding
        # the sample bits. Newer Xenia uses a canonical, guest-pixel-interleaved
        # sample layout. Importing that helper alone scrambles every MSAA scene.
        text = text[:start] + '''  // Pinned ReXGlue ROV EDRAM ABI; sample bit 0 is vertical.
  uint2_xe rt_sample_index = pixel_index;
  if (msaa_samples >= kXenosMsaaSamples_2X) {
    rt_sample_index.y = (pixel_index.y << 1u) | (pixel_sample_index & 1u);
    if (msaa_samples >= kXenosMsaaSamples_4X) {
      rt_sample_index.x = (pixel_index.x << 1u) | (pixel_sample_index >> 1u);
    }
  }
''' + text[end:]
    if item['path'].endswith('/resolve.xesli'):
        start = text.index('      return uint2_xe(', text.index('uint2_xe XeResolveColorCopySampleOffsetsBytes('))
        end = text.index('\n    }', start)
        text = text[:start] + '''      // Pinned sample 1 is one host sample row below sample 0;
      // sample 2 is one host sample column to its right.
      return uint2_xe(80u * 4u * resolve_info.resolution_scale.x,
                      4u << resolve_info.edram_format_ints_log2);''' + text[end:]
    if item['path'].endswith('/texture_address.xesli'):
        old = ('return uint2_xe(bytes_per_block_log2 >= 3u ? 5u - bytes_per_block_log2 : 4u,\n'
               '                  3u - min(bytes_per_block_log2, 2u));')
        new = ('// FM4 pinned SDK ABI: 8-byte groups for 8bpp, 16 bytes otherwise;\n'
               '  // a single row per group, matching texture_load_*_scaled and fast resolves.\n'
               '  return uint2_xe((bytes_per_block_log2 == 0u ? 3u : 4u) - bytes_per_block_log2, 0u);')
        if text.count(old) != 1:
            raise ValueError('Expected scaled-addressing source was not found')
        text = text.replace(old, new)
    if item['path'].endswith('/resolve_full_64bpp.xesli') or item['path'].endswith('/resolve_full_128bpp.xesli'):
        # Each output store spans 16 bytes, which is the entire pinned group.
        # A second store can enter another tiled/scaled group. Its address is
        # not a linear increment, including at 1x. Recompute from host XY.
        bpp = 3 if item['path'].endswith('/resolve_full_64bpp.xesli') else 4
        step = 2 if bpp == 3 else 1
        old = f'dest_address += XeResolveLocalXAddressXor({step}u, {bpp}u);'
        if text.count(old) != 1:
            raise ValueError('Expected second HDR store was not found')
        text = text.replace(old, f'dest_address = XeResolveDestPixelAddress(\n      resolve_info, pixel_index + uint2_xe({step}u, 0u), {bpp}u);')
    if item['path'].endswith('/xesl.xesli'):
        old = 'RWByteAddressBuffer name : register(hlsl_t, hlsl_t_space);'
        if text.count(old) != 1:
            raise ValueError('Expected HLSL output-buffer declaration was not found')
        text = text.replace(old, 'RWBuffer<value_type> name : register(hlsl_t, hlsl_t_space);')
        for size, method in ((4, 'Store'), (8, 'Store2'), (16, 'Store4')):
            old = f'((name).{method}(uint(byte_address), value))'
            new = f'((name)[uint(byte_address) >> {size.bit_length() - 1}] = (value))'
            if text.count(old) != 1:
                raise ValueError('Expected HLSL output store was not found')
            text = text.replace(old, new)
    destination = prepared / item['path']
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text, encoding='utf-8', newline='\n')


with ThreadPoolExecutor(max_workers=6) as pool:
    list(pool.map(fetch, lock['files']))
print(f"Prepared {len(lock['files'])} locked shader files at {prepared}")
