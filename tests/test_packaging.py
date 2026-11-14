#!/usr/bin/env python3
"""Exercise release packaging boundaries without a DOS toolchain or emulator."""
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('release_packager', REPO / 'pkg/build_release.py')
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)


def run(command, cwd, **kwargs):
    return subprocess.run(command, cwd=cwd, check=True, capture_output=True, timeout=30, **kwargs)


class PackagingTests(unittest.TestCase):
    def test_source_and_filename_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='gw-package-boundaries-') as temp:
            root = Path(temp)
            repo = root / 'repo'
            repo.mkdir()
            source = repo / 'long_original_source_name.c'
            source.write_bytes(b'int main(void) { return 0; }\n')
            run(['git', 'init', '-q'], repo)
            run(['git', 'add', source.name], repo)
            for case in ('valid', 'long_outer_name', 'changed_source', 'missing_source',
                         'duplicate_source', 'nonregular_source'):
                with self.subTest(case=case):
                    data = io.BytesIO()
                    with tarfile.open(fileobj=data, mode='w:gz') as archive:
                        names = [] if case == 'missing_source' else [source.name]
                        if case == 'duplicate_source':
                            names *= 2
                        for name in names:
                            member = tarfile.TarInfo(name)
                            if case == 'nonregular_source':
                                member.type = tarfile.SYMTYPE
                                member.linkname = 'elsewhere'
                                archive.addfile(member)
                            else:
                                payload = b'wrong' if case == 'changed_source' else source.read_bytes()
                                member.size = len(payload)
                                archive.addfile(member, io.BytesIO(payload))
                    package = root / (case + '.zip')
                    with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED) as archive:
                        archive.writestr('SOURCE/GWBASIC/SOURCE.TGZ', data.getvalue())
                        if case == 'long_outer_name':
                            archive.writestr('DOC/GWBASIC/TOOLONGNAME.TXT', 'bad')
                    if case == 'valid':
                        PACKAGER.validate_dos_package(package, repo)
                    else:
                        with self.assertRaises(ValueError):
                            PACKAGER.validate_dos_package(package, repo)

    def test_standalone_and_caller_selected_output(self):
        with tempfile.TemporaryDirectory(prefix='gw-package-pipeline-') as temp:
            root = Path(temp)
            repo = root / 'repo'
            repo.mkdir()
            for name in ('pkg/build_pkg.sh', 'pkg/GWBASIC.LSM', 'pkg/README.DOS',
                         'README.md', 'CHANGES.TXT', 'LICENSE'):
                target = repo / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO / name, target)
            (repo / 'include').mkdir()
            (repo / 'include/gwbasic.h').write_text(
                '// unrelated version "9.9.9"\n#define GW_VERSION "1.2.3"\n')
            (repo / 'build').mkdir()
            probe = repo / 'build/tracked-probe.txt'
            probe.write_text('Every tracked file belongs in the source archive.\n')
            run(['git', 'init', '-q'], repo)
            run(['git', 'add', '.'], repo)
            binary = root / 'fixture.exe'
            binary.write_bytes(b'MZ packaging fixture; never executed\n')
            (repo / 'dist').mkdir()
            default = repo / 'dist/gwbasic-1.2.3.zip'
            sentinel = b'Existing output must survive a caller-selected output path.'
            default.write_bytes(sentinel)
            custom = root / 'chosen output/GWBASIC.ZIP'
            env = dict(os.environ, GW_DOS16_EXE=str(binary), GW_PKG_OUT=str(custom))
            run(['bash', 'pkg/build_pkg.sh'], repo, env=env)
            self.assertEqual(default.read_bytes(), sentinel)
            PACKAGER.validate_dos_package(custom, repo)
            with zipfile.ZipFile(custom) as archive:
                entries = {name: archive.read(name) for name in archive.namelist()}
                self.assertIn(b'Version:         1.2.3\r\n', archive.read('APPINFO/GWBASIC.LSM'))
                self.assertEqual(archive.read('BIN/GWBASIC.EXE'), binary.read_bytes())
                with tarfile.open(fileobj=io.BytesIO(archive.read('SOURCE/GWBASIC/SOURCE.TGZ')),
                                  mode='r:gz') as sources:
                    self.assertEqual(sources.extractfile('build/tracked-probe.txt').read(), probe.read_bytes())
            env.pop('GW_PKG_OUT')
            run(['bash', 'pkg/build_pkg.sh'], repo, env=env)
            with zipfile.ZipFile(default) as archive:
                self.assertEqual({name: archive.read(name) for name in archive.namelist()}, entries)


if __name__ == '__main__':
    unittest.main(verbosity=2)
