"""Validate, import, finish and separately package FM4 DDS texture packs.

A pack is used automatically by the Enhanced launcher (texture_mode auto) only once its
manifest says "complete": true. `finish` sets that after validating every file and checking
that the files and hashes match the manifest exactly; imported packs are finished on import.
`combine` merges several packs into a new one (the game loads one pack folder); an identity
present in two packs with different pixels is refused.

Only deterministic file/format operations are performed. Modified 2026-10-23.
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
    else:
        print(json.dumps(export_pack(args.folder, args.output)))


if __name__ == '__main__':
    main()
