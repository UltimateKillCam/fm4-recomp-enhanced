"""Inspect original FM4 containers; optionally copy them into a private profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
from datetime import datetime, timezone


def inspect(path):
    with path.open('rb') as stream:
        header = stream.read(0x971A)
    if len(header) < 0x971A or header[:4] not in (b'LIVE', b'PIRS', b'CON '):
        raise ValueError(f'Invalid or truncated content header: {path}')
    word = lambda offset: struct.unpack_from('>I', header, offset)[0]
    if word(0x360) != 0x4D530910:
        raise ValueError(f'Not FM4 content: {path}')
    content_type = word(0x344)
    if content_type not in (2, 0x9000):
        raise ValueError(f'Unsupported content type {content_type:08X}: {path}')
    if path.parent.name.upper() != f'{content_type:08X}':
        raise ValueError(f'Content type/folder mismatch: {path}')
    licenses = []
    mask = 0
    for offset in range(0x22C, 0x32C, 16):
        identity, bits, flags = struct.unpack_from('>QII', header, offset)
        if identity or bits or flags:
            licenses.append(dict(id=f'{identity:016X}', bits=f'{bits:08X}', flags=f'{flags:08X}'))
        if identity == 0xFFFFFFFFFFFFFFFF and flags & 1:
            mask |= bits
    return dict(file=str(path), filename=path.name, size=path.stat().st_size,
                magic=header[:4].decode('ascii'), content_type=f'{content_type:08X}',
                title_id=f'{word(0x360):08X}', content_id=header[0x32C:0x340].hex().upper(),
                title=header[0x411:0x511].decode('utf-16-be').split('\0')[0],
                marketplace_offer_type=None, licenses=licenses,
                unrestricted_license_mask=f'{mask:08X}',
                header_sha256=hashlib.sha256(header).hexdigest())


def file_hash(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def scan(root):
    records = []
    for folder in sorted(root.iterdir()):
        if folder.is_dir() and folder.name in ('00000002', '00009000'):
            for path in sorted(folder.iterdir()):
                if path.is_file():
                    if not (1 <= len(path.name) <= 42 and all(c in '0123456789abcdefABCDEF' for c in path.name)):
                        raise ValueError(f'Invalid content filename: {path}')
                    records.append(inspect(path))
    if not records:
        raise ValueError('No supported FM4 packages found')
    return records


def install(records, user_root, apply):
    # Resolve every ancestor before writing: profile junctions must not redirect
    # a package replacement into the original content collection.
    project = Path(__file__).resolve().parent.parent
    private = project / '.local'
    user_root = user_root.absolute()
    if not user_root.resolve().is_relative_to(private.resolve()):
        raise ValueError('Destination must be a private profile inside this project .local')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    archive = private / 'archive' / ('dlc-packages-' + stamp)
    plans = []
    for record in records:
        target = user_root / '0000000000000000/4D530910' / record['content_type'] / record['filename']
        for parent in [target, *target.parents]:
            if parent == private:
                break
            if parent.is_symlink() or parent.is_junction():
                # The leaf may be an old extracted-package junction, archived
                # by moving the junction itself; its target is never modified.
                if parent != target:
                    raise ValueError(f'Redirected destination: {parent}')
        same = (target.is_file() and target.stat().st_size == record['size']
                and inspect(target)['header_sha256'] == record['header_sha256']
                and file_hash(target) == file_hash(record['file']))
        plans.append(dict(source=record['file'], target=str(target), action='present' if same else 'copy'))
    if apply:
        for plan in plans:
            if plan['action'] == 'present':
                continue
            target = Path(plan['target'])
            target.parent.mkdir(parents=True, exist_ok=True)
            if os.path.lexists(target):
                backup = archive / target.parent.name / target.name
                backup.parent.mkdir(parents=True, exist_ok=True)
                target.rename(backup)
            shutil.copyfile(plan['source'], target)
    return plans


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--user-root', type=Path)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--hash-payloads', action='store_true')
    args = parser.parse_args()
    records = scan(args.source.resolve())
    if args.hash_payloads:
        for record in records:
            record['sha256'] = file_hash(record['file'])
    result = dict(captured_utc=datetime.now(timezone.utc).isoformat(), packages=records)
    if args.user_root:
        result['installation'] = install(records, args.user_root, args.apply)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f"Original content: {len(records)} packages; {sum(r['content_type'] == '00000002' for r in records)} marketplace, {sum(r['content_type'] == '00009000' for r in records)} avatar. " + ('Copied.' if args.apply else 'Preview only.'))


if __name__ == '__main__':
    main()
