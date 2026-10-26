"""Validate, import, finish and separately package FM4 DDS texture packs.

A pack is used automatically by the Enhanced launcher (texture_mode auto) only once its
manifest says "complete": true. `finish` sets that after validating every file and checking
that the files and hashes match the manifest exactly; imported packs are finished on import.
`combine` merges several packs into a new one (the game loads one pack folder); an identity
present in two packs with different pixels is refused.
`repair` creates a new pack, restoring low-alpha BC3 top-level RGB from the original mip
below it. It requires numpy; other commands use only the standard library.

Only deterministic file/format operations are performed. Modified 2026-10-26.
Texture pixels never belong to the public source archive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
IDENTITY = re.compile(r'(bc[123]|rgba8)-([1-9][0-9]*)x([1-9][0-9]*)-[0-9a-f]{16}\.dds')
MAX_FILE = 64 << 20
MAX_TOTAL = 8 << 30
MAX_ENTRIES = 50000


def validate_dds(path):
    match = IDENTITY.fullmatch(path.name)
    if not match:
        raise ValueError('Invalid content-addressed DDS filename: ' + path.name)
    kind, source_w, source_h = match.groups()
    source_w, source_h = int(source_w), int(source_h)
    size = path.stat().st_size
    if not 128 <= size <= MAX_FILE:
        raise ValueError('Invalid DDS length: ' + path.name)
    with path.open('rb') as stream:
        header = stream.read(148)
    def u(offset):
        return struct.unpack_from('<I', header, offset)[0]
    if u(0) != 0x20534444 or u(4) != 124 or u(76) != 32:
        raise ValueError('Invalid DDS header: ' + path.name)
    width, height, mips, offset = u(16), u(12), max(u(28), 1), 128
    expected_format = {'bc1': 71, 'bc2': 74, 'bc3': 77, 'rgba8': 28}[kind]
    fourcc = u(84)
    actual = {0x31545844: 71, 0x33545844: 74, 0x35545844: 77}.get(fourcc)
    if (u(80) & 0x41) == 0x41 and [u(n) for n in (88, 92, 96, 100, 104)] == [32, 255, 0xff00, 0xff0000, 0xff000000]:
        actual = 28
    if fourcc == 0x30315844:
        if len(header) < 148 or u(132) != 3 or u(136) != 0 or u(140) != 1:
            raise ValueError('Only single 2D DDS textures are supported: ' + path.name)
        actual, offset = u(128), 148
    if u(112) != 0 or u(24) > 1:
        raise ValueError('Cubemaps, arrays and volume DDS are not supported: ' + path.name)
    if (actual != expected_format or not width or not height or max(width, height) > 4096 or
        width % source_w or height % source_h or width // source_w != height // source_h or
        width // source_w not in (2, 4) or mips != max(width, height).bit_length()):
        raise ValueError('DDS format, scale or complete mip chain is invalid: ' + path.name)
    block, block_bytes = (1, 4) if kind == 'rgba8' else (4, 8 if kind == 'bc1' else 16)
    payload = sum(((max(width >> mip, 1) + block - 1) // block) *
                  ((max(height >> mip, 1) + block - 1) // block) * block_bytes for mip in range(mips))
    if offset + payload != size:
        raise ValueError('DDS mip payload is truncated or has trailing data: ' + path.name)
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'file': path.name, 'width': width, 'height': height, 'mips': mips, 'sha256': digest}


def pack_folder(folder):
    folder = folder.resolve(strict=True)
    if (folder / 'replacements').is_dir():
        folder /= 'replacements'
    if not folder.is_dir():
        raise ValueError('Expected a texture pack folder')
    paths = sorted(folder.glob('*.dds'))
    if not paths or len(paths) > MAX_ENTRIES or sum(p.stat().st_size for p in paths) > MAX_TOTAL:
        raise ValueError('Empty or oversized texture pack')
    for path in paths:
        if path.is_symlink() or path.resolve().parent != folder:
            raise ValueError('Texture symlinks are not supported')
    return folder, [validate_dds(p) for p in paths]


def import_pack(source, name, textures_root):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', name):
        raise ValueError('Pack names use letters, digits, hyphens and underscores')
    textures_root.mkdir(parents=True, exist_ok=True)
    destination = textures_root.resolve() / name
    if destination.exists():
        raise ValueError('Pack already exists; choose a new name: ' + name)
    with tempfile.TemporaryDirectory(prefix='.import-', dir=textures_root) as stage_name:
        stage = Path(stage_name)
        replacements = stage / 'replacements'
        replacements.mkdir()
        if source.is_file():
            with zipfile.ZipFile(source) as archive:
                entries = archive.infolist()
                if len(entries) > MAX_ENTRIES or sum(e.file_size for e in entries) > MAX_TOTAL:
                    raise ValueError('Oversized texture ZIP')
                names = set()
                for entry in entries:
                    path = PurePosixPath(entry.filename.replace('\\', '/'))
                    if path.is_absolute() or '..' in path.parts or ':' in str(path) or ((entry.external_attr >> 16) & 0xf000) == 0xa000:
                        raise ValueError('Unsafe texture ZIP entry: ' + entry.filename)
                    if entry.is_dir():
                        continue
                    if path.name.lower() == 'manifest.json':
                        continue  # Import only validated DDS pixels, never commands/paths from a manifest.
                    if not IDENTITY.fullmatch(path.name) or entry.file_size > MAX_FILE:
                        raise ValueError('Unexpected texture ZIP entry: ' + entry.filename)
                    if path.name.lower() in names:
                        raise ValueError('Duplicate texture identity in ZIP')
                    names.add(path.name.lower())
                    with archive.open(entry) as src, (replacements / path.name).open('xb') as dst:
                        shutil.copyfileobj(src, dst, 1 << 20)
        else:
            folder, entries = pack_folder(source)
            for entry in entries:
                shutil.copyfile(folder / entry['file'], replacements / entry['file'])
        _, entries = pack_folder(replacements)
        (stage / 'manifest.json').write_text(json.dumps({'schema': 1, 'name': name, 'complete': True,
                                                        'completed': time.strftime('%Y-%m-%d'),
                                                        'textures': entries}, indent=2) + '\n')
        os.rename(stage, destination)
    return destination


def pack_root(folder):
    folder = folder.resolve(strict=True)
    return folder.parent if folder.name == 'replacements' else folder


def read_manifest(root):
    path = root / 'manifest.json'
    return json.loads(path.read_text(encoding='utf-8-sig')) if path.is_file() else {}


def pack_status(folder):
    root = pack_root(folder)
    manifest = read_manifest(root)
    return {'folder': str(root), 'complete': manifest.get('complete') is True,
            'textures': len(manifest.get('textures', [])), 'completed': manifest.get('completed')}


def finish_pack(folder):
    root = pack_root(folder)
    _, entries = pack_folder(root)
    manifest = read_manifest(root)
    listed = {e['file']: e['sha256'] for e in manifest.get('textures', [])}
    actual = {e['file']: e['sha256'] for e in entries}
    if listed and listed != actual:
        missing = sorted(set(listed) - set(actual))
        extra = sorted(set(actual) - set(listed))
        changed = sorted(f for f in set(listed) & set(actual) if listed[f] != actual[f])
        raise ValueError(f'Manifest does not match the pack files: {len(missing)} missing, '
                         f'{len(extra)} unlisted, {len(changed)} changed')
    manifest.update({'schema': 1, 'complete': True, 'completed': time.strftime('%Y-%m-%d'), 'textures': entries})
    manifest.setdefault('name', root.name)
    staged = root / 'manifest.json.finishing'
    staged.write_text(json.dumps(manifest, indent=2) + '\n')
    os.replace(staged, root / 'manifest.json')
    return pack_status(root)


def combine_packs(name, sources, textures_root):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', name):
        raise ValueError('Pack names use letters, digits, hyphens and underscores')
    destination = textures_root.resolve() / name
    if destination.exists():
        raise ValueError('Pack already exists; choose a new name: ' + name)
    chosen, origins = {}, []
    for source in sources:
        folder, entries = pack_folder(source)
        origins.append({'pack': pack_root(source).name, 'textures': len(entries)})
        for entry in entries:
            previous = chosen.get(entry['file'])
            if previous and previous[1]['sha256'] != entry['sha256']:
                raise ValueError('Packs disagree on ' + entry['file'] + '; combine refused')
            chosen.setdefault(entry['file'], (folder, entry))
    with tempfile.TemporaryDirectory(prefix='.combine-', dir=textures_root) as stage_name:
        stage = Path(stage_name)
        replacements = stage / 'replacements'
        replacements.mkdir()
        for file, (folder, _) in sorted(chosen.items()):
            shutil.copyfile(folder / file, replacements / file)
        _, entries = pack_folder(replacements)
        if {e['file']: e['sha256'] for e in entries} != {f: e['sha256'] for f, (_, e) in chosen.items()}:
            raise ValueError('Combined copy does not match its sources')
        (stage / 'manifest.json').write_text(json.dumps({'schema': 1, 'name': name, 'complete': True,
                                                        'completed': time.strftime('%Y-%m-%d'),
                                                        'combinedFrom': origins, 'textures': entries},
                                                       indent=2) + '\n')
        os.rename(stage, destination)
    return pack_status(destination)


def export_pack(folder, output):
    folder, entries = pack_folder(folder)
    if output.exists():
        raise ValueError('Refusing to overwrite an existing ZIP')
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_STORED) as archive:
        manifest = json.dumps({'schema': 1, 'textures': entries}, sort_keys=True, indent=2).encode()
        item = zipfile.ZipInfo('manifest.json', date_time=(1980, 1, 1, 0, 0, 0))
        archive.writestr(item, manifest)
        for entry in entries:
            item = zipfile.ZipInfo('replacements/' + entry['file'], date_time=(1980, 1, 1, 0, 0, 0))
            with (folder / entry['file']).open('rb') as src, archive.open(item, 'w', force_zip64=True) as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
    with output.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    output.with_suffix(output.suffix + '.sha256').write_text(digest + '  ' + output.name + '\n')
    return {'archive': str(output), 'textures': len(entries), 'sha256': digest}


def colour_palette(endpoints, four=True):
    import numpy as np
    values = endpoints.astype(np.int32)
    rgb = np.stack([((values >> 11) & 31) * 255 // 31,
                    ((values >> 5) & 63) * 255 // 63, (values & 31) * 255 // 31], -1)
    a, b = rgb[:, 0], rgb[:, 1]
    palette = np.stack([a, b, (2 * a + b) // 3, (a + 2 * b) // 3], 1)
    if not four:
        three = values[:, 0] <= values[:, 1]
        palette[three, 2] = (a[three] + b[three]) // 2
        palette[three, 3] = 0
    return palette


def decode_blocks(blocks, kind):
    import numpy as np
    colour = np.ascontiguousarray(blocks[:, -8:])
    endpoints = colour[:, :4].copy().view('<u2').reshape(-1, 2)
    palette = colour_palette(endpoints, four=kind != 'bc1')
    bits = colour[:, 4:].copy().view('<u4').reshape(-1)
    indices = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    rgb = palette[np.arange(len(blocks))[:, None], indices].astype(np.uint8)
    alpha = np.full((len(blocks), 16), 255, np.uint8)
    if kind == 'bc1':
        alpha[(endpoints[:, 0] <= endpoints[:, 1])[:, None] & (indices == 3)] = 0
    elif kind == 'bc3':
        a, b = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
        table = np.zeros((len(blocks), 8), np.int32)
        table[:, 0], table[:, 1] = a, b
        for index in range(1, 7):
            table[:, index + 1] = np.where(a > b, ((7 - index) * a + index * b) // 7,
                                          ((5 - index) * a + index * b) // 5 if index < 5 else 0)
        table[a <= b, 7] = 255
        bits = np.zeros(len(blocks), np.uint64)
        for index in range(6):
            bits |= blocks[:, index + 2].astype(np.uint64) << np.uint64(8 * index)
        indices = (bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & 7
        alpha = table[np.arange(len(blocks))[:, None], indices].astype(np.uint8)
    return rgb, alpha


def encode_colour_blocks(pixels):
    """Fit a principal axis, refine endpoints and search all four palette entries."""
    import numpy as np
    pixels = pixels.astype(np.float32)
    centre = pixels.mean(1)
    delta = pixels - centre[:, None]
    _, vectors = np.linalg.eigh(np.einsum('npi,npj->nij', delta, delta))
    axis = vectors[:, :, -1]
    projection = np.einsum('npi,ni->np', delta, axis)
    ends = np.stack([centre + axis * projection.max(1)[:, None],
                     centre + axis * projection.min(1)[:, None]], 1)
    best_error = np.full(len(pixels), np.inf)
    best = np.zeros((len(pixels), 8), np.uint8)
    rows = np.arange(len(pixels))[:, None]
    weights = np.array([1, 0, 2 / 3, 1 / 3], np.float32)
    for _ in range(8):
        quantized = np.rint(np.clip(ends, 0, 255) * np.array([31, 63, 31]) / 255).astype(np.uint16)
        packed = (quantized[..., 0] << 11) | (quantized[..., 1] << 5) | quantized[..., 2]
        high, low = packed.max(1), packed.min(1)
        equal = high == low
        high = np.where(equal & (high < 65535), high + 1, high).astype(np.uint16)
        low = np.where(equal & (high == low), low - 1, low).astype(np.uint16)
        endpoints = np.stack([high, low], 1)
        palette = colour_palette(endpoints)
        distance = ((pixels[:, :, None] - palette[:, None]) ** 2).sum(3)
        indices = distance.argmin(2).astype(np.uint32)
        error = distance[rows, np.arange(16), indices].sum(1)
        improved = error < best_error
        best_error[improved] = error[improved]
        bits = np.sum(indices << (2 * np.arange(16, dtype=np.uint32)), axis=1, dtype=np.uint32)
        candidate = np.concatenate([endpoints.astype('<u2').view(np.uint8).reshape(-1, 4),
                                    bits.astype('<u4').view(np.uint8).reshape(-1, 4)], 1)
        best[improved] = candidate[improved]
        # Least-squares endpoint fit for the current index assignment, then requantize.
        a = weights[indices]
        b = 1 - a
        aa, ab, bb = (a * a).sum(1), (a * b).sum(1), (b * b).sum(1)
        ap = (a[..., None] * pixels).sum(1)
        bp = (b[..., None] * pixels).sum(1)
        determinant = aa * bb - ab * ab
        usable = determinant > 1e-6
        divisor = np.where(usable, determinant, 1)[:, None]
        fitted = np.stack([(bb[:, None] * ap - ab[:, None] * bp) / divisor,
                           (aa[:, None] * bp - ab[:, None] * ap) / divisor], 1)
        ends = np.where(usable[:, None, None], fitted, ends)
    return best


def bilinear_rows(image, width, height, start, stop):
    """Pixel-centred bilinear resampling with clamped edges; RGB is not premultiplied."""
    import numpy as np
    x = np.clip((np.arange(width, dtype=np.float32) + .5) * image.shape[1] / width - .5,
                0, image.shape[1] - 1)
    y = np.clip((np.arange(start, stop, dtype=np.float32) + .5) * image.shape[0] / height - .5,
                0, image.shape[0] - 1)
    x0, y0 = x.astype(np.int32), y.astype(np.int32)
    x1, y1 = np.minimum(x0 + 1, image.shape[1] - 1), np.minimum(y0 + 1, image.shape[0] - 1)
    wx, wy = (x - x0)[None, :, None], (y - y0)[:, None, None]
    top = image[y0[:, None], x0] * (1 - wx) + image[y0[:, None], x1] * wx
    bottom = image[y1[:, None], x0] * (1 - wx) + image[y1[:, None], x1] * wx
    return np.rint(top * (1 - wy) + bottom * wy).astype(np.uint8)


def repair_dds(data, entry, alpha_threshold):
    import numpy as np
    kind = entry['file'].split('-')[0]
    if kind not in ('bc1', 'bc3'):
        return data, {'file': entry['file'], 'kind': kind, 'changed_blocks': 0, 'skipped': True}
    width, height = entry['width'], entry['height']
    offset = 148 if data[84:88] == b'DX10' else 128
    block_bytes = 8 if kind == 'bc1' else 16
    bw, bh = (width + 3) // 4, (height + 3) // 4
    size = bw * bh * block_bytes
    sw, sh = max(1, width // 2), max(1, height // 2)
    sbw, sbh = (sw + 3) // 4, (sh + 3) // 4
    source = np.frombuffer(data, np.uint8, sbw * sbh * block_bytes, offset + size).reshape(-1, block_bytes)
    rgb, _ = decode_blocks(source, kind)
    original = rgb.reshape(sbh, sbw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(sbh * 4, sbw * 4, 3)[:sh, :sw]
    output = bytearray(data)
    blocks = np.frombuffer(output, np.uint8, size, offset).reshape(-1, block_bytes)
    report = {'file': entry['file'], 'kind': kind, 'changed_blocks': 0, 'reencoded_blocks': 0, 'low_alpha_pixels': 0,
              'low_alpha_error_before': 0., 'low_alpha_error_after': 0.,
              'opaque_pixels': 0, 'opaque_squared_error': 0., 'visible_pixels': 0, 'gross_pixels': 0}
    for row in range(0, bh, 8):
        end = min(bh, row + 8)
        start, stop = row * 4, min(height, end * 4)
        chunk = blocks[row * bw:end * bw]
        rgb, alpha = decode_blocks(chunk, kind)
        target = bilinear_rows(original, width, height, start, stop)
        target = np.pad(target, ((0, (end - row) * 4 - len(target)), (0, bw * 4 - width), (0, 0)), mode='edge')
        target = target.reshape(end - row, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
        valid = np.zeros(((end - row) * 4, bw * 4), bool)
        valid[:stop - start, :width] = True
        valid = valid.reshape(end - row, 4, bw, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
        difference = np.abs(rgb.astype(np.int16) - target.astype(np.int16)).mean(2)
        visible = (alpha >= 128) & valid
        report['visible_pixels'] += int(visible.sum())
        report['gross_pixels'] += int(((difference > 60) & visible).sum())
        if kind == 'bc1':
            continue
        low = (alpha < alpha_threshold) & valid
        changed = ((difference > 0) & low).any(1)
        repaired = rgb.copy()
        repaired[low] = target[low]
        if changed.any():
            encoded = encode_colour_blocks(repaired[changed])
            report['changed_blocks'] += int((chunk[changed, 8:] != encoded).any(1).sum())
            chunk[changed, 8:] = encoded
        decoded, _ = decode_blocks(chunk, kind)
        report['reencoded_blocks'] += int(changed.sum())
        report['low_alpha_pixels'] += int(low.sum())
        report['low_alpha_error_before'] += float(difference[low].sum())
        report['low_alpha_error_after'] += float(np.abs(decoded.astype(np.int16) - target.astype(np.int16))[low].sum()) / 3
        opaque = (alpha == 255) & valid
        report['opaque_pixels'] += int(opaque.sum())
        report['opaque_squared_error'] += float(((decoded.astype(np.float64) - rgb)[opaque] ** 2).sum())
    report['gross_visible_fraction'] = report['gross_pixels'] / max(1, report['visible_pixels'])
    report['flagged'] = kind == 'bc1' and report['gross_visible_fraction'] > .02
    if kind == 'bc3':
        for key in ('low_alpha_error_before', 'low_alpha_error_after'):
            report[key] /= max(1, report['low_alpha_pixels'])
        mse = report['opaque_squared_error'] / max(1, 3 * report['opaque_pixels'])
        report['opaque_psnr_db'] = float(10 * np.log10(255 ** 2 / mse)) if mse else None
    return bytes(output), report


def repair_pack(folder, destination, alpha_threshold=16):
    import numpy  # Required only for this explicit, offline operation.
    if not 1 <= alpha_threshold <= 32:
        raise ValueError('Low-alpha threshold must be between 1 and 32')
    root = pack_root(folder)
    destination = destination.resolve()
    if destination.exists() or destination == root or root in destination.parents:
        raise ValueError('Repair requires a new folder outside the input pack')
    source, entries = pack_folder(root)
    manifest = read_manifest(root)
    listed = {e['file']: e['sha256'] for e in manifest.get('textures', [])}
    if listed and listed != {e['file']: e['sha256'] for e in entries}:
        raise ValueError('Input manifest does not match the pack files')
    for entry in entries:
        if entry['file'].startswith('bc3-'):
            match = IDENTITY.fullmatch(entry['file'])
            if entry['width'] != 2 * int(match[2]) or entry['height'] != 2 * int(match[3]):
                raise ValueError('BC3 repair requires original mip 0 at pack mip 1: ' + entry['file'])
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Keep an incomplete stage on failure for inspection; never overwrite or remove a pack.
    stage = Path(tempfile.mkdtemp(prefix='.repair-', dir=destination.parent))
    replacements = stage / 'replacements'
    replacements.mkdir()
    reports = []
    for entry in entries:
        data = (source / entry['file']).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise ValueError('Input changed during repair: ' + entry['file'])
        repaired, report = repair_dds(data, entry, alpha_threshold)
        with (replacements / entry['file']).open('xb') as stream:
            stream.write(repaired)
        reports.append(report)
        print(f"{len(reports)}/{len(entries)} {entry['file']}: {report['changed_blocks']} blocks", flush=True)
    _, fixed = pack_folder(stage)
    summary = {'source': str(root), 'alpha_threshold': alpha_threshold,
               'changed_textures': sum(r['changed_blocks'] > 0 for r in reports),
               'changed_blocks': sum(r['changed_blocks'] for r in reports),
               'reencoded_blocks': sum(r.get('reencoded_blocks', 0) for r in reports),
               'bc1_flagged': [r['file'] for r in reports if r.get('flagged')], 'textures': reports}
    (stage / 'repair-report.json').write_text(json.dumps(summary, indent=2) + '\n')
    manifest.update({'schema': 1, 'name': destination.name, 'complete': True,
                     'completed': time.strftime('%Y-%m-%d'), 'textures': fixed,
                     'repairedFrom': {'pack': root.name, 'alphaThreshold': alpha_threshold}})
    (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    os.rename(stage, destination)
    return {'folder': str(destination), 'textures': len(fixed), 'valid': True,
            'changed_textures': summary['changed_textures'], 'changed_blocks': summary['changed_blocks'],
            'bc1_flagged': summary['bc1_flagged']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    check = sub.add_parser('check')
    check.add_argument('folder', type=Path)
    imp = sub.add_parser('import')
    imp.add_argument('source', type=Path)
    imp.add_argument('--name', required=True)
    imp.add_argument('--textures-root', type=Path, default=ROOT / 'textures')
    status = sub.add_parser('status')
    status.add_argument('folder', type=Path)
    finish = sub.add_parser('finish', help='validate the pack and mark it finished for automatic use')
    finish.add_argument('folder', type=Path)
    comb = sub.add_parser('combine', help='merge packs into a new finished pack under textures/')
    comb.add_argument('sources', type=Path, nargs='+')
    comb.add_argument('--name', required=True)
    comb.add_argument('--textures-root', type=Path, default=ROOT / 'textures')
    exp = sub.add_parser('export')
    exp.add_argument('folder', type=Path)
    exp.add_argument('--output', type=Path, required=True)
    repair = sub.add_parser('repair', help='restore low-alpha BC3 top-level RGB into a new pack (numpy required)')
    repair.add_argument('folder', type=Path)
    repair.add_argument('destination', type=Path)
    repair.add_argument('--alpha-threshold', type=int, default=16)
    args = parser.parse_args()
    if args.command == 'check':
        folder, entries = pack_folder(args.folder)
        print(json.dumps({'folder': str(folder), 'textures': len(entries), 'valid': True}))
    elif args.command == 'status':
        print(json.dumps(pack_status(args.folder)))
    elif args.command == 'finish':
        print(json.dumps(finish_pack(args.folder)))
    elif args.command == 'combine':
        print(json.dumps(combine_packs(args.name, args.sources, args.textures_root)))
    elif args.command == 'import':
        print(import_pack(args.source, args.name, args.textures_root))
    elif args.command == 'repair':
        print(json.dumps(repair_pack(args.folder, args.destination, args.alpha_threshold)))
    else:
        print(json.dumps(export_pack(args.folder, args.output)))


if __name__ == '__main__':
    main()
