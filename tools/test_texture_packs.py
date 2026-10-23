"""Format, ZIP boundary and round-trip regression tests; no game pixels."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('packs', Path(__file__).with_name('Texture-Packs.py'))
packs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packs)
NAME = 'rgba8-2x2-0123456789abcdef.dds'


def dds():
    header = [0] * 32
    for index, value in {0: 0x20534444, 1: 124, 2: 0x2100f, 3: 4, 4: 4,
                         5: 16, 7: 3, 19: 32, 20: 0x41, 22: 32, 23: 255,
                         24: 0xff00, 25: 0xff0000, 26: 0xff000000, 27: 0x401008}.items():
        header[index] = value
    return struct.pack('<32I', *header) + bytes(range(84))


class Packs(unittest.TestCase):
    def test_export_import_and_hashes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source'
            source.mkdir()
            (source / NAME).write_bytes(dds())
            first = packs.export_pack(source, root / 'a.zip')
            second = packs.export_pack(source, root / 'b.zip')
            self.assertEqual(first['sha256'], second['sha256'])
            dest = packs.import_pack(root / 'a.zip', 'Mine', root / 'textures')
            self.assertEqual((dest / 'replacements' / NAME).read_bytes(), dds())
            manifest = json.loads((dest / 'manifest.json').read_text())
            self.assertEqual(len(manifest['textures']), 1)
            with self.assertRaises(ValueError):
                packs.import_pack(source, 'Mine', root / 'textures')

    def test_unsafe_zip_rolls_back(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for index, name in enumerate(('../' + NAME, 'C:/' + NAME, 'run.ps1', 'sub/../../' + NAME)):
                path = root / f'bad-{index}.zip'
                with zipfile.ZipFile(path, 'w') as archive:
                    archive.writestr(name, dds())
                with self.assertRaises(ValueError):
                    packs.import_pack(path, 'Bad', root / 'textures')
                self.assertFalse((root / 'textures' / 'Bad').exists())

    def test_payload_and_scale_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / NAME
            path.write_bytes(dds()[:-1])
            with self.assertRaises(ValueError):
                packs.validate_dds(path)
            data = bytearray(dds())
            struct.pack_into('<I', data, 16, 6)
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                packs.validate_dds(path)


if __name__ == '__main__':
    unittest.main()
