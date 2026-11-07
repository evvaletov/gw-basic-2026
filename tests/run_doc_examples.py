#!/usr/bin/env python3
"""Run explicitly marked Markdown examples in disposable POSIX workspaces."""
import argparse
from dataclasses import dataclass
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
DOCUMENTS = ('README.md', 'docs/getting-started.md')
MARKER = re.compile(r'<!-- docs-test: ([a-z][a-z0-9-]*) -->')
MAX_LOG_BYTES = 1024 * 1024


@dataclass(frozen=True)
class Snippet:
    language: str
    text: str
    location: str


@dataclass(frozen=True)
class Example:
    name: str
    shell: tuple[str, ...]
    files: tuple[tuple[str, str], ...] = ()
    executable: str | None = None
    expected: str = 'HELLO FROM BASIC'
    expected_file: str | None = None
    generated_c: str | None = None
    abi: tuple[str, str, str | None] | None = None


EXAMPLES = (
    Example('readme-interpreter', ('readme-run',), expected_file='prime_sieve'),
    Example('readme-native', ('readme-compile',), executable='tests/programs/fibonacci',
            expected_file='fibonacci'),
    Example('readme-compiler', ('readme-compiler',), (('myprog.bas', 'readme-program'),),
            executable='myprog', generated_c='myprog.c'),
    Example('guide-interpreter', ('guide-run',), expected_file='prime_sieve'),
    Example('guide-pipe', ('guide-pipe',), expected='1 4 9 16 25 36 49 64 81 100'),
    Example('guide-compiler', ('guide-compiler',), (('program.bas', 'guide-program'),),
            executable='program', generated_c='stdout'),
    Example('guide-safety', ('guide-safety',), (('program.bas', 'guide-program'),),
            executable='program', generated_c='stdout'),
    Example('guide-c-host', ('guide-object', 'guide-c-link'),
            (('greet.bas', 'guide-greet'), ('driver.c', 'guide-c-driver')),
            executable='greet', abi=('greet.bas', 'driver.c', 'run_basic_greet')),
    Example('guide-fortran-host', ('guide-object', 'guide-fortran-link'),
            (('greet.bas', 'guide-greet'), ('driver.f90', 'guide-fortran-driver')),
            executable='greet'),
    Example('guide-extern', ('guide-extern-build',),
            (('demo.bas', 'guide-extern-program'), ('lib.c', 'guide-extern-library')),
            executable='demo', expected='10 Hello, World! 42', abi=('demo.bas', 'lib.c', None)),
)


def parse_snippets(text, document):
    """Require a named, top-level fenced block immediately after each marker."""
    lines = text.splitlines(keepends=True)
    snippets = {}
    index = 0
    while index < len(lines):
        line = lines[index].rstrip('\r\n')
        if 'docs-test:' not in line:
            index += 1
            continue
        match = MARKER.fullmatch(line)
        location = f'{document}:{index + 1}'
        if not match:
            raise ValueError(f'{location}: malformed docs-test marker')
        name = match[1]
        if name in snippets:
            raise ValueError(f'{location}: duplicate snippet {name}')
        index += 1
        while index < len(lines) and not lines[index].strip():
            index += 1
        opening = re.fullmatch(r'```(bash|basic|c|fortran)\s*', lines[index]) if index < len(lines) else None
        if not opening:
            raise ValueError(f'{location}: expected a bash/basic/c/fortran fenced block')
        index += 1
        start = index
        while index < len(lines) and not re.fullmatch(r'```[ \t]*(?:\r?\n)?', lines[index]):
            if lines[index].lstrip().startswith('```'):
                raise ValueError(f'{location}: malformed closing fence in {name}')
            index += 1
        if index == len(lines) or not ''.join(lines[start:index]).strip():
            raise ValueError(f'{location}: empty or unclosed snippet {name}')
        snippets[name] = Snippet(opening[1], ''.join(lines[start:index]), location)
        index += 1
    return snippets


def load_snippets(repo):
    snippets = {}
    for document in DOCUMENTS:
        for name, snippet in parse_snippets((repo / document).read_text(), document).items():
            if name in snippets:
                raise ValueError(f'{snippet.location}: duplicate snippet {name}')
            snippets[name] = snippet
    required = {name for example in EXAMPLES for name in example.shell}
    required.update(name for example in EXAMPLES for _, name in example.files)
    if set(snippets) != required:
        raise ValueError(f'snippet inventory mismatch: missing={sorted(required - snippets.keys())}, '
                         f'unused={sorted(snippets.keys() - required)}')
    return snippets


def run_command(command, cwd, timeout=30, input_text=None):
    """Bound live logs/runtime and terminate owned descendants on every exit.

    The process group is an ownership boundary, not a security sandbox: a child
    that deliberately starts another session can escape it.
    """
    environment = dict(os.environ, LC_ALL='C', PULSE_SERVER='unix:/tmp/gwbasic-docs-no-audio')
    environment.pop('BASH_ENV', None)
    environment.pop('ENV', None)
    process = subprocess.Popen(command, cwd=cwd, env=environment, start_new_session=True,
                               stdin=subprocess.PIPE if input_text is not None else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output = [bytearray(), bytearray()]
    reason = None
    deadline = time.monotonic() + timeout
    stopped = False

    def stop_group():
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    try:
        with selectors.DefaultSelector() as selector:
            for index, stream in enumerate((process.stdout, process.stderr)):
                os.set_blocking(stream.fileno(), False)
                selector.register(stream, selectors.EVENT_READ, index)
            pending = memoryview(input_text.encode()) if input_text is not None else memoryview(b'')
            if process.stdin:
                if pending:
                    os.set_blocking(process.stdin.fileno(), False)
                    selector.register(process.stdin, selectors.EVENT_WRITE, None)
                else:
                    process.stdin.close()
            while selector.get_map() or process.poll() is None:
                # A successful parent can leave children holding the output
                # pipes. Terminate the group as soon as the parent has exited.
                if process.poll() is not None and not stopped:
                    stop_group()
                    stopped = True
                    deadline = min(deadline, time.monotonic() + 1)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    reason = f'timeout after {timeout}s'
                    break
                for event, _ in selector.select(min(remaining, 0.05)):
                    stream, index = event.fileobj, event.data
                    if index is None:
                        try:
                            pending = pending[os.write(stream.fileno(), pending[:65536]):]
                        except BlockingIOError:
                            continue
                        except BrokenPipeError:
                            pending = memoryview(b'')
                        if not pending:
                            selector.unregister(stream)
                            stream.close()
                        continue
                    try:
                        chunk = os.read(stream.fileno(), 65536)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        selector.unregister(stream)
                        continue
                    available = MAX_LOG_BYTES - len(output[index])
                    output[index].extend(chunk[:available])
                    if len(chunk) > available:
                        reason = f'output exceeds {MAX_LOG_BYTES} bytes'
                        break
                if reason:
                    break
    finally:
        stop_group()
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream and not stream.closed:
                stream.close()
        process.wait()
    decoded = [data.decode('utf-8', errors='replace') for data in output]
    if reason or process.returncode:
        reason = reason or f'exit {process.returncode}'
        raise AssertionError(f'{reason}: {command}\nstdout:\n{decoded[0]}\nstderr:\n{decoded[1]}')
    return decoded[0]


def check_output(actual, expected):
    # These smoke checks verify values and text, not terminal column layout.
    if actual.split() != expected.split():
        difference = '\n'.join(difflib.unified_diff(expected.splitlines(), actual.splitlines(),
                                                   fromfile='expected', tofile='actual'))
        raise AssertionError(f'output mismatch:\n{difference}')


def run_example(example, snippets, repo, build):
    with tempfile.TemporaryDirectory(prefix='gwbasic-docs-') as temporary:
        work = Path(temporary)
        (work / 'build').mkdir()
        for name in ('gwbasic', 'gwbasic-compile', 'libgwrt.a'):
            shutil.copy2(build / name, work / 'build' / name)
        shutil.copytree(repo / 'include', work / 'include')
        (work / 'tests/programs').mkdir(parents=True)
        for name in ('prime_sieve', 'fibonacci'):
            shutil.copy2(repo / f'tests/programs/{name}.bas', work / f'tests/programs/{name}.bas')
        languages = {'.bas': 'basic', '.c': 'c', '.f90': 'fortran'}
        for filename, name in example.files:
            snippet = snippets[name]
            if snippet.language != languages[Path(filename).suffix]:
                raise ValueError(f'{snippet.location}: wrong language for {filename}')
            (work / filename).write_text(snippet.text)
        shell_output = ''
        for name in example.shell:
            snippet = snippets[name]
            if snippet.language != 'bash':
                raise ValueError(f'{snippet.location}: expected bash for {name}')
            try:
                shell_output += run_command(['bash', '--noprofile', '--norc', '-e', '-u',
                                             '-o', 'pipefail', '-c', snippet.text], work)
            except AssertionError as error:
                raise AssertionError(f'{snippet.location} ({name}): {error}') from error
        if example.generated_c:
            generated = shell_output if example.generated_c == 'stdout' else (work / example.generated_c).read_text()
            if '#include "gwrt.h"' not in generated:
                raise AssertionError(f'expected generated runtime C in {example.generated_c}')
        if example.abi:
            # Separate-object linking can hide conflicting C declarations. Compile
            # the documented C and generated prototypes together as a second check.
            basic, c_source, entrypoint = example.abi
            command = ['build/gwbasic-compile', basic]
            if entrypoint:
                command.extend(['--main-name', entrypoint])
            generated = run_command(command, work)
            run_command(['gcc', '-fsyntax-only', '-Iinclude', '-x', 'c', '-'], work,
                        input_text=generated + '\n' + (work / c_source).read_text())
        actual = run_command([str(work / example.executable)], work, timeout=5) if example.executable else shell_output
        expected = (repo / f'tests/expected/{example.expected_file}.expected').read_text() if example.expected_file else example.expected
        check_output(actual, expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=REPO, help='source/docs root (also used by mutation tests)')
    parser.add_argument('--build', type=Path, help='built binaries/runtime; defaults to REPO/build')
    parser.add_argument('--case', choices=[example.name for example in EXAMPLES], action='append')
    parser.add_argument('--report', type=Path, help='write machine-readable results, including failures')
    args = parser.parse_args()
    repo = args.repo.resolve()
    build = (args.build or repo / 'build').resolve()
    report = {'cases': [], 'snippets': {}, 'errors': []}
    try:
        snippets = load_snippets(repo)
        report['snippets'] = {name: {'location': snippet.location,
                                    'sha256': hashlib.sha256(snippet.text.encode()).hexdigest()}
                              for name, snippet in snippets.items()}
        for example in EXAMPLES:
            if args.case and example.name not in args.case:
                continue
            started = time.monotonic()
            result = {'name': example.name, 'status': 'passed'}
            try:
                run_example(example, snippets, repo, build)
            except (AssertionError, OSError, ValueError) as error:
                result.update(status='failed', error=str(error))
            result['seconds'] = round(time.monotonic() - started, 3)
            report['cases'].append(result)
            print(f"{result['status'].upper()} {example.name}", flush=True)
            if 'error' in result:
                print(result['error'], flush=True)
    except (OSError, ValueError) as error:
        report['errors'].append(str(error))
        print(f'ERROR: {error}', flush=True)
    success = bool(report['cases']) and not report['errors'] and all(case['status'] == 'passed' for case in report['cases'])
    report['success'] = success
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"Documentation examples: {sum(case['status'] == 'passed' for case in report['cases'])}/{len(report['cases'])} passed")
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
