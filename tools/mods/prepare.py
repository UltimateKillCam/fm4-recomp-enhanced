"""Prepare read-only FM4 content overlays. Python 3.11+, standard library only."""
from __future__ import annotations

import argparse
from contextlib import closing
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sqlite3
import struct
import tomllib
import uuid
import zipfile
import zlib

PROTOCOL = 1
MAX_FILE = 256 << 20
MAX_ARCHIVE = 2 << 30
ID = re.compile(r"[a-z0-9][a-z0-9_-]{0,63}")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def relative(value):
    if not isinstance(value, str) or not value or '\\' in value:
        raise ValueError('Use a nonempty relative path with forward slashes')
    parts = value.split('/')
    if any(p in ('', '.', '..') or ':' in p or p.endswith((' ', '.')) for p in parts):
        raise ValueError('Unsafe relative path: ' + value)
    if any(any(ord(c) < 32 for c in p) for p in parts):
        raise ValueError('Control character in path')
    if any(p.split('.')[0].upper() in {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(10)), *(f'LPT{i}' for i in range(10))} for p in parts):
        raise ValueError('Reserved path: ' + value)
    return PurePosixPath(value)


def beneath(root, name, *, exists=True):
    root = Path(root).resolve()
    path = root.joinpath(*relative(name).parts)
    current = root
    for part in relative(name).parts:
        current /= part
        if current.is_symlink() or (current.exists() and getattr(current, 'is_junction', lambda: False)()):
            raise ValueError('Links are not supported: ' + str(current))
    resolved = path.resolve(strict=exists)
    if not resolved.is_relative_to(root):
        raise ValueError('Path escaped root')
    return resolved


def game_file(game, name):
    # Disc extraction preserves mixed case; guest paths are case insensitive.
    current = Path(game)
    for part in relative(name).parts:
        matches = [p for p in current.iterdir() if p.name.casefold() == part.casefold()]
        if len(matches) != 1:
            raise ValueError('Missing or ambiguous game path: ' + name)
        current = matches[0]
        if current.is_symlink() or getattr(current, 'is_junction', lambda: False)():
            raise ValueError('Linked game path: ' + name)
    if not current.is_file():
        raise ValueError('Game target is not a file: ' + name)
    return current


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def manifest(folder, game_hash):
    value = tomllib.loads((folder / 'mod.toml').read_text(encoding='utf-8-sig'))
    allowed = {'id', 'name', 'version', 'enabled', 'order', 'game_version', 'gameplay',
               'native', 'files', 'archives', 'database', 'textures', 'settings', 'strings'}
    if set(value) - allowed:
        raise ValueError(f'{folder.name}: unknown manifest fields {set(value) - allowed}')
    if not ID.fullmatch(value.get('id', '')) or value['id'] != folder.name:
        raise ValueError('Mod id must match its directory')
    for key in ('name', 'version', 'game_version'):
        if not isinstance(value.get(key), str) or not value[key]:
            raise ValueError(f'{folder.name}: missing {key}')
    if type(value.get('enabled', False)) is not bool or type(value.get('gameplay', False)) is not bool:
        raise ValueError('enabled/gameplay must be boolean')
    if type(value.get('order', 0)) is not int:
        raise ValueError('order must be an integer')
    version = value['game_version'].lower()
    if version != '*' and (not re.fullmatch('[0-9a-f]{16,64}', version) or not game_hash.startswith(version)):
        raise ValueError(folder.name + ': executable version mismatch')
    if not isinstance(value.get('settings', {}), dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in value.get('settings', {}).items()):
        raise ValueError('settings must contain string values')
    return value


def quote(name):
    if not isinstance(name, str) or not IDENTIFIER.fullmatch(name):
        raise ValueError('Invalid database identifier')
    return '"' + name + '"'


def patch_database(source, destination, patches):
    destination.parent.mkdir(parents=True, exist_ok=True)
    # SQLite backup captures a consistent source snapshot without hard links.
    with closing(sqlite3.connect(source.as_uri() + '?mode=ro', uri=True)) as original:
        with closing(sqlite3.connect(destination)) as copied:
            original.backup(copied)
    with closing(sqlite3.connect(destination)) as db, db:
        db.execute('PRAGMA foreign_keys=ON')
        for patch in patches:
            if set(patch) - {'table', 'where', 'set', 'expect'}:
                raise ValueError('Unknown database patch fields')
            values, where = patch['set'], patch['where']
            if not values or not where or type(patch.get('expect')) is not int or patch['expect'] < 1:
                raise ValueError('Database patches require set, nonempty where and positive expect')
            if any(type(v) not in (str, int, float, bool, type(None)) for v in [*values.values(), *where.values()]):
                raise ValueError('Patch values must be scalar')
            statement = ('UPDATE ' + quote(patch['table']) + ' SET ' +
                         ','.join(quote(k) + '=?' for k in values) + ' WHERE ' +
                         ' AND '.join(quote(k) + ' IS ?' for k in where))
            result = db.execute(statement, [*values.values(), *where.values()])
            if result.rowcount != patch['expect']:
                raise ValueError(f'Database row count mismatch: expected {patch["expect"]}, got {result.rowcount}')
        if db.execute('PRAGMA integrity_check').fetchone() != ('ok',):
            raise ValueError('Patched database failed integrity_check')
        if db.execute('PRAGMA foreign_key_check').fetchone():
            raise ValueError('Patched database failed foreign_key_check')


def patch_archive(source, destination, replacements):
    if not zipfile.is_zipfile(source):
        raise ValueError('Unsupported archive format (only ZIP and ZIP-format PAK): ' + str(source))
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(source) as old:
        entries = old.infolist()
        if len(entries) > 50000 or sum(x.file_size for x in entries) > MAX_ARCHIVE:
            raise ValueError('Archive exceeds preparation limits')
        names = {}
        for entry in entries:
            key = str(relative(entry.filename.rstrip('/'))).lower()
            if key in names or ((entry.external_attr >> 16) & 0xf000) == 0xa000 or entry.flag_bits & 1:
                raise ValueError('Duplicate, linked or encrypted archive member')
            names[key] = entry
        if set(replacements) - set(names):
            raise ValueError('Archive replacement member does not exist')
        # Preserve compressed bytes of untouched members, including FM4 method
        # 21 members which the Python ZIP decoder does not understand.
        if len(entries) >= 65535 or source.stat().st_size > MAX_ARCHIVE:
            raise ValueError('Only bounded ZIP32 containers are supported')
        central = []
        with source.open('rb') as src, destination.open('xb') as out:
            src.seek(old.start_dir)
            originals = []
            for entry in entries:
                record = src.read(46)
                if record[:4] != b'PK\x01\x02' or entry.file_size >= 0xffffffff or entry.compress_size >= 0xffffffff:
                    raise ValueError('Invalid or ZIP64 central directory')
                tail = src.read(sum(struct.unpack_from('<3H', record, 28)))
                originals.append(record + tail)
            for entry, original in zip(entries, originals):
                offset = out.tell()
                key = entry.filename.rstrip('/').lower()
                if key in replacements:
                    replacement = replacements[key]
                    payload = replacement if isinstance(replacement, bytes) else replacement.read_bytes()
                    buffer = io.BytesIO()
                    with zipfile.ZipFile(buffer, 'w') as temporary:
                        info = zipfile.ZipInfo(entry.filename, entry.date_time)
                        info.external_attr = entry.external_attr
                        info.comment = entry.comment
                        temporary.writestr(info, payload, compress_type=zipfile.ZIP_STORED)
                    with zipfile.ZipFile(buffer) as temporary:
                        raw = buffer.getvalue()
                        out.write(raw[:temporary.start_dir])
                        record = bytearray(raw[temporary.start_dir:-22])
                else:
                    src.seek(entry.header_offset)
                    header = src.read(30)
                    if header[:4] != b'PK\x03\x04':
                        raise ValueError('Invalid local ZIP header')
                    name_size, extra_size = struct.unpack_from('<2H', header, 26)
                    length = 30 + name_size + extra_size + entry.compress_size
                    if entry.flag_bits & 8:
                        src.seek(entry.header_offset + length)
                        marker = src.read(4)
                        length += 16 if marker == b'PK\x07\x08' else 12
                    src.seek(entry.header_offset)
                    remaining = length
                    while remaining:
                        block = src.read(min(remaining, 1 << 20))
                        if not block:
                            raise ValueError('Truncated archive member')
                        out.write(block)
                        remaining -= len(block)
                    record = bytearray(original)
                struct.pack_into('<I', record, 42, offset)
                central.append(record)
            start = out.tell()
            for record in central:
                out.write(record)
            size = out.tell() - start
            out.write(struct.pack('<4s4H2IH', b'PK\x05\x06', 0, 0, len(entries), len(entries), size, start, len(old.comment)))
            out.write(old.comment)


def patch_strings(data, replacements):
    """Rebuild the UTF-16 pool of a checked FM4 LSB2 table; keep key sections."""
    if len(data) < 44 or data[:12] != b'LSB2\x01\0\0\0\x02\0\0\0':
        raise ValueError('Unsupported string table header')
    first, stride, pool, second, extra, last = struct.unpack_from('>6I', data, 12)
    sections = [offset for offset in (second, extra, last) if offset]
    if first != 36 or stride != 4 or pool != first or not sections or sections != sorted(set(sections)) or not first < second <= len(data) or any(offset > len(data) for offset in sections):
        raise ValueError('Unsupported string table sections')
    length, count = struct.unpack_from('>2I', data, first)
    start = first + 8 + (count + 1) * 6
    if length != second - first or count > 65534 or start > second or (second - start) % 2:
        raise ValueError('Invalid string table pool')
    records = [struct.unpack_from('>HI', data, first + 8 + i * 6) for i in range(count + 1)]
    pool_end = start + records[-1][1] * 2
    if records[-1][0] != 65535 or pool_end > second or second - pool_end > 3 or any(data[pool_end:second]):
        raise ValueError('Invalid string table sentinel')
    if set(replacements) - {key for key, _ in records[:-1]}:
        raise ValueError('Replacement string id does not exist')
    if records[0][1] > records[-1][1]:
        raise ValueError('Invalid leading string pool offset')
    body, index = bytearray(data[start:start + records[0][1] * 2]), bytearray()
    for i, (key, offset) in enumerate(records[:-1]):
        end = records[i + 1][1]
        if not offset < end <= records[-1][1]:
            raise ValueError('Invalid string table offsets')
        text = data[start + offset * 2:start + end * 2]
        if not text.endswith(b'\0\0'):
            raise ValueError('String table text is not terminated')
        if key in replacements:
            value = replacements[key]
            if not isinstance(value, str) or '\0' in value or len(value) > 16384:
                raise ValueError('Invalid replacement string')
            text = value.encode('utf-16-be') + b'\0\0'
        index.extend(struct.pack('>HI', key, len(body) // 2))
        body.extend(text)
    index.extend(struct.pack('>HI', 65535, len(body) // 2))
    padding = data[pool_end:second]
    section_size = 8 + len(index) + len(body) + len(padding)
    section = struct.pack('>2I', section_size, count) + index + body
    section += padding
    delta = len(section) - length
    header = bytearray(data[:first])
    for at, offset in ((24, second), (28, extra), (32, last)):
        if offset:
            struct.pack_into('>I', header, at, offset + delta)
    return bytes(header) + section + data[second:]


def archive_member(archive, name, codec_runtime=None):
    entry = archive.getinfo(name)
    if entry.file_size > MAX_FILE or entry.compress_size > MAX_FILE:
        raise ValueError('String table exceeds preparation limits')
    if entry.compress_type != 21:
        return archive.read(name)
    if not codec_runtime or os.name != 'nt':
        raise ValueError('FM4 LZX string tables require --codec-runtime pointing to a mod-capable rexruntime.dll')
    import ctypes
    library_path = Path(codec_runtime).resolve(strict=True)
    with os.add_dll_directory(str(library_path.parent)):
        library = ctypes.CDLL(str(library_path))
    decoder = library.Fm4ModsDecodeLzx
    decoder.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32]
    decoder.restype = ctypes.c_int32
    with Path(archive.filename).open('rb') as stream:
        stream.seek(entry.header_offset)
        header = stream.read(30)
        if header[:4] != b'PK\x03\x04':
            raise ValueError('Invalid compressed member header')
        sizes = struct.unpack_from('<HH', header, 26)
        stream.seek(sum(sizes), 1)
        raw = stream.read(entry.compress_size)
    packed, at, total = bytearray(), 0, 0
    while at < len(raw):
        if raw[at] == 255:
            if at + 5 > len(raw): raise ValueError('Truncated LZX frame')
            amount, size = struct.unpack_from('>HH', raw, at + 1)
            at += 5
        else:
            if at + 2 > len(raw): raise ValueError('Truncated LZX frame')
            size = struct.unpack_from('>H', raw, at)[0]
            amount, at = 32768, at + 2
        if not size:
            if any(raw[at:]): raise ValueError('Trailing LZX data')
            break
        if not 0 < amount <= 32768 or at + size > len(raw):
            raise ValueError('Invalid LZX frame bounds')
        packed.extend(raw[at:at + size]); at += size; total += amount
        if total > entry.file_size: raise ValueError('LZX output exceeds declared size')
    if total != entry.file_size:
        raise ValueError('LZX output size differs')
    output = ctypes.create_string_buffer(total)
    if decoder(bytes(packed), len(packed), output, total) != 0 or zlib.crc32(output.raw) != entry.CRC:
        raise ValueError('LZX string decoding failed CRC verification')
    return output.raw


def texture_module():
    spec = importlib.util.spec_from_file_location('fm4_texture_packs', Path(__file__).parents[1] / 'Texture-Packs.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare(game, mods, state, stock_user, *, enabled=False, base_texture=None, string_requests=None, codec_runtime=None):
    # Do not even scan manifests or create state when disabled / absent.
    if not enabled or not mods.is_dir():
        return None
    if mods.is_symlink() or getattr(mods, 'is_junction', lambda: False)():
        raise ValueError('Linked mods root is not supported')
    folders = sorted(p for p in mods.iterdir() if p.is_dir() and (p / 'mod.toml').is_file())
    selected = []
    for folder in folders:
        if folder.is_symlink() or getattr(folder, 'is_junction', lambda: False)():
            raise ValueError('Linked mod directory')
        raw = tomllib.loads((folder / 'mod.toml').read_text(encoding='utf-8-sig'))
        if raw.get('enabled') is True:
            selected.append(folder)
    if not selected:
        return None
    game = game.resolve(strict=True)
    stock_user = stock_user.resolve()
    game_hash = digest(game_file(game, 'default.xex'))
    loaded = [(folder, manifest(folder, game_hash)) for folder in selected]
    loaded.sort(key=lambda item: (item[1].get('order', 0), item[1]['id']))
    inputs = []
    def input_file(root, name):
        path = beneath(root, name)
        if not path.is_file() or path.stat().st_size > MAX_FILE:
            raise ValueError('Missing or oversized mod input: ' + str(path))
        inputs.append({'path': str(path), 'sha256': digest(path)})
        return path
    loose, archives, databases, textures, plugins, strings = {}, {}, {}, {}, [], []
    string_archives = {}
    requests = read_json(string_requests) if string_requests else {}
    if codec_runtime:
        inputs.append({'path': str(codec_runtime.resolve()), 'sha256': digest(codec_runtime)})
    if string_requests:
        inputs.append({'path': str(string_requests.resolve()), 'sha256': digest(string_requests)})
    gameplay = False
    texture_api = texture_module()
    if base_texture:
        folder, entries = texture_api.pack_folder(base_texture)
        for entry in entries:
            textures[entry['file']] = input_file(folder, entry['file'])
    for folder, mod in loaded:
        input_file(folder, 'mod.toml')
        gameplay |= bool(mod.get('gameplay') or mod.get('database') or mod.get('native') or mod.get('files') or mod.get('archives'))
        for target, local in mod.get('files', {}).items():
            key = str(relative(target)).lower()
            if key == 'default.xex':
                raise ValueError('Executable replacement is not supported')
            loose[key] = input_file(folder, local)
            # A later complete file shadows earlier member/row/text edits.
            archives.pop(key, None)
            databases.pop(key, None)
            string_archives.pop(key, None)
        for item in mod.get('archives', []):
            if set(item) != {'path', 'member', 'source'}:
                raise ValueError('Archive patch requires path, member, source')
            key = str(relative(item['path'])).lower()
            member = str(relative(item['member'])).lower()
            archives.setdefault(key, {})[member] = input_file(folder, item['source'])
        for item in mod.get('database', []):
            if set(item) != {'path', 'patch'}:
                raise ValueError('Database entry requires path and patch')
            key = str(relative(item['path'])).lower()
            patch = read_json(input_file(folder, item['patch']))
            databases.setdefault(key, []).extend(patch)
        # Replacement text is an explicit SQLite table/key/column mapping.
        for item in mod.get('strings', []):
            sqlite_binding = set(item) == {'path', 'table', 'id_column', 'id', 'text_column', 'text'}
            archive_binding = set(item) == {'archive', 'table', 'id', 'text'}
            if not sqlite_binding and not archive_binding:
                raise ValueError('Invalid replacement string declaration')
            item = dict(item)
            item['text'] = requests.get(mod['id'], {}).get(item['table'], {}).get(str(item['id']), item['text'])
            if not isinstance(item['text'], str) or len(item['text']) > 16384:
                raise ValueError('Invalid replacement text')
            if archive_binding:
                key = str(relative(item['archive'])).lower()
                table = str(relative(item['table'])).lower()
                if type(item['id']) is not int or not 0 <= item['id'] < 65535:
                    raise ValueError('String id must be an unsigned 16-bit integer')
                string_archives.setdefault(key, {}).setdefault(table, {})[item['id']] = item['text']
            else:
                gameplay = True
                key = str(relative(item['path'])).lower()
                databases.setdefault(key, []).append({'table': item['table'], 'where': {item['id_column']: item['id']},
                                                       'set': {item['text_column']: item['text']}, 'expect': 1})
            strings.append(item)
        if mod.get('native'):
            library = input_file(folder, mod['native'])
            if library.suffix.lower() != '.dll':
                raise ValueError('Native plugin must be a DLL')
            plugins.append({'id': mod['id'], 'source': str(library), 'settings': mod.get('settings', {})})
        if mod.get('textures'):
            pack = beneath(folder, mod['textures'])
            pack_folder, entries = texture_api.pack_folder(pack)
            for entry in entries:
                textures[entry['file']] = input_file(pack_folder, entry['file'])
    for key in set(archives) | set(databases) | set(string_archives):
        if key in archives and key in databases:
            raise ValueError('A target cannot be both archive and database')
        source = loose.get(key) or game_file(game, key)
        inputs.append({'path': str(source), 'sha256': digest(source)})
    identity = {'protocol': PROTOCOL, 'game': str(game), 'game_sha256': game_hash,
                'mods': [m for _, m in loaded], 'inputs': inputs, 'stock_user': str(stock_user)}
    session = hashlib.sha256(json.dumps(identity, sort_keys=True, allow_nan=False).encode()).hexdigest()
    state = state.resolve()
    if state == game or state.is_relative_to(game) or state == stock_user or state.is_relative_to(stock_user):
        raise ValueError('State must be separate from game and stock saves')
    if any(state == f.resolve() or state.is_relative_to(f.resolve()) for f, _ in loaded):
        raise ValueError('State must be separate from mod inputs')
    generation = state / 'sessions' / session
    receipt_path = generation / 'session.json'
    if receipt_path.is_file():
        verify(receipt_path)
        return receipt_path
    state.mkdir(parents=True, exist_ok=True)
    stage = state / ('preparing-' + uuid.uuid4().hex)
    stage.mkdir()
    overlay = stage / 'overlay'
    overlay.mkdir()
    for key, source in loose.items():
        if key not in archives and key not in databases:
            dst = beneath(overlay, key, exists=False)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, dst)
    for key, tables in string_archives.items():
        source = loose.get(key) or game_file(game, key)
        with zipfile.ZipFile(source) as original:
            members = {str(relative(n)).lower(): n for n in original.namelist() if not n.endswith('/')}
            for table, changes in tables.items():
                if table not in members or table in archives.get(key, {}):
                    raise ValueError('Missing string table or conflicting whole-member override')
                data = archive_member(original, members[table], codec_runtime)
                archives.setdefault(key, {})[table] = patch_strings(data, changes)
    for key, members in archives.items():
        patch_archive(loose.get(key) or game_file(game, key), beneath(overlay, key, exists=False), members)
    for key, patches in databases.items():
        patch_database(loose.get(key) or game_file(game, key), beneath(overlay, key, exists=False), patches)
    for plugin in plugins:
        target = stage / 'plugins' / (plugin['id'] + '.dll')
        target.parent.mkdir(exist_ok=True)
        shutil.copyfile(plugin.pop('source'), target)
        plugin['library'] = 'plugins/' + target.name
    if textures:
        target = stage / 'textures' / 'replacements'
        target.mkdir(parents=True)
        for name, source in textures.items():
            shutil.copyfile(source, target / name)
        _, entries = texture_api.pack_folder(target)
        (stage / 'textures' / 'manifest.json').write_text(json.dumps({'schema': 1, 'complete': True, 'textures': entries}), encoding='utf-8')
    # Detect inputs changed while copying or patching. Failed stages are retained.
    if any(digest(item['path']) != item['sha256'] for item in inputs):
        raise ValueError('Mod/game input changed during preparation')
    outputs = {p.relative_to(stage).as_posix(): digest(p) for p in stage.rglob('*') if p.is_file()}
    user = state / 'profiles' / session if gameplay else stock_user
    receipt = {**identity, 'session': session, 'overlay': 'overlay', 'plugins': plugins,
               'textures': 'textures' if textures else '', 'isolated_saves': gameplay,
               'user_root': str(user), 'outputs': outputs, 'strings': strings}
    (stage / 'session.json').write_text(json.dumps(receipt, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    generation.parent.mkdir(exist_ok=True)
    stage.rename(generation)
    if gameplay:
        user.mkdir(parents=True, exist_ok=True)
        (user / 'mod-session.json').write_text(json.dumps({'session': session, 'mods': receipt['mods']}, indent=2), encoding='utf-8')
    return receipt_path


def verify(receipt_path):
    path = Path(receipt_path).resolve(strict=True)
    data = read_json(path)
    if data['protocol'] != PROTOCOL or data['session'] != path.parent.name:
        raise ValueError('Invalid session receipt')
    if digest(game_file(Path(data['game']), 'default.xex')) != data['game_sha256']:
        raise ValueError('Game executable changed')
    actual = {p.relative_to(path.parent).as_posix() for p in path.parent.rglob('*') if p.is_file() and p != path}
    if actual != set(data['outputs']):
        raise ValueError('Prepared session file set changed')
    for name, expected in data['outputs'].items():
        if digest(beneath(path.parent, name)) != expected:
            raise ValueError('Prepared output changed: ' + name)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    prep = sub.add_parser('prepare')
    for key in ('game', 'mods', 'state', 'stock-user'):
        prep.add_argument('--' + key, type=Path, required=True)
    prep.add_argument('--enable', action='store_true')
    prep.add_argument('--base-texture', type=Path)
    prep.add_argument('--string-requests', type=Path)
    prep.add_argument('--codec-runtime', type=Path)
    check = sub.add_parser('verify')
    check.add_argument('receipt', type=Path)
    args = parser.parse_args()
    if args.command == 'verify':
        verify(args.receipt)
        print('Session verified')
    else:
        result = prepare(args.game, args.mods, args.state, args.stock_user, enabled=args.enable, base_texture=args.base_texture, string_requests=args.string_requests, codec_runtime=args.codec_runtime)
        print(str(result) if result else 'Mods disabled; stock paths retained')


if __name__ == '__main__':
    main()
