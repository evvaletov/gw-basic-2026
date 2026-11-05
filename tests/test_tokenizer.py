#!/usr/bin/env python3
"""Check token-buffer limits and rejection through both command-line tools."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
#include "gwbasic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(const char *text, int expected) {
    unsigned char reference[1024];
    if (gw_crunch(text, reference, sizeof reference) != expected) return 1;
    for (int size = 0; size <= expected + 2; size++) {
        unsigned char *buf = malloc((size_t)size + 2);
        if (!buf) return 2;
        memset(buf, 0xA5, (size_t)size + 2);
        int n = gw_crunch(text, buf + 1, size);
        if (buf[0] != 0xA5 || buf[size + 1] != 0xA5) return 3;
        if (size <= expected) {
            if (n != -1 || (size && buf[1] != 0)) return 4;
        } else if (n != expected || memcmp(buf + 1, reference, expected + 1)) {
            return 5;
        }
        free(buf);
    }
    return 0;
}

int main(void) {
    struct { const char *text; int size; } cases[] = {
        {"", 0}, {"9", 1}, {"10", 2}, {"256", 3},
        {"&H1234", 3}, {"&O777", 3}, {"1!", 5}, {"1#", 9},
        {"1D2", 9}, {"SIN", 2}, {"VARIABLE$", 9}, {"\"abc\"", 5},
        {"REM 1#", 4}, {"DATA 1#:1#", 14}, {".", 1},
        {"99999999999999999999999999", 5}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int status = check(cases[i].text, cases[i].size);
        if (status) { fprintf(stderr, "case %u: %d\n", i, status); return status; }
    }
    char expanded[256] = "10 PRINT ";
    for (int i = 0; i < 40; i++) strcat(expanded, "1#;");
    int status = check(expanded, 405);
    if (status) return status;
    puts("PASS 17 token cases at every boundary capacity");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gw-tokenizer-') as temp:
        temp = Path(temp)
        source = temp / 'probe.c'
        source.write_text(PROBE)
        command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c99', '-I', str(ROOT / 'include')]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        command += [str(source), str(ROOT / 'src/tokenizer.c'), str(ROOT / 'src/tokens.c'), '-o', str(temp / 'probe')]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(temp / 'probe')], check=True, timeout=10)
        program = temp / 'oversize.bas'
        program.write_text('10 PRINT ' + '1#;' * 40 + '\n')
        build = args.build.resolve()
        invocations = [
            ([str(build / 'gwbasic'), str(program)], ''),
            ([str(build / 'gwbasic')], program.read_text() + 'SYSTEM\n'),
            ([str(build / 'gwbasic')], f'LOAD "{program}"\nSYSTEM\n'),
            ([str(build / 'gwbasic-compile'), str(program)], ''),
        ]
        for command, data in invocations:
            result = subprocess.run(command, input=data, text=True, capture_output=True, timeout=10)
            assert 'Line buffer overflow' in result.stdout + result.stderr, result
            assert result.returncode >= 0, result
            assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr, result
            if command[0].endswith('gwbasic-compile'):
                assert result.returncode != 0 and not result.stdout, result
        print('PASS 4 oversized-line entry paths')


if __name__ == '__main__':
    main()
