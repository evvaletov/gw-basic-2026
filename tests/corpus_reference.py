#!/usr/bin/env python3
"""Capture curated transcripts from a user-supplied Microsoft GWBASIC.EXE.

Only terminal I/O is adapted: PRINT goes to file #9, INPUT reads file #8
after writing its prompt, and END closes files and exits DOS. Original
listings remain unchanged. This adapter deliberately supports the constructs
used by fixtures.json, rather than arbitrary BASIC programs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


MARKER = "__CORPUS_DONE__"


def normalize(text):
    return "\n".join(line.rstrip(" \t") for line in text.replace("\r\n", "\n").split("\n"))


def statements(text):
    start = 0
    quoted = False
    for i, char in enumerate(text):
        if char == '"':
            quoted = not quoted
        elif char == ':' and not quoted:
            yield text[start:i]
            start = i + 1
    yield text[start:]


def adapt_statement(text):
    result = []
    pos = 0
    token = re.compile(r'"[^"\r\n]*"|[A-Za-z][A-Za-z0-9.$%!#]*')
    for match in token.finditer(text):
        if match.start() < pos:
            continue
        result.append(text[pos:match.start()])
        word = match.group()
        upper = word.upper()
        pos = match.end()
        if upper.startswith("REM"):
            result.append(text[match.start():])
            return "".join(result)
        if upper == "PRINT":
            if text[pos:].lstrip().startswith('#'):
                raise ValueError("reference adapter does not support existing PRINT#")
            result.append("PRINT #9,")
        elif upper == "INPUT":
            tail = text[pos:].lstrip()
            if tail.startswith(('#', ';')):
                raise ValueError("unsupported INPUT form in reference adapter")
            prompt = ""
            question = True
            if tail.startswith('"'):
                literal = re.match(r'("[^"\r\n]*")\s*([;,])\s*(.*)', tail)
                if not literal:
                    raise ValueError("unsupported INPUT prompt")
                prompt, separator, tail = literal.groups()
                question = separator == ';'
            output = prompt + (';' if prompt and question else '') + ('"? "' if question else '')
            # Console INPUT starts a fresh print column after Enter. INPUT#
            # leaves the output file's column unchanged, which gives TAB after
            # a prompt the wrong layout. Reopening in append mode resets that
            # column without adding terminal echo to the captured transcript.
            result.append(f'PRINT #9,{output};:INPUT #8,{tail}:CLOSE #9:OPEN "OUTPUT.TXT" FOR APPEND AS #9')
            return "".join(result)
        elif upper == "END":
            result.append(f'PRINT #9,"{MARKER}":CLOSE:SYSTEM')
        elif upper in {"SYSTEM", "SHELL", "OPEN", "CLOSE", "LINE"}:
            raise ValueError(f"unsupported {upper} in reference adapter")
        else:
            result.append(word)
    result.append(text[pos:])
    return "".join(result)


def adapt(source):
    lines = ['0 OPEN "OUTPUT.TXT" FOR OUTPUT AS #9:OPEN "INPUT.TXT" FOR INPUT AS #8:ON ERROR GOTO 65500']
    for line in source.splitlines():
        match = re.fullmatch(r'\s*(\d+)\s+(.*)', line)
        if not match or not 0 < int(match[1]) < 65500:
            raise ValueError("expected source lines between 1 and 65499")
        body = match[2]
        # REM text can contain arbitrary colons and keywords.
        if body.upper().startswith("REM"):
            lines.append(line)
        else:
            lines.append(match[1] + ' ' + ':'.join(adapt_statement(part) for part in statements(body)))
    lines.append('65500 PRINT #9,"__CORPUS_ERROR__";ERR;ERL:CLOSE:SYSTEM')
    return '\r\n'.join(lines) + '\r\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='fresh output directory; never overwrites checked-in expectations')
    parser.add_argument('--case', action='append')
    parser.add_argument('--suite', choices=('corpus', 'semantics'), default='corpus')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    corpus = repo / 'tests/corpus'
    manifest = corpus / 'fixtures.json' if args.suite == 'corpus' else repo / 'tests/compatibility/semantics.json'
    cases = json.loads(manifest.read_text())['cases']
    if args.case:
        unknown = set(args.case) - {case['id'] for case in cases}
        if unknown:
            parser.error(f'unknown cases: {sorted(unknown)}')
        cases = [case for case in cases if case['id'] in args.case]
    if args.output.exists():
        parser.error('--output must not already exist')
    args.output.mkdir(parents=True)
    metadata = {'reference_sha256': hashlib.sha256(args.reference_exe.read_bytes()).hexdigest(),
                'adapter_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'method': 'Microsoft GW-BASIC under DOSBox-X; only I/O redirected', 'cases': []}
    for case in cases:
        with tempfile.TemporaryDirectory(prefix='gw-corpus-reference-') as temp:
            drive = Path(temp)
            shutil.copy2(args.reference_exe, drive / 'GWBASIC.EXE')
            source = (corpus / case['source']).read_text() if args.suite == 'corpus' else case['source']
            stdin = (corpus / case['input']).read_text() if args.suite == 'corpus' else case['stdin']
            transformed = adapt(source)
            (drive / 'CASE.BAS').write_bytes(transformed.encode('ascii'))
            (drive / 'INPUT.TXT').write_bytes(stdin.replace('\n', '\r\n').encode('ascii'))
            command = ['flatpak', 'run', '--die-with-parent',
                       f'--filesystem={drive}', 'com.dosbox_x.DOSBox-X',
                       '-conf', str(repo / 'tests/dosbox-compat.conf'),
                       '-c', f'MOUNT C {drive}', '-c', 'C:', '-c', 'GWBASIC.EXE CASE.BAS', '-c', 'EXIT']
            with (args.output / f"{case['id']}.log").open('wb') as log:
                try:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30,
                                            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
                    status = result.returncode
                except subprocess.TimeoutExpired:
                    status = 'timeout'
            (args.output / f"{case['id']}.capture.bas").write_bytes(transformed.encode('ascii'))
            capture = drive / 'OUTPUT.TXT'
            raw = capture.read_bytes() if capture.exists() else b''
            (args.output / f"{case['id']}.raw").write_bytes(raw)
            # DOS text files can contain sector padding after the Ctrl-Z EOF.
            text = raw.split(b'\x1a', 1)[0].decode('ascii').replace('\r\n', '\n')
            if status != 0 or not text.endswith(MARKER + '\n'):
                raise RuntimeError(f"{case['id']}: reference did not complete (exit {status}); tail={text[-200:]!r}")
            text = normalize(text[:-len(MARKER + '\n')])
            (args.output / f"{case['id']}.txt").write_text(text)
            metadata['cases'].append({'id': case['id'], 'sha256': hashlib.sha256(text.encode()).hexdigest(),
                                      'source_sha256': hashlib.sha256(source.encode('ascii')).hexdigest(),
                                      'input_sha256': hashlib.sha256(stdin.encode('ascii')).hexdigest(),
                                      'capture_sha256': hashlib.sha256(transformed.encode('ascii')).hexdigest()})
            print('CAPTURED', case['id'], flush=True)
    (args.output / 'reference.json').write_text(json.dumps(metadata, indent=2) + '\n')


if __name__ == '__main__':
    main()
