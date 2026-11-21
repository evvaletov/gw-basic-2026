"""Debug controls must observe the real interpreter without changing it."""
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = os.environ.get("GWBASIC_BINARY", str(ROOT / "build/gwbasic"))
ENV = dict(os.environ, PULSE_SERVER="unix:/tmp/gwbasic-no-audio-server")


class Debug(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="gw-debug-")
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.serial = 0

    def run_basic(self, source, commands=None, data=b""):
        self.serial += 1
        src = self.work / "main.bas"; src.write_text(source)
        args = [BIN]
        log = self.work / f"log{self.serial}"
        if commands is not None:
            script = self.work / "commands"; script.write_text(commands)
            args += ["--debug-script", str(script), "--debug-output", str(log)]
        p = subprocess.run(args+[str(src)], input=data, capture_output=True, cwd=self.work, env=ENV, timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p, log.read_text() if log.exists() else ""

    def test_colon_if_calls_loops_and_repeatability(self):
        source = """10 A%=1:A%=A%+1:IF A%=2 THEN GOSUB 100
20 FOR I%=1 TO 2:A%=A%+I%:NEXT I%
30 PRINT A%:SYSTEM
100 A%=A%+10:RETURN
"""
        commands = "capabilities\nwatch A%\nwatch I%\ncheckpoint start\n" + "step\n"*25
        normal, _ = self.run_basic(source)
        debug, log = self.run_basic(source, commands)
        self.assertEqual(debug.stdout, normal.stdout)
        self.assertEqual(debug.stdout.split(), [b"15"])
        offsets = re.findall(r"stop \d+ line=10 offset=(\d+)", log)
        self.assertEqual(len(offsets), 4)  # assignment, assignment, IF, GOSUB
        self.assertEqual(len(set(offsets)), 4)
        self.assertRegex(log, r"line=100 offset=0 depth=1")
        self.assertIn("default integer=0", log)
        self.assertIn("checkpoint start", log)
        _, repeated = self.run_basic(source, commands)
        self.assertEqual(log, repeated)

    def test_missing_watches_do_not_allocate_or_call_rng(self):
        source = "10 A=RND(-123):PRINT A\n20 PRINT RND(1)\n30 SYSTEM\n"
        normal, _ = self.run_basic(source)
        debug, log = self.run_basic(source,
            "watch NEVER%\nwatch MISSING$\nwatch X(2)\ninspect RND(1+0)\ninspect FNX(1)\ninspect state\n"
            "checkpoint one\ncheckpoint two\nstep\nstep\ninspect state\ncontinue\n")
        self.assertEqual(debug.stdout, normal.stdout)
        self.assertIn("state variables=0 arrays=0", log)
        self.assertIn("array-not-allocated", log)
        self.assertIn("error invalid-reference RND(1+0)", log)
        self.assertIn("value MISSING$ default string length=0", log)

    def test_array_types_and_input_are_separate(self):
        source = "10 OPTION BASE 1\n20 DIM A%(2,3)\n30 INPUT N%\n40 A%(2,3)=N%\n50 PRINT A%(2,3):SYSTEM\n"
        normal, _ = self.run_basic(source, data=b"23\n")
        debug, log = self.run_basic(source,
            "watch N%\nwatch A%(2,3)\nstep\nstep\ninspect A%(0,1)\nstep\nstep\ncheckpoint read\ncontinue\n", b"23\n")
        self.assertEqual(debug.stdout, normal.stdout)
        self.assertIn("error bounds", log)
        self.assertIn("bounds=1:2,1:3 integer=23", log)

    def test_breakpoint_statement_offset_and_eof(self):
        source = "10 A%=1:A%=2:A%=3\n20 SYSTEM\n"
        _, all_stops = self.run_basic(source, "step\n"*10)
        offset = re.findall(r"line=10 offset=(\d+)", all_stops)[2]
        _, log = self.run_basic(source, f"break 10:{offset}\ncontinue\ncheckpoint third\n")
        self.assertEqual(len(re.findall(r"^stop ", log, re.M)), 2)
        self.assertIn(f"line=10 offset={offset}", log)
        self.assertIn("resume eof", log)

    def test_distinct_multidimensional_elements(self):
        source = "10 DIM A%(2,3):A%(1,2)=17:A%(2,1)=29\n20 PRINT A%(1,2);A%(2,1):SYSTEM\n"
        normal, _ = self.run_basic(source)
        debug, log = self.run_basic(source,
            "break 20\ncontinue\ninspect A%(1,2)\ninspect A%(2,1)\ncontinue\n")
        self.assertEqual(debug.stdout, normal.stdout)
        self.assertIn("value A%(1,2) bounds=0:2,0:3 integer=17", log)
        self.assertIn("value A%(2,1) bounds=0:2,0:3 integer=29", log)

    def test_field_and_file_inspection_is_pure(self):
        source = """10 OPEN "R",#1,"data.bin",8
20 FIELD #1,4 AS A$,4 AS B$
30 LSET A$="TEST":LSET B$="1234"
40 PUT #1,1
50 GET #1,1
60 PRINT A$;B$
70 CLOSE #1:SYSTEM
"""
        normal, _ = self.run_basic(source)
        expected = (self.work / "data.bin").read_bytes()
        debug, log = self.run_basic(source,
            "break 60\ncontinue\nwatch A$\nwatch B$\ninspect file 1 0 8\ninspect file 1 0 8\ncheckpoint field\ncontinue\n")
        self.assertEqual(debug.stdout, normal.stdout)
        self.assertEqual((self.work / "data.bin").read_bytes(), expected)
        self.assertEqual(expected, b"TEST1234")
        self.assertIn("field A file=1 offset=0 length=4", log)
        self.assertIn("field B file=1 offset=4 length=4", log)
        self.assertIn("hex=5445535431323334", log)
        self.assertEqual(len(set(re.findall(r"file 1 mode=.*", log))), 1)

    def test_screen_state_survives_monitor(self):
        source = "10 SCREEN 1\n20 PSET (4,5),2\n30 A%=POINT(4,5)\n40 SCREEN 0:PRINT A%:SYSTEM\n"
        normal, _ = self.run_basic(source)
        debug, log = self.run_basic(source, "watch A%\n" + "checkpoint video\nstep\n"*8)
        self.assertEqual(debug.stdout, normal.stdout)
        self.assertIn(b"2", debug.stdout)
        self.assertIn("integer=2", log)

    def test_errors_and_long_commands_do_not_change_program(self):
        normal, _ = self.run_basic("10 PRINT 7:SYSTEM\n")
        debug, log = self.run_basic("10 PRINT 7:SYSTEM\n",
            "x"*300 + "\nnext\nrestore x\nwatch A(999999999999999)\ninspect file 9999999999999 0 8\ncapabilities extra\nunwatch\nstep\rjunk\ncontinue\n")
        self.assertEqual(debug.stdout, normal.stdout)
        self.assertIn("error command-too-long", log)
        self.assertIn("error unsupported-command", log)
        self.assertIn("error watch", log)
        self.assertIn("error file-request", log)
        self.assertIn("error invalid-command", log)
        self.assertEqual(len(re.findall(r"^stop ", log, re.M)), 1)

    def test_watch_removal_case_and_invalid_breakpoint(self):
        _, log = self.run_basic("10 A%=1\n20 SYSTEM\n",
            "watch a%\nwatch A%\nunwatch a%\nbreak :1\nstep\ncheckpoint empty\ncontinue\n")
        self.assertIn("error breakpoint", log)
        self.assertNotIn("value A%", log.split("stop 2")[1])
        commands = "".join(f"watch I{i}\n" for i in range(16)) + "unwatch I15\nwatch I16\ncontinue\n"
        _, log = self.run_basic("10 SYSTEM\n", commands)
        self.assertIn("value I16 default single=0", log)
        self.assertNotIn("error watch", log)

    def test_command_read_and_log_write_failures(self):
        src = self.work / "main.bas"; src.write_text("10 PRINT 7:SYSTEM\n")
        args = [BIN, "--debug-script", str(self.work), "--debug-output", str(self.work / "log"), str(src)]
        p = subprocess.run(args, input=b"", capture_output=True, cwd=self.work, env=ENV, timeout=15)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn(b"debugger I/O failure", p.stderr)
        self.assertNotIn(b"7", p.stdout)
        script = self.work / "commands"; script.write_text("continue\n")
        args[2] = str(script); args[4] = str(self.work / "full")
        def limited_file():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (16, 16))
        p = subprocess.run(args, input=b"", capture_output=True, cwd=self.work, env=ENV, timeout=15, preexec_fn=limited_file)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn(b"debugger I/O failure", p.stderr)
        self.assertNotIn(b"7", p.stdout)


if __name__ == "__main__":
    unittest.main()
