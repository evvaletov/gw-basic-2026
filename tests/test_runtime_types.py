#!/usr/bin/env python3
"""Native runtime DEFTYPE with foreign functions and hosted entry lifetime."""
import argparse
import itertools
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
SANITIZE = False
SOURCE = '''10 '$EXTERN Mark(INTEGER) AS INTEGER
20 '$EXTERN Add(INTEGER,INTEGER) AS INTEGER
30 '$EXTERN Echo(STRING) AS STRING
40 '$EXTERN Twice(SINGLE) AS SINGLE
50 '$EXTERN Quarter(DOUBLE) AS DOUBLE
60 '$EXTERN Empty() AS STRING
70 IF 1 THEN DEFINT A
80 A=Add(Mark(1),Mark(2)):PRINT A
90 DEFSTR B:B=Echo("left")+Echo("right"):PRINT B
100 OPEN "FFI.TXT" FOR OUTPUT AS #1:PRINT #1,Echo("file");Mark(3):CLOSE #1
110 DEF FNF(X)=Add(X,Mark(4)):PRINT FNF(10)
120 PRINT USING "###";Add(Mark(5),6)
130 PRINT Twice(1.5);Quarter(2.25);LEN(Empty())
140 ON ERROR GOTO 300
150 A=Add("bad",Mark(6))
160 PRINT Echo("preserved");A:END
300 PRINT ERR:RESUME NEXT
'''
PROVIDER = '''#include "gwrt.h"
#include <stdlib.h>
static int trace;
int16_t Mark(int16_t x) { trace=10*trace+x; return x; }
int16_t Add(int16_t x,int16_t y) { return x+y; }
const char *Echo(const char *s) { return s; }
float Twice(float x) { return 2*x; }
double Quarter(double x) { return x+0.25; }
const char *Empty(void) { return NULL; }
const char *Keep(const char *s,int16_t x) { (void)x; return s; }
int16_t Mix(const char *s,int16_t x) { (void)s; (void)x; abort(); }
int typed_entry(int,char **);
int main(void) {
 for(int i=0;i<2;i++) {
  trace=0;
  if(typed_entry(0,NULL) || trace!=EXPECTED_TRACE || gw.extern_eval ||
     gw.var_count || gw.array_count) return 1;
 }
 return 0;
}
'''


def run(command, directory, allow_stderr=False):
    result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise AssertionError(f'{command}: exit {result.returncode}\n{result.stdout}\n{result.stderr}')
    if result.stderr and not allow_stderr:
        raise AssertionError(result.stderr)
    return result.stdout


def normalized(text):
    return [' '.join(line.split()) for line in text.splitlines() if line.strip()]


class RuntimeTypesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gw-runtime-types-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        pkg = subprocess.run(['pkg-config', '--libs', 'libpulse-simple'], capture_output=True, text=True)
        self.libs = shlex.split(pkg.stdout) if pkg.returncode == 0 else []
        self.cc = shlex.split(os.environ.get('CC', 'cc'))
        if SANITIZE:
            self.cc += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']

    def compile(self, source, provider, flags):
        bas, cfile, host, exe = [self.root / name for name in ('case.bas', 'case.c', 'host.c', 'case')]
        bas.write_text(source)
        host.write_text(provider)
        run([str(BUILD/'gwbasic-compile'), str(bas), '-o', str(cfile),
             '--main-name', 'typed_entry', *flags], self.root, allow_stderr=True)
        code = cfile.read_text()
        self.assertNotIn('gw_run_loop(', code)
        self.assertNotIn('gw_store_line(', code)
        self.assertIn('gwrt_var(', code)
        self.assertIn('_gw_foreign_', code)
        run([*self.cc, flags[0], str(cfile), str(host), str(BUILD/'libgwrt.a'),
             '-I'+str(ROOT/'include'), '-lm', '-pthread', *self.libs, '-o', str(exe)], self.root)
        return exe

    def test_native_and_delegated_foreign_calls_and_reentry(self):
        for level, inline, safe in itertools.product(range(4), (False, True), (False, True)):
            flags = [f'-O{level}'] + (['--inline-arrays'] if inline else []) + (['--safe'] if safe else [])
            with self.subTest(flags=flags):
                exe = self.compile(SOURCE, PROVIDER.replace('EXPECTED_TRACE', '12345'), flags)
                self.assertEqual(normalized(run([str(exe)], self.root)),
                                 ['3', 'leftright', '14', '11', '3 2.5 0', '13', 'preserved 3'] * 2)
                self.assertEqual((self.root/'FFI.TXT').read_text(), 'file 3 \n')

    def test_error_unwind_preserves_storage_and_releases_temporaries(self):
        source = '''10 '$EXTERN Echo(STRING) AS STRING
20 '$EXTERN Mix(STRING,INTEGER) AS INTEGER
30 ON ERROR GOTO 200
40 IF 1 THEN DEFSTR A
50 A="kept":FOR I%=1 TO 180
60 J%=Mix(Echo(STRING$(200,65)),"bad")
70 NEXT I%
80 PRINT Echo(A)+LEFT$(STR$(FRE("")),0);E%;ERR:END
200 E%=E%+1:RESUME NEXT
'''
        for flags in (['-O0'], ['-O3', '--safe', '--inline-arrays']):
            with self.subTest(flags=flags):
                exe = self.compile(source, PROVIDER.replace('EXPECTED_TRACE', '0'), flags)
                self.assertEqual(normalized(run([str(exe)], self.root)), ['kept 180 0'] * 2)

    def test_collection_relocates_native_and_delegated_temporaries(self):
        source = '''10 '$EXTERN Echo(STRING) AS STRING
20 '$EXTERN Keep(STRING,INTEGER) AS STRING
30 IF 1 THEN DEFINT Z
40 X=FRE(""):G$=STRING$(100,88):G$="":PRINT X-FRE("")
50 G$=STRING$(200,88):G$=""
60 PRINT Keep(Echo("first"),FRE("") MOD 100)
70 PRINT USING "&";Keep(Echo("second"),FRE("") MOD 100)
80 DEF FNA$(X$)=Keep(X$,FRE("") MOD 100)
90 X$="saved":PRINT FNA$("arg");X$
100 PRINT USING "&";Echo("left")+LEFT$(STR$(FRE("")),0)+Echo("right")
104 PRINT Echo("left")<=Echo(LEFT$(STR$(FRE("")),0)+"left")
105 PRINT USING "###";Echo("left")<=Echo(LEFT$(STR$(FRE("")),0)+"left")
106 PRINT USING "###";Echo("left")>=Echo(LEFT$(STR$(FRE("")),0)+"left")
107 PRINT USING "###";Echo("left")<>Echo(LEFT$(STR$(FRE("")),0)+"left")
108 PRINT USING "###";Echo("left")=<Echo(LEFT$(STR$(FRE("")),0)+"left")
109 PRINT USING "###";Echo("left")=>Echo(LEFT$(STR$(FRE("")),0)+"left")
110 END
'''
        for flags in (['-O0'], ['-O3', '--safe', '--inline-arrays']):
            with self.subTest(flags=flags):
                exe = self.compile(source, PROVIDER.replace('EXPECTED_TRACE', '0'), flags)
                self.assertEqual(normalized(run([str(exe)], self.root)),
                                 ['0', 'first', 'second', 'argsaved', 'leftright',
                                  '-1', '-1', '-1', '0', '-1', '-1'] * 2)

    def test_expression_unwind_keeps_host_pool_pin(self):
        host, exe = self.root/'pin.c', self.root/'pin'
        host.write_text('''#include "gwrt.h"
int main(void) {
 gwrt_init(); size_t empty=strpool_free();
 strpool_pin(); gwrt_expr_begin(); gwrt_expr_begin();
 (void)gw_str_from_cstr("temporary");
 gwrt_expr_unwind(); strpool_gc();
 if(strpool_free()!=empty-9) return 1;
 strpool_unpin(); strpool_gc();
 if(strpool_free()!=empty) return 2;
 gwrt_shutdown(); return 0;
}
''')
        run([*self.cc, str(host), str(BUILD/'libgwrt.a'), '-I'+str(ROOT/'include'),
             '-lm', '-pthread', *self.libs, '-o', str(exe)], self.root)
        run([str(exe)], self.root)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=BUILD)
    parser.add_argument('--sanitize', action='store_true')
    args, rest = parser.parse_known_args()
    BUILD = args.build.resolve()
    SANITIZE = args.sanitize
    unittest.main(argv=[__file__, *rest])
