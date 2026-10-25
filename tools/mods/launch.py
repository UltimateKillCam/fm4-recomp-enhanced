"""Launch a locally built mod-capable player with a verified optional session."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys

from prepare import prepare, verify


def exports(path):
    # Inspect PE export names without executing an EXE or library entry point.
    data = Path(path).read_bytes()
    if data[:2] != b'MZ':
        raise ValueError('Expected a Windows player executable')
    pe = struct.unpack_from('<I', data, 60)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid PE header')
    count, optional_size = struct.unpack_from('<H12xH', data, pe + 6)
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    directory = optional + (112 if magic == 0x20b else 96 if magic == 0x10b else 0)
    if directory == optional:
        raise ValueError('Unsupported PE optional header')
    sections = optional + optional_size
    def offset(rva):
        for i in range(count):
            at = sections + i * 40
            virtual_size, address, size, raw = struct.unpack_from('<4I', data, at + 8)
            if address <= rva < address + min(virtual_size, size):
                return raw + rva - address
        raise ValueError('Invalid export RVA')
    rva = struct.unpack_from('<I', data, directory)[0]
    if not rva:
        return set()
    table = offset(rva)
    names_count, names_rva = struct.unpack_from('<I4xI', data, table + 24)
    names = offset(names_rva)
    result = set()
    for i in range(names_count):
        at = offset(struct.unpack_from('<I', data, names + 4 * i)[0])
        end = data.index(0, at)
        result.add(data[at:end].decode('ascii'))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('exe', 'game', 'mods', 'state', 'stock-user'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--enable', action='store_true')
    parser.add_argument('--base-texture', type=Path)
    parser.add_argument('--string-requests', type=Path)
    parser.add_argument('--codec-runtime', type=Path)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    receipt = prepare(args.game, args.mods, args.state, args.stock_user, enabled=args.enable,
                      base_texture=args.base_texture, string_requests=args.string_requests, codec_runtime=args.codec_runtime)
    if args.prepare_only:
        print(receipt or 'Mods disabled; stock paths retained')
        return 0
    environment = os.environ.copy()
    environment.pop('FM4_MODS', None)
    environment.pop('FM4_MOD_SESSION', None)
    if receipt:
        verify(receipt)
        if 'Fm4ModGameProtocolV1' not in exports(args.exe):
            raise ValueError('Build the player with FM4_ENABLE_MODS=ON before enabling mods')
        if 'Fm4ModsConfigure' not in exports(args.exe.parent / 'rexruntime.dll'):
            raise ValueError('Build the runtime with FM4_MODS=ON before enabling mods')
        environment.update(FM4_MODS='1', FM4_MOD_SESSION=str(receipt))
    forwarded = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
    for argument in forwarded:
        if argument.split('=')[0] in ('--game_data_root', '--user_data_root'):
            raise ValueError('Use --game and --stock-user for session paths')
    command = [str(args.exe.resolve()), *forwarded,
               '--game_data_root=' + str(args.game.resolve()), '--user_data_root=' + str(args.stock_user.resolve())]
    flags = subprocess.BELOW_NORMAL_PRIORITY_CLASS | subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    child = subprocess.Popen(command, cwd=args.exe.resolve().parent, env=environment, creationflags=flags)
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        ctypes.windll.kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        ctypes.windll.kernel32.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not ctypes.windll.kernel32.GetProcessAffinityMask(int(child._handle), ctypes.byref(allowed), ctypes.byref(system)):
            child.terminate()
            raise OSError('Cannot query player CPU affinity')
        budget, selected = 0, 0
        for bit in range(ctypes.sizeof(ctypes.c_size_t) * 8):
            if allowed.value & (1 << bit) and selected < 12:
                budget |= 1 << bit
                selected += 1
        # Keep four of the owner's sixteen logical CPUs free.
        if not budget or not ctypes.windll.kernel32.SetProcessAffinityMask(int(child._handle), budget):
            child.terminate()
            raise OSError('Cannot apply player CPU affinity')
        while child.poll() is None:
            ctypes.windll.kernel32.SetProcessAffinityMask(int(child._handle), budget)
            ctypes.windll.kernel32.SetPriorityClass(int(child._handle), subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            try:
                return child.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                pass
    return child.wait()


if __name__ == '__main__':
    sys.exit(main())
