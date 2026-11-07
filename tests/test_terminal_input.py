#!/usr/bin/env python3
"""Check real echoing terminals outside the full-screen BASIC editor."""
import argparse
import json
import os
from pathlib import Path
import shlex
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
before=termios.tcgetattr(0)
r=subprocess.run(sys.argv[1:])
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

    def spawn(self, command, rows=25):
        child = pexpect.spawn(sys.executable, ["-c", WRAPPER, *command], cwd=str(self.root),
                              encoding="utf-8", dimensions=(rows, 80), echo=True, timeout=5)
        child.delaybeforesend = 0
        self.addCleanup(child.terminate, force=True)
        screen = pyte.Screen(80, rows)
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=BUILD)
    args, rest = parser.parse_known_args()
    BUILD = args.build.resolve()
    unittest.main(argv=[__file__] + rest)
