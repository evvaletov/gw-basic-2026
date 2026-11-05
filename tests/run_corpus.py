#!/usr/bin/env python3
"""Compare curated BASIC transcripts across the interpreter and compiler modes.

--audit builds every vendored listing without executing it. A successful build
alone does not establish runtime compatibility.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import signal
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
CORPUS = REPO / 'tests/corpus'
MODES = {'O0': ['-O0'], 'O1': ['-O1'], 'O2': ['-O2'], 'O3': ['-O3'],
         'inline': ['-O2', '--inline-arrays'], 'safe': ['-O2', '--safe']}


def normalize(text):
    return '\n'.join(line.rstrip(' \t') for line in text.replace('\r\n', '\n').split('\n'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def contained(root, relative):
    path = (root / relative).resolve()
    if root.resolve() not in path.parents or not path.is_file():
        raise ValueError(f'invalid corpus file: {relative}')
    return path


def load_cases(corpus, selected=None):
    cases = json.loads((corpus / 'fixtures.json').read_text())['cases']
    references = json.loads((corpus / 'reference.json').read_text())
    refs = {entry['id']: entry for entry in references['cases']}
    ids = [case['id'] for case in cases]
    if not ids or len(ids) != len(set(ids)):
        raise ValueError('fixture IDs must be nonempty and unique')
    if selected and set(selected) - set(ids):
        raise ValueError(f'unknown cases: {sorted(set(selected) - set(ids))}')
    for case in cases:
        if not case['id'] or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in case['id']):
            raise ValueError('invalid fixture ID')
        ref = refs[case['id']]
        for key, hash_key in [('source', 'source_sha256'), ('input', 'input_sha256'), ('expected', 'sha256')]:
            if digest(contained(corpus, case[key])) != ref[hash_key]:
                raise ValueError(f"{case['id']}: {key} differs from its reviewed reference capture")
    return [case for case in cases if not selected or case['id'] in selected]


def verify_sources(corpus):
    sources = json.loads((corpus / 'sources.json').read_text())
    if not sources['files']:
        raise ValueError('empty source inventory')
    for name, metadata in sources['files'].items():
        if digest(contained(corpus, name)) != metadata['sha256']:
            raise ValueError(f'{name}: differs from pinned upstream source')
    return sources


def limit_files():
    # Bound logs and files written by a faulty child. Generated C also has the
    # compiler's smaller one-megabyte limit.
    resource.setrlimit(resource.RLIMIT_FSIZE, (8 * 1024 * 1024, 8 * 1024 * 1024))


def stop_group(process, sig):
    try:
        os.killpg(process.pid, sig)
    except ProcessLookupError:
        pass


def execute(command, work, name, stdin='', timeout=5):
    work.mkdir(parents=True, exist_ok=True)
    out_path, err_path = work / f'{name}.out', work / f'{name}.err'
    timed_out = False
    with out_path.open('wb') as out, err_path.open('wb') as err:
        try:
            process = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE, stdout=out, stderr=err,
                                       start_new_session=True, preexec_fn=limit_files,
                                       env=dict(os.environ, PULSE_SERVER='unix:/tmp/gwbasic-corpus-no-audio'))
        except OSError as error:
            return {'status': 'spawn-error', 'exit_code': None, 'stderr': str(error)}, ''
        try:
            process.communicate(stdin.encode(), timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            stop_group(process, signal.SIGTERM)
            try:
                process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                stop_group(process, signal.SIGKILL)
                process.communicate()
        finally:
            # Fixtures cannot leave background children behind, including when
            # the parent exits during the timeout grace period.
            stop_group(process, signal.SIGKILL)
    output = out_path.read_bytes().decode(errors='replace')
    errors = err_path.read_bytes().decode(errors='replace')
    status = 'timeout' if timed_out else 'ok' if process.returncode == 0 else f'exit:{process.returncode}'
    return {'status': status, 'exit_code': process.returncode, 'stderr': errors[-2000:]}, output


def compare(actual, expected):
    if normalize(actual) == normalize(expected):
        return {'status': 'match'}
    diff = ''.join(difflib.unified_diff(normalize(expected).splitlines(True),
                                      normalize(actual).splitlines(True), fromfile='reference', tofile='actual'))
    return {'status': 'mismatch', 'diff': diff[:2500]}


def build_program(source, flags, work, build, cc, libs):
    cfile, exe = work / 'program.c', work / 'program'
    result, _ = execute([str(build / 'gwbasic-compile'), str(source), '-o', str(cfile),
                         '--max-output-size', '1', *flags], work, 'transpile', timeout=20)
    phases = {'transpile': result}
    if result['status'] != 'ok':
        return phases, None
    if not cfile.is_file() or cfile.stat().st_size == 0:
        phases['transpile']['status'] = 'missing-output'
        return phases, None
    result, _ = execute([*cc, flags[0], str(cfile), str(build / 'libgwrt.a'),
                         '-I' + str(REPO / 'include'), '-lm', '-pthread', *libs, '-o', str(exe)],
                        work, 'cc', timeout=60)
    phases['cc'] = result
    if result['status'] != 'ok':
        return phases, None
    if not exe.is_file() or not os.access(exe, os.X_OK):
        phases['cc']['status'] = 'missing-output'
        return phases, None
    return phases, exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=REPO / 'build')
    parser.add_argument('--case', action='append', help='run one named case (repeatable)')
    parser.add_argument('--mode', action='append', choices=list(MODES))
    parser.add_argument('--report', type=Path, help='write the complete JSON matrix')
    parser.add_argument('--audit', action='store_true', help='build every listing without executing it')
    args = parser.parse_args()
    if args.audit and (args.case or args.mode):
        parser.error('--audit cannot be combined with --case or --mode')
    build = args.build.resolve()
    try:
        sources = verify_sources(CORPUS)
        cases = [] if args.audit else load_cases(CORPUS, args.case)
        for name in ('gwbasic', 'gwbasic-compile', 'libgwrt.a'):
            if not (build / name).is_file():
                raise ValueError(f'missing build artifact: {build / name}')
        try:
            pkg = subprocess.run(['pkg-config', '--libs', 'libpulse-simple'], capture_output=True, text=True, timeout=10)
            libs = shlex.split(pkg.stdout) if pkg.returncode == 0 else []
        except FileNotFoundError:
            libs = []
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc:
            raise ValueError('CC must name a C compiler')
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    modes = args.mode or list(MODES)
    report = {'kind': 'build-audit' if args.audit else 'reference-and-parity',
              'upstream_revision': sources['revision'], 'rows': [], 'passed': 0, 'failed': 0}
    with tempfile.TemporaryDirectory(prefix='gwbasic-corpus-') as temp:
        root = Path(temp)
        if args.audit:
            for name in sources['files']:
                phases, exe = build_program(CORPUS / name, ['-O0'], root / Path(name).stem, build, cc, libs)
                report['rows'].append({'source': name, **phases})
                ok = exe is not None
                report['passed' if ok else 'failed'] += 1
                print(f"{'PASS' if ok else 'FAIL'} {name}: " + ' '.join(f'{k}={v["status"]}' for k, v in phases.items()), flush=True)
        else:
            for case in cases:
                source = CORPUS / case['source']
                stdin = (CORPUS / case['input']).read_text()
                expected = (CORPUS / case['expected']).read_bytes().decode()
                row = {'case': case['id'], 'source': case['source'], 'compiled': {}}
                process, interpreted = execute([str(build / 'gwbasic'), str(source)],
                                                root / case['id'] / 'interpreter', 'run', stdin)
                row['interpreter'] = {'process': process, 'reference': compare(interpreted, expected)}
                ok = process['status'] == 'ok' and not process['stderr'] and row['interpreter']['reference']['status'] == 'match'
                labels = ['interpreter=' + ('match' if ok else 'FAIL')]
                for mode in modes:
                    work = root / case['id'] / mode
                    phases, exe = build_program(source, MODES[mode], work, build, cc, libs)
                    row['compiled'][mode] = phases
                    passed = False
                    if exe:
                        process, compiled = execute([str(exe)], work, 'run', stdin)
                        phases.update(run=process, reference=compare(compiled, expected),
                                      parity=compare(compiled, interpreted))
                        passed = process['status'] == 'ok' and not process['stderr'] and \
                            phases['reference']['status'] == 'match' and phases['parity']['status'] == 'match'
                    labels.append(mode + '=' + ('match' if passed else 'FAIL'))
                    ok = ok and passed
                report['rows'].append(row)
                report['passed' if ok else 'failed'] += 1
                print(f"{'PASS' if ok else 'FAIL'} {case['id']}: " + ' '.join(labels), flush=True)
                if not ok:
                    print(json.dumps(row, indent=2), flush=True)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{report['kind']}: {report['passed']} passed, {report['failed']} failed")
    return int(report['failed'] != 0)


if __name__ == '__main__':
    raise SystemExit(main())
