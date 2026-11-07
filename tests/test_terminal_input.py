#!/usr/bin/env python3
"""Check real echoing terminals outside the full-screen BASIC editor."""
import argparse
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest

import pexpect
import pyte

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
WRAPPER = '''import json,subprocess,sys,termios
from pathlib import Path
before=termios.tcgetattr(0)
r=subprocess.Popen(sys.argv[1:])
Path('child.tmp').write_text(str(r.pid))
Path('child.tmp').replace('child.pid')
r.wait()
after=termios.tcgetattr(0)
print("\\nTERMINAL_RESTORED="+str(before==after),flush=True)
sys.exit(r.returncode if before==after else 99)
'''
KEYS = {"left": "\x1b[D", "right": "\x1b[C", "home": "\x1b[H",
        "end": "\x1b[F", "backspace": "\x7f", "delete": "\x1b[3~",
        "insert": "\x1b[2~"}


class TerminalInputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gw-terminal-input-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, source, mode):
        bas = self.root / "case.bas"
        bas.write_text(source)
        if mode == "interpreter":
            return [str(BUILD / "gwbasic"), str(bas)]
        flags = ["-O0"] if mode == "O0" else ["-O2", "--safe"]
        cfile, exe = self.root / "case.c", self.root / "case"
        subprocess.run([str(BUILD / "gwbasic-compile"), str(bas), "-o", str(cfile), *flags],
                       capture_output=True, check=True, timeout=20)
        libs = subprocess.run(["pkg-config", "--libs", "libpulse-simple"], capture_output=True,
                              text=True, timeout=10)
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), flags[0], str(cfile),
                        str(BUILD / "libgwrt.a"), "-I" + str(ROOT / "include"), "-lm", "-pthread",
                        *shlex.split(libs.stdout if libs.returncode == 0 else ""), "-o", str(exe)],
                       capture_output=True, check=True, timeout=30)
        return [str(exe)]

    def spawn(self, command, rows=25, cols=80):
        (self.root / 'child.pid').unlink(missing_ok=True)
        child = pexpect.spawn(sys.executable, ["-c", WRAPPER, *command], cwd=str(self.root),
                              encoding="utf-8", dimensions=(rows, cols), echo=True, timeout=5)
        deadline = time.monotonic() + 5
        while not (self.root / 'child.pid').exists():
            if time.monotonic() > deadline: self.fail('child PID handshake timed out')
            time.sleep(0.01)
        child.delaybeforesend = 0
        self.addCleanup(child.terminate, force=True)
        screen = pyte.Screen(cols, rows)
        stream = pyte.Stream(screen)
        return child, screen, stream

    def finish(self, child, stream, expected_exit=0):
        child.expect(pexpect.EOF)
        stream.feed(child.before)
        child.close()
        self.assertEqual(child.exitstatus, expected_exit)
        self.assertIn("TERMINAL_RESTORED=True", child.before)

    def test_original_console_references(self):
        cases = json.loads((ROOT / "tests/compatibility/input-console.json").read_text())["cases"]
        for case in cases:
            for mode in ("interpreter", "O0", "safe"):
                with self.subTest(case=case["id"], mode=mode):
                    # SYSTEM exits the file-launched interpreter before it opens
                    # the editor. All input, output and position probes are unchanged.
                    command = self.command(case["source"].replace(":END", ":SYSTEM"), mode)
                    # The reference has 24 usable rows plus its key bar. The
                    # plain terminal uses all rows and has no key bar.
                    child, screen, stream = self.spawn(command, rows=24)
                    child.expect("PRE")
                    stream.feed(child.before + child.after)
                    self.assertTrue(child.waitnoecho(timeout=5))
                    child.send(case["answer"])
                    for key in case["keys"]:
                        child.send(KEYS[key])
                    child.send("\r")
                    self.finish(child, stream)
                    for row, expected in case["rows"].items():
                        self.assertEqual(screen.display[int(row)].rstrip(), expected)
                    output = next(self.root.glob("O*.TXT"))
                    lines = output.read_text().splitlines()
                    self.assertEqual([int(x) for x in lines[0].split()], case["positions"])
                    self.assertEqual(lines[1], case["value"])
                    output.unlink()

    def test_relative_cursor_typeahead_and_split_escape(self):
        source = ('10 PRINT "KEPT":PRINT "ANSWER:";:LINE INPUT ;S$\n'
                  '20 PRINT "<";S$;">":INPUT ;"NEXT";T$\n'
                  '30 PRINT "[";T$;"]":SYSTEM\n')
        for mode in ("interpreter", "O0", "safe"):
            with self.subTest(mode=mode):
                child, screen, stream = self.spawn(self.command(source, mode))
                child.expect("ANSWER:"); stream.feed(child.before + child.after)
                self.assertTrue(child.waitnoecho(timeout=5))
                child.send("ABC\x1b")
                time.sleep(.03)
                child.send("[D\x1b[DX\rSECOND\r")
                self.finish(child, stream)
                self.assertEqual(screen.display[0].rstrip(), "KEPT")
                self.assertTrue(any("<AXC>" in line for line in screen.display))
                self.assertTrue(any("[SECOND]" in line for line in screen.display))
                self.assertNotIn("\x1b[?1049h", child.before)

    def test_break_eof_and_capacity_restore_terminal(self):
        source = '10 LINE INPUT ;"READ:";S$\n20 PRINT "LEN=";LEN(S$):SYSTEM\n'
        for mode in ("interpreter", "O0", "safe"):
            for answer, marker in [("\x03", "Break"), ("\x04", "LEN= 0"),
                                   ("A" * 270 + "\r", "LEN= 255")]:
                with self.subTest(mode=mode, marker=marker):
                    child, screen, stream = self.spawn(self.command(source, mode))
                    child.expect("READ:"); stream.feed(child.before + child.after)
                    self.assertTrue(child.waitnoecho(timeout=5))
                    child.send(answer)
                    if marker == "Break" and mode == "interpreter":
                        # An interrupted file-launched interpreter returns to its
                        # regular editor; finish it through the public command.
                        child.expect("Ok")
                        self.assertIn("Break", child.before)
                        stream.feed(child.before + child.after)
                        child.send("SYSTEM\r")
                    # Native programs report an interrupted run as exit 1.
                    self.finish(child, stream, expected_exit=1 if marker == "Break" and mode != "interpreter" else 0)
                    if marker != "Break" or mode != "interpreter":
                        self.assertTrue(any(marker in line for line in screen.display))

        # Restored termios must not hide a failing child process.
        child, _, _ = self.spawn([sys.executable, "-c", "raise SystemExit(7)"])
        child.expect(pexpect.EOF)
        child.close()
        self.assertEqual(child.exitstatus, 7)

    def test_pipe_and_file_input_keep_stdio_behavior(self):
        source = ('10 INPUT ;"Q";S$:PRINT "[";S$;"]"\n'
                  '20 OPEN "data.txt" FOR INPUT AS #1:LINE INPUT #1,T$\n'
                  '30 PRINT T$:CLOSE:SYSTEM\n')
        (self.root / "data.txt").write_text("FILE DATA\n")
        for mode in ("interpreter", "O0", "safe"):
            command = self.command(source, mode)
            for answer, value in (("PIPE DATA", "PIPE DATA"), ('""', ""), ("   ", "")):
                with self.subTest(mode=mode, answer=answer):
                    result = subprocess.run(command, cwd=self.root, input=answer + "\n",
                                            text=True, capture_output=True, timeout=5)
                    self.assertEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, f"Q? [{value}]\nFILE DATA\n")
                    self.assertEqual(result.stderr, "")

    def test_escape_preserves_the_next_queued_key(self):
        source = '10 LINE INPUT ;"READ:";S$\n20 PRINT "VALUE=[";S$;"]":SYSTEM\n'
        for mode in ("interpreter", "O0", "safe"):
            for keys, value in (("ABC\x1b\r", ""), ("ABC\x1bX\r", "X")):
                with self.subTest(mode=mode, value=value):
                    child, screen, stream = self.spawn(self.command(source, mode))
                    child.expect("READ:"); stream.feed(child.before + child.after)
                    self.assertTrue(child.waitnoecho(timeout=5))
                    child.send(keys)
                    self.finish(child, stream)
                    self.assertTrue(any(f"VALUE=[{value}]" in line for line in screen.display))

    def test_tabs_modified_keys_and_mixed_readers(self):
        source = '10 PRINT "A";CHR$(9);"Q:";:LINE INPUT ;S$\n20 PRINT "VALUE=[";S$;"]":SYSTEM\n'
        for mode in ("interpreter", "O0", "safe"):
            with self.subTest(mode=mode):
                child, screen, stream = self.spawn(self.command(source, mode))
                child.expect('Q:'); stream.feed(child.before + child.after)
                self.assertTrue(child.waitnoecho(timeout=5))
                child.send('ABC\x1b[1;2DX\x1b[3;2~\r')
                self.finish(child, stream)
                self.assertTrue(screen.display[0].startswith('A       Q:ABXVALUE=[ABX]'))

        source = 'PRINT "READ:";:A$=INPUT$(1)\nLINE INPUT ;S$\nPRINT "VALUE=[";A$;":";S$;"]":SYSTEM\n'
        for mode in ("interpreter", "O0", "safe"):
            with self.subTest(mode=mode):
                child, screen, stream = self.spawn(self.command(source, mode))
                child.expect('READ:'); stream.feed(child.before + child.after)
                self.assertTrue(child.waitnoecho(timeout=5))
                child.send('ABC\r')
                self.finish(child, stream)
                self.assertTrue(any('VALUE=[A:BC]' in line for line in screen.display))

    def test_input_chars_console_references(self):
        cases = json.loads((ROOT / "tests/compatibility/input-functions.json").read_text())["input_chars"]
        keys = {"a": "a", "b": "b", "shift-y": "Y", "ret": "\r", "ctrl-z": "\x1a", "left": "\x1b[D", "shift-z": "Z"}
        for case in cases:
            for mode in ("interpreter", "O0", "safe"):
                with self.subTest(case=case["id"], mode=mode):
                    child, screen, stream = self.spawn(self.command(case["source"], mode))
                    child.expect(r"READ[0-9]:"); stream.feed(child.before + child.after)
                    self.assertTrue(child.waitnoecho(timeout=5))
                    child.send("".join(keys[key] for key in case["keys"]))
                    # No final Enter: the function must finish at the requested count.
                    self.finish(child, stream)
                    output = next(self.root.glob("I*.TXT"))
                    self.assertEqual([int(x) for x in output.read_text().split()], case["values"])
                    output.unlink()
                    self.assertRegex(screen.display[0], r"^READ[0-9]:DONE[0-9]")

    def test_input_chars_break_and_pipe_bytes(self):
        source = '10 PRINT "READ:";:A$=INPUT$(3)\n20 PRINT LEN(A$):SYSTEM\n'
        for mode in ("interpreter", "O0", "safe"):
            with self.subTest(mode=mode):
                child, _, stream = self.spawn(self.command(source, mode))
                child.expect('READ:'); stream.feed(child.before + child.after)
                self.assertTrue(child.waitnoecho(timeout=5))
                child.send('A\x03')
                if mode == "interpreter":
                    child.expect('Ok')
                    self.assertIn('Break', child.before)
                    child.send('SYSTEM\r')
                self.finish(child, stream, expected_exit=0 if mode == "interpreter" else 1)
            pipe = ('10 ON ERROR GOTO 100\n20 A$=INPUT$(3)\n'
                    '30 FOR I=1 TO LEN(A$):PRINT ASC(MID$(A$,I,1));:NEXT I:SYSTEM\n'
                    '100 PRINT ERR;ERL:SYSTEM\n')
            command = self.command(pipe, mode)
            for data, expected in ((b'A\x00B', [65, 0, 66]), (b'A', [62, 20])):
                result = subprocess.run(command, cwd=self.root, input=data, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0)
                self.assertEqual([int(x) for x in result.stdout.split()], expected)
                self.assertEqual(result.stderr, b'')

    def test_randomize_console_references(self):
        cases = json.loads((ROOT / "tests/compatibility/input-functions.json").read_text())["randomize"]
        for case in cases:
            for mode in ("interpreter", "O0", "safe"):
                with self.subTest(case=case["id"], mode=mode):
                    child, _, stream = self.spawn(self.command(case["source"], mode))
                    for answer in case["answers"]:
                        child.expect_exact('Random number seed (-32768 to 32767)? ')
                        self.assertTrue(child.waitnoecho(timeout=5))
                        child.send(answer + '\r')
                    self.finish(child, stream)
                    output = next(self.root.glob('R*.TXT'))
                    self.assertEqual([int(x) for x in output.read_text().split()], case["values"])
                    output.unlink()

    def drain(self, child, stream):
        while True:
            try:
                stream.feed(child.read_nonblocking(65536, timeout=0.15))
            except pexpect.TIMEOUT:
                return

    def test_long_answer_in_tiny_viewports(self):
        source = ('10 PRINT "ANSWER:";:LINE INPUT ;S$\n'
                  '20 OPEN "CAP.TXT" FOR OUTPUT AS #1\n'
                  '30 PRINT #1,LEN(S$);POS(0);CSRLIN:PRINT #1,S$:CLOSE:SYSTEM\n')
        for mode in ("interpreter", "O0", "safe"):
            command = self.command(source, mode)
            for rows, cols in ((3, 12), (1, 8), (1, 1)):
                with self.subTest(mode=mode, rows=rows, cols=cols):
                    child, screen, stream = self.spawn(command, rows=rows, cols=cols)
                    self.assertTrue(child.waitnoecho(timeout=5))
                    child.send('A'*255)
                    self.drain(child, stream)
                    child.send(KEYS['home']+'X')
                    self.drain(child, stream)
                    if cols > 2:
                        self.assertTrue(screen.display[-1].startswith('>X'), screen.display)
                    child.send(KEYS['end']+KEYS['backspace']+'\r')
                    self.finish(child, stream)
                    saved = (self.root/'CAP.TXT').read_text().splitlines()
                    length, col, row = map(int, saved[0].split())
                    self.assertEqual(length, 254)
                    self.assertTrue(1 <= col <= cols and 1 <= row <= rows, saved[0])
                    self.assertEqual(saved[1], 'X'+'A'*253)

    def test_resize_during_and_before_prompt(self):
        source = ('10 PRINT "ANSWER:";:LINE INPUT ;S$\n'
                  '20 OPEN "CAP.TXT" FOR OUTPUT AS #1\n'
                  '30 PRINT #1,LEN(S$);POS(0);CSRLIN:PRINT #1,S$:CLOSE:SYSTEM\n')
        for mode in ("interpreter", "O0", "safe"):
            with self.subTest(mode=mode):
                child, screen, stream = self.spawn(self.command(source, mode))
                child.expect('ANSWER:'); stream.feed(child.before+child.after)
                self.assertTrue(child.waitnoecho(timeout=5))
                child.send('A'*200); self.drain(child, stream)
                screen.resize(lines=3, columns=12); child.setwinsize(3, 12)
                child.expect_exact('\x1b[3;1H'); stream.feed(child.before+child.after)
                self.drain(child, stream)
                child.send(KEYS['home']+'Z'); self.drain(child, stream)
                self.assertTrue(screen.display[-1].startswith('>Z'), screen.display)
                screen.resize(lines=5, columns=40); child.setwinsize(5, 40)
                child.expect_exact('\x1b[5;1H'); stream.feed(child.before+child.after)
                self.drain(child, stream)
                child.send('\r'); self.finish(child, stream)
                saved=(self.root/'CAP.TXT').read_text().splitlines()
                self.assertEqual([int(x) for x in saved[0].split()], [200, 3, 5])
                self.assertEqual(saved[1], 'Z'+'A'*199)
            before = source.replace('10 PRINT', '5 A$=INPUT$(1)\n10 PRINT')
            with self.subTest(mode=mode, before_prompt=True):
                child, screen, stream = self.spawn(self.command(before, mode))
                self.assertTrue(child.waitnoecho(timeout=5))
                screen.resize(lines=3, columns=12); child.setwinsize(3, 12)
                child.send('X'); child.expect('ANSWER:'); stream.feed(child.before+child.after)
                child.send('BEFORE\r'); self.finish(child, stream)
                self.assertEqual((self.root/'CAP.TXT').read_text().splitlines()[1], 'BEFORE')

    def test_cp437_display_keeps_byte_editing(self):
        source=('10 PRINT "ANSWER:";:LINE INPUT ;S$\n'
                '20 OPEN "BYTES.TXT" FOR OUTPUT AS #1:PRINT #1,LEN(S$)\n'
                '30 FOR I=1 TO LEN(S$):PRINT #1,ASC(MID$(S$,I,1)):NEXT I\n'
                '40 CLOSE:SYSTEM\n')
        for mode in ("interpreter", "O0", "safe"):
            with self.subTest(mode=mode):
                child, screen, stream=self.spawn(self.command(source, mode))
                child.expect('ANSWER:');stream.feed(child.before+child.after)
                self.assertTrue(child.waitnoecho(timeout=5))
                child.send('é');self.drain(child,stream)
                self.assertEqual(screen.display[0].rstrip(), 'ANSWER:'+b'\xc3\xa9'.decode('cp437'))
                self.assertEqual(screen.cursor.x, 9)
                child.send(KEYS['backspace']+'X\r');self.finish(child,stream)
                self.assertEqual([int(x) for x in (self.root/'BYTES.TXT').read_text().split()], [2,195,88])

    def test_redirected_high_bytes_are_unchanged(self):
        for mode in ("interpreter", "O0", "safe"):
            with self.subTest(mode=mode):
                command=self.command('10 PRINT CHR$(195);CHR$(169)\n20 SYSTEM\n',mode)
                result=subprocess.run(command,input=b'',capture_output=True,timeout=5,cwd=self.root)
                self.assertEqual((result.returncode,result.stdout,result.stderr),(0,b'\xc3\xa9\n',b''))

    def test_termination_signals_restore_terminal(self):
        for mode in ("interpreter", "O0", "safe"):
            for statement in ('LINE INPUT S$', 'S$=INPUT$(3)'):
                command=self.command('10 PRINT "READY:";:'+statement+'\n20 SYSTEM\n',mode)
                for number in (signal.SIGTERM, signal.SIGHUP):
                    with self.subTest(mode=mode, statement=statement, signal=number):
                        child, _, stream=self.spawn(command)
                        child.expect('READY:');stream.feed(child.before+child.after)
                        self.assertTrue(child.waitnoecho(timeout=5))
                        os.kill(int((self.root/'child.pid').read_text()),number)
                        self.finish(child,stream,expected_exit=256-number)

    def test_embedding_host_signal_handlers_are_preserved(self):
        host=self.root/'host.c';exe=self.root/'host'
        host.write_text('''#include "hal.h"
#include <signal.h>
#include <stdio.h>
static volatile sig_atomic_t count;
static void caught(int sig) { (void)sig; count++; }
int main(void) {
 struct sigaction action={0}, after;
 action.sa_handler=caught; sigemptyset(&action.sa_mask); sigaddset(&action.sa_mask,SIGUSR1);
 sigaction(SIGHUP,&action,0); signal(SIGTERM,SIG_IGN);
 hal_ops_t *h=hal_posix_create(); h->init(); char text[16];
 printf("READY:"); fflush(stdout); int n=h->read_input(text,sizeof text,0);
 sigaction(SIGHUP,0,&after);
 int ok=n==2 && count==1 && after.sa_handler==caught && sigismember(&after.sa_mask,SIGUSR1);
 sigaction(SIGTERM,0,&after);ok=ok && after.sa_handler==SIG_IGN;
 h->shutdown();return ok?0:1;
}
''')
        subprocess.run([*shlex.split(os.environ.get('CC','cc')),str(host),'-I'+str(ROOT/'include'),
                        str(BUILD/'libgwrt.a'),'-o',str(exe)],check=True,capture_output=True)
        child, _, stream=self.spawn([str(exe)])
        child.expect('READY:');stream.feed(child.before+child.after)
        self.assertTrue(child.waitnoecho(timeout=5))
        os.kill(int((self.root/'child.pid').read_text()),signal.SIGHUP)
        os.kill(int((self.root/'child.pid').read_text()),signal.SIGTERM)
        child.send('OK\r');self.finish(child,stream)


    def test_repeated_raw_activation_uses_current_host_settings(self):
        host, exe = self.root/'raw.c', self.root/'raw'
        host.write_text('''#include "hal.h"
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
int main(void) {
 struct termios original, expected, actual;
 tcgetattr(0,&original);
 hal_ops_t *h=hal_posix_create(); h->init();
 h->enable_raw(); h->disable_raw();
 expected=original; expected.c_iflag^=ICRNL;
 tcsetattr(0,TCSANOW,&expected); h->enable_raw();
 printf("READY:"); fflush(stdout); char byte;
 int n=h->read_chars(&byte,1); h->disable_raw(); tcgetattr(0,&actual);
 int ok=n==1 && byte=='X' && actual.c_iflag==expected.c_iflag &&
        actual.c_lflag==expected.c_lflag;
 tcsetattr(0,TCSANOW,&original); h->shutdown(); return ok?0:1;
}
''')
        subprocess.run([*shlex.split(os.environ.get('CC','cc')),str(host),'-I'+str(ROOT/'include'),
                        str(BUILD/'libgwrt.a'),'-o',str(exe)],check=True,capture_output=True)
        child, _, stream=self.spawn([str(exe)])
        child.expect('READY:');stream.feed(child.before+child.after)
        self.assertTrue(child.waitnoecho(timeout=5))
        child.send('X');self.finish(child,stream)



if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=BUILD)
    args, rest = parser.parse_known_args()
    BUILD = args.build.resolve()
    unittest.main(argv=[__file__] + rest)
