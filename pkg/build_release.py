#!/usr/bin/env python3
"""Package supplied binaries beside clean source; never creates a tag or publishes."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

REPO = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=120, **kwargs)


def validate_dos_package(path, repo):
    """Check DOS-installable names and lossless inclusion of the tracked source."""
    tracked = subprocess.check_output(
        ['git', 'ls-files', '-z'], cwd=repo
    ).decode().split('\0')[:-1]
    with zipfile.ZipFile(path) as package:
        if package.testzip() is not None:
            raise ValueError('FreeDOS package has a CRC error')
        for name in package.namelist():
            if any(not re.fullmatch(r'[A-Za-z0-9_-]{1,8}(?:\.[A-Za-z0-9_-]{1,3})?', part)
                   for part in name.rstrip('/').split('/')):
                raise ValueError(f'FreeDOS package entry is not 8.3: {name}')
        source = package.read('SOURCE/GWBASIC/SOURCE.TGZ')
    with tarfile.open(fileobj=io.BytesIO(source), mode='r:gz') as archive:
        members = archive.getmembers()
        if len(members) != len(tracked) or {m.name for m in members} != set(tracked):
            raise ValueError('FreeDOS source inventory differs from tracked source')
        for member in members:
            if not member.isfile() or archive.extractfile(member).read() != (repo / member.name).read_bytes():
                raise ValueError(f'FreeDOS source differs: {member.name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=REPO / 'build')
    parser.add_argument('--dos16', type=Path, default=REPO / 'gwbasic16.exe')
    parser.add_argument('--dos32', type=Path, default=REPO / 'gwbasic.exe')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # This proves the source archive's identity. CI supplies fresh binaries from
    # that same checkout; version checks alone cannot prove a local build's age.
    run(['git', 'diff', '--quiet', 'HEAD'], cwd=REPO)
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip()
    version = re.search(r'#define GW_VERSION "([0-9.]+)"', (REPO / 'include/gwbasic.h').read_text())[1]
    build = args.build.resolve()
    for path in (build / 'gwbasic', build / 'gwbasic-compile', build / 'libgwrt.a', args.dos16, args.dos32):
        if not path.is_file():
            parser.error(f'missing build artifact: {path}')
    actual = subprocess.check_output([str(build / 'gwbasic'), '--version'], text=True, timeout=5).strip()
    if actual != f'GW-BASIC {version}':
        parser.error(f'interpreter version mismatch: {actual}')
    for path in (args.dos16, args.dos32):
        if version.encode() not in path.read_bytes():
            parser.error(f'DOS binary does not contain version {version}: {path}')
    if args.output.exists() and any(args.output.iterdir()):
        parser.error('--output must be empty or absent')
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    binaries = {'linux/gwbasic': build / 'gwbasic', 'linux/gwbasic-compile': build / 'gwbasic-compile',
                'linux/libgwrt.a': build / 'libgwrt.a', 'dos16/gwbasic.exe': args.dos16,
                'dos32/gwbasic.exe': args.dos32}
    metadata = {'version': version, 'source_commit': commit, 'system': platform.system(),
                'machine': platform.machine(), 'libc': platform.libc_ver(),
                'binary_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest()
                                  for name, path in binaries.items()},
                'provenance': 'Source commit is verified. Binary provenance depends on the build workflow; '
                              'this packager checks versions and records hashes of supplied binaries.',
                'linux_dependencies': 'glibc, libm, libpulse-simple, libpulse; gcc for AOT compilation',
                'dos32_extender': 'DOS4GW.EXE required, not included'}
    if os.environ.get('GITHUB_RUN_ID'):
        metadata['ci_run_url'] = (os.environ.get('GITHUB_SERVER_URL', 'https://github.com') + '/' +
                                  os.environ['GITHUB_REPOSITORY'] + '/actions/runs/' + os.environ['GITHUB_RUN_ID'])
    metadata_json = json.dumps(metadata, indent=2) + '\n'
    with tempfile.TemporaryDirectory(prefix='gwbasic-release-') as temp:
        root = Path(temp)
        name = f'gwbasic-{version}-linux-{platform.machine()}'
        linux = root / name
        (linux / 'bin').mkdir(parents=True)
        (linux / 'build').mkdir()
        for binary in ('gwbasic', 'gwbasic-compile'):
            shutil.copy2(build / binary, linux / 'bin' / binary)
        shutil.copy2(build / 'libgwrt.a', linux / 'build/libgwrt.a')
        shutil.copytree(REPO / 'include', linux / 'include')
        for doc in ('README.md', 'CHANGES.TXT', 'LICENSE', 'RELEASE_NOTES.md'):
            shutil.copy2(REPO / doc, linux / doc)
        (linux / 'BUILD.json').write_text(metadata_json)
        (linux / 'QUICKSTART.txt').write_text(
            'Run bin/gwbasic program.bas. For the compiler, from this directory:\n'
            'bin/gwbasic-compile program.bas -c --runtime .\n'
            'Install GCC and PulseAudio development libraries for native compilation.\n'
            'See BUILD.json for the build platform and host glibc version.\n')
        # Validate the archive layout using the copied compiler and runtime.
        example = linux / 'hello.bas'
        example.write_text('10 PRINT "RELEASE OK"\n')
        run([str(linux / 'bin/gwbasic-compile'), 'hello.bas', '-c', '--runtime', '.'], cwd=linux,
            stdout=subprocess.DEVNULL)
        result = subprocess.check_output([str(linux / 'hello')], cwd=linux, text=True, timeout=5).strip()
        if result != 'RELEASE OK':
            raise RuntimeError('packaged compiler/runtime smoke failed')
        (linux / 'hello').unlink()
        (linux / 'hello.c').unlink(missing_ok=True)
        with tarfile.open(output / (name + '.tar.gz'), 'w:gz') as archive:
            archive.add(linux, arcname=name)
        dos16_zip = root / f'gwbasic-{version}.zip'
        run(['bash', str(REPO / 'pkg/build_pkg.sh')], cwd=REPO,
            env=dict(os.environ, GW_DOS16_EXE=str(args.dos16.resolve()),
                     GW_PKG_OUT=str(dos16_zip)), stdout=subprocess.DEVNULL)
        validate_dos_package(dos16_zip, REPO)
        shutil.copy2(dos16_zip, output)
        with zipfile.ZipFile(output / f'gwbasic-{version}-dos32.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.write(args.dos32, 'GWBASIC.EXE')
            for doc, name in [('LICENSE', 'LICENSE'), ('CHANGES.TXT', 'CHANGES.TXT'),
                              ('RELEASE_NOTES.md', 'RELNOTES.TXT')]:
                archive.writestr(name, (REPO / doc).read_text().replace('\n', '\r\n'))
            archive.writestr('README.TXT', 'GW-BASIC '+version+' for DOS/4GW.\r\n'
                'Provide DOS4GW.EXE on PATH or in this directory; it is not included.\r\n')
        run(['git', 'archive', '--format=tar.gz', f'--prefix=gwbasic-{version}/',
             '-o', str(output / f'gwbasic-{version}-source.tar.gz'), commit], cwd=REPO)
    (output / 'BUILD.json').write_text(metadata_json)
    shutil.copy2(REPO / 'RELEASE_NOTES.md', output)
    checksums = [hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.name
                 for path in sorted(output.iterdir()) if path.is_file()]
    (output / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')
    print(f'Packaged {version} from {commit} into {output}')


if __name__ == '__main__':
    main()
