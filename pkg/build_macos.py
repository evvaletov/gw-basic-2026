#!/usr/bin/env python3
"""Package and test native macOS binaries from a clean source checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile

REPO = Path(__file__).resolve().parents[1]
SYSTEM_PATH = '/usr/bin:/bin:/usr/sbin:/sbin'


def output(command, **kwargs):
    return subprocess.check_output(command, text=True, timeout=120, **kwargs).strip()


def inspect_binary(path, architecture, executable=True):
    """Reject a different architecture or dependencies outside macOS itself."""
    architectures = output(['/usr/bin/lipo', '-archs', str(path)]).split()
    if architectures != [architecture]:
        raise ValueError(f'{path}: expected {architecture}, found {architectures}')
    result = {'architecture': architecture}
    if executable:
        libraries = [line.strip().split(' (', 1)[0] for line in
                     output(['/usr/bin/otool', '-L', str(path)]).splitlines()[1:]]
        if not libraries or any(not lib.startswith(('/usr/lib/', '/System/Library/'))
                                for lib in libraries):
            raise ValueError(f'{path}: non-system or missing dependencies: {libraries}')
        load_commands = output(['/usr/bin/otool', '-l', str(path)])
        minimum = re.findall(r'^\s*minos\s+(\S+)', load_commands, re.MULTILINE)
        if minimum != ['15.0']:
            raise ValueError(f'{path}: expected macOS 15.0 deployment target, found {minimum}')
        result.update(dependencies=libraries, minimum_macos=minimum[0])
    return result


def smoke_test(archive_path, architecture):
    """Exercise the extracted package with only Apple's command-line tools on PATH."""
    env = dict(os.environ, PATH=SYSTEM_PATH, MACOSX_DEPLOYMENT_TARGET='15.0')
    # No source/build directory is used by these runs, including when compiling.
    with tempfile.TemporaryDirectory(prefix='GW BASIC extracted ') as temp:
        root = Path(temp)
        with tarfile.open(archive_path) as archive:
            archive.extractall(root, filter='data')
        package, = root.iterdir()
        work = root / 'user programs'
        work.mkdir()
        program = work / 'package check.bas'
        program.write_text(
            '10 INPUT N\n'
            '20 DIM A(3)\n'
            '30 FOR I=1 TO 3:A(I)=N*I:NEXT I\n'
            '40 IF A(3)<>12 THEN ERROR 5\n'
            '50 IF MID$("APPLE",2,3)<>"PPL" THEN ERROR 5\n'
            '60 OPEN "result.txt" FOR OUTPUT AS #1\n'
            '70 PRINT #1,"MACOS OK"\n'
            '80 CLOSE #1\n'
            '90 OPEN "result.txt" FOR INPUT AS #1\n'
            '100 LINE INPUT #1,S$\n'
            '110 CLOSE #1\n'
            '120 PRINT S$\n'
            '130 END\n')
        runs = []
        for mode in ('interpreter', 'O0', 'O2', 'safe'):
            if mode == 'interpreter':
                command = [str(package / 'bin/gwbasic'), str(program)]
            else:
                flags = ['-O2', '--safe'] if mode == 'safe' else ['-' + mode]
                executable = program.with_suffix('')
                subprocess.run([str(package / 'bin/gwbasic-compile'), str(program),
                                '-c', '--runtime', str(package), *flags],
                               cwd=work, env=env, check=True, timeout=120)
                inspect_binary(executable, architecture)
                command = [str(executable)]
            result = subprocess.run(command, input='4\n', text=True, capture_output=True,
                                    cwd=work, env=env, check=True, timeout=15)
            if result.stdout.split() != ['?', 'MACOS', 'OK'] or result.stderr:
                raise ValueError(f'{mode}: unexpected output {result.stdout!r} {result.stderr!r}')
            if (work / 'result.txt').read_text().strip() != 'MACOS OK':
                raise ValueError(f'{mode}: file I/O smoke failed')
            (work / 'result.txt').unlink()
            runs.append(mode)
        hello = output([str(package / 'bin/gwbasic'), str(package / 'hello.bas')],
                       cwd=work, env=env)
        if hello != 'RELEASE OK':
            raise ValueError('packaged example failed')
        return {'passed': runs, 'example': 'passed', 'path': SYSTEM_PATH,
                'checks': 'input, arrays, strings, file I/O, paths with spaces, native compilation'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=REPO)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--architecture', choices=('arm64', 'x86_64'), required=True)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != args.architecture:
        parser.error('run on a native macOS host of the requested architecture')
    source, build, destination = args.source.resolve(), args.build.resolve(), args.output.resolve()
    subprocess.run(['git', 'diff', '--quiet', 'HEAD'], cwd=source, check=True)
    commit = output(['git', 'rev-parse', 'HEAD'], cwd=source)
    version = re.search(r'^#define GW_VERSION "([0-9.]+)"',
                        (source / 'include/gwbasic.h').read_text(), re.MULTILINE)[1]
    if output([str(build / 'gwbasic'), '--version']) != f'GW-BASIC {version}':
        parser.error('interpreter version differs from source')
    if destination.exists() and any(destination.iterdir()):
        parser.error('--output must be empty or absent')
    binaries = {name: inspect_binary(build / name, args.architecture, name != 'libgwrt.a')
                for name in ('gwbasic', 'gwbasic-compile', 'libgwrt.a')}
    for name, information in binaries.items():
        information['sha256'] = hashlib.sha256((build / name).read_bytes()).hexdigest()
    metadata = {
        'version': version, 'source_commit': commit, 'architecture': args.architecture,
        'packager_commit': output(['git', 'rev-parse', 'HEAD'], cwd=REPO),
        'build_macos': platform.mac_ver()[0], 'minimum_macos': '15.0',
        'compiler': output(['/usr/bin/clang', '--version']), 'binaries': binaries,
        'audio_backend': 'disabled',
        'provenance': 'CI builds the recorded source checkout. The packager checks its clean '
                      'tracked files, interpreter version, architecture and dependencies.',
    }
    if os.environ.get('GITHUB_RUN_ID'):
        metadata['ci_run_url'] = (os.environ.get('GITHUB_SERVER_URL', 'https://github.com') + '/' +
                                  os.environ['GITHUB_REPOSITORY'] + '/actions/runs/' + os.environ['GITHUB_RUN_ID'])
    destination.mkdir(parents=True, exist_ok=True)
    name = f'gwbasic-{version}-macos-{args.architecture}'
    archive_path = destination / (name + '.tar.gz')
    with tempfile.TemporaryDirectory(prefix='gwbasic-macos-') as temp:
        package = Path(temp) / name
        (package / 'bin').mkdir(parents=True)
        (package / 'build').mkdir()
        for binary in ('gwbasic', 'gwbasic-compile'):
            shutil.copy2(build / binary, package / 'bin' / binary)
        shutil.copy2(build / 'libgwrt.a', package / 'build/libgwrt.a')
        for relative in output(['git', 'ls-files', 'include'], cwd=source).splitlines():
            target = package / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / relative, target)
        for doc in ('README.md', 'CHANGES.TXT', 'LICENSE', 'RELEASE_NOTES.md'):
            shutil.copy2(source / doc, package / doc)
        (package / 'hello.bas').write_text('10 PRINT "RELEASE OK"\n20 END\n')
        (package / 'QUICKSTART.txt').write_text(
            'GW-BASIC 2026 for macOS 15 or later (' + args.architecture + ').\n\n'
            'From the extracted directory:\n'
            '  bin/gwbasic hello.bas\n'
            '  bin/gwbasic-compile hello.bas -c --runtime .\n'
            '  ./hello\n\n'
            'Native compilation needs Apple Command Line Tools (xcode-select --install).\n'
            'The interpreter needs no Homebrew libraries. Audio output is disabled.\n'
            'These command-line binaries are not Developer ID signed or notarised.\n'
            'BUILD.json records the source and build environment. The source release notes\n'
            'predate these additional macOS archives.\n')
        (package / 'BUILD.json').write_text(json.dumps(metadata, indent=2) + '\n')
        with tarfile.open(archive_path, 'w:gz') as archive:
            archive.add(package, arcname=name)
    metadata['extracted_package_test'] = smoke_test(archive_path, args.architecture)
    metadata_name = f'BUILD-macos-{args.architecture}.json'
    (destination / metadata_name).write_text(json.dumps(metadata, indent=2) + '\n')
    files = (archive_path, destination / metadata_name)
    (destination / f'SHA256SUMS-macos-{args.architecture}').write_text(''.join(
        hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.name + '\n' for path in files))
    print(f'Packaged and tested {name} from {commit}')


if __name__ == '__main__':
    main()
