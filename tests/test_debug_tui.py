"""Exercise the real curses controller and interpreter in separate terminals."""
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
import pyte

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("GWBASIC_BINARY", ROOT / "build/gwbasic")).absolute()
TOOL = ROOT / "tools/debug_tui.py"
ENV = dict(os.environ, TERM="xterm-256color", PULSE_SERVER="unix:/tmp/gwbasic-no-audio-server")


class Terminal:
    def __init__(self, command, cwd):
        self.master, self.slave = pty.openpty()
        self.screen = pyte.Screen(100, 28)
        self.stream = pyte.Stream(self.screen)
        self.resize(28, 100)
        self.before = termios.tcgetattr(self.slave)
        os.set_blocking(self.master, False)
        self.output = bytearray()
        self.process = subprocess.Popen(command, stdin=self.slave, stdout=self.slave,
                                        stderr=self.slave, cwd=cwd, env=ENV)

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.screen.resize(rows, columns)
        if hasattr(self, "process"):
            self.process.send_signal(signal.SIGWINCH)

    def drain(self):
        try:
            while True:
                data = os.read(self.master, 65536)
                if not data:
                    break
                self.output.extend(data)
                self.stream.feed(data.decode("utf-8", errors="replace"))
                if len(self.output) > 2 * 1024 * 1024:
                    raise AssertionError("terminal output exceeded 2 MiB")
        except BlockingIOError:
            pass
        except OSError as exc:
            if exc.errno != errno.EIO:
                raise

    def send(self, text):
        os.write(self.master, text.encode("ascii"))

    def text(self):
        return "\n".join(self.screen.display)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        os.close(self.master)
        os.close(self.slave)


class DebugTUI(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gw-debug-tui-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.session = self.work / "session"
        self.terminals = []

    def terminal(self, command):
        terminal = Terminal(command, self.work)
        self.terminals.append(terminal)
        self.addCleanup(terminal.close)
        return terminal

    def wait(self, predicate, description):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            for terminal in self.terminals:
                terminal.drain()
            if predicate():
                return
            time.sleep(.02)
        self.fail(description + "\n" + "\n".join(bytes(t.output[-2000:]).decode(errors="replace") for t in self.terminals))

    def trace(self):
        log = self.session / "events"
        return log.read_text() if log.exists() else ""

    def launch(self, source):
        path = self.work / "main.bas"
        path.write_text(source)
        guest = self.terminal([sys.executable, str(TOOL), "run", "--binary", str(BIN),
                               "--session", str(self.session), str(path)])
        self.wait(lambda: (self.session / "session.json").exists(), "session did not start")
        controller = self.terminal([sys.executable, str(TOOL), "attach", str(self.session)])
        self.wait(lambda: "stop 1 " in self.trace(), "first statement did not stop")
        # The event file is produced before curses reads it. Await its PAUSED view.
        self.wait(lambda: "PAUSED" in controller.text(), "controller did not see the stop")
        return guest, controller

    def command(self, controller, text, expected):
        controller.send(":" + text + "\n")
        self.wait(lambda: expected in self.trace(), "missing event: " + expected)

    def test_controls_calls_input_files_and_graphics(self):
        source = '''10 OPEN "R",#1,"data.bin",8
20 FIELD #1,4 AS A$,4 AS B$
30 LSET A$="TEST":LSET B$="1234":PUT #1,1
40 GOSUB 100
50 INPUT N%
60 PRINT "ANSWER";N%;A%
70 SCREEN 1:PSET (4,5),2:C%=POINT(4,5)
80 SCREEN 0:PRINT "PIXEL";C%
90 CLOSE #1:SYSTEM
100 A%=7:RETURN
'''
        guest, controller = self.launch(source)
        self.command(controller, "watch N%", "value N% default integer=0")
        self.command(controller, "watch A$", "value A$ default string length=0")
        self.command(controller, "break 40", "break line=40")
        controller.send("c")
        self.wait(lambda: "stop 2 line=40" in self.trace(), "breakpoint not reached")
        self.wait(lambda: "Stop 2" in controller.text(), "breakpoint not displayed")
        controller.send("s")
        self.wait(lambda: "stop 3 line=100 offset=0 depth=1" in self.trace(), "step did not enter GOSUB")
        self.command(controller, "inspect state", "gosub-depth=1")
        self.command(controller, "break 50", "break line=50")
        controller.send("c")
        self.wait(lambda: "stop 4 line=50" in self.trace(), "return did not reach INPUT")
        self.command(controller, "inspect file 1 0 8", "hex=5445535431323334")
        controller.send("s")
        self.wait(lambda: b"? " in guest.output, "BASIC input prompt missing")
        guest.send("23\n")
        self.wait(lambda: "stop 5 line=60" in self.trace(), "BASIC input was not separate")
        self.command(controller, "checkpoint entered", "checkpoint entered end")
        self.assertIn("value N% integer=23", self.trace())
        self.assertIn("field A file=1 offset=0 length=4", self.trace())
        controller.send("q")
        self.wait(lambda: controller.process.poll() is not None and guest.process.poll() is not None, "detach did not finish")
        self.assertEqual(controller.process.returncode, 0)
        self.assertEqual(guest.process.returncode, 0)
        self.assertEqual(termios.tcgetattr(controller.slave), controller.before)
        self.assertEqual((self.work / "data.bin").read_bytes(), b"TEST1234")
        self.assertRegex(bytes(guest.output), rb"ANSWER\s+23\s+7")
        self.assertRegex(bytes(guest.output), rb"PIXEL\s+2")
        self.assertIn(b"\x1bP", guest.output)  # Sixel stays in the program terminal.
        self.assertNotIn(b"checkpoint entered", guest.output)
        self.assertNotIn(b"\x1bP", controller.output)
        self.assertIn("resume eof", self.trace())

    def test_single_controller_resize_signal_and_eof(self):
        guest, controller = self.launch("10 INPUT A%\n20 PRINT A%:SYSTEM\n")
        self.command(controller, "break 20", "break line=20")
        duplicate = subprocess.run([sys.executable, str(TOOL), "attach", str(self.session)],
                                   capture_output=True, env=ENV, timeout=5)
        self.assertEqual(duplicate.returncode, 2)
        self.assertIn(b"already has a controller", duplicate.stderr)
        controller.resize(5, 20)
        self.wait(lambda: "Resize" in controller.text(), "small terminal not handled")
        controller.resize(28, 100)
        controller.process.send_signal(signal.SIGTERM)
        self.wait(lambda: controller.process.poll() is not None, "controller did not handle SIGTERM")
        self.assertEqual(controller.process.returncode, 130)
        self.assertEqual(termios.tcgetattr(controller.slave), controller.before)
        guest.send("19\n")
        self.wait(lambda: guest.process.poll() is not None, "EOF left BASIC stopped")
        self.assertEqual(guest.process.returncode, 0)
        self.assertEqual(len(re.findall(r"^stop ", self.trace(), re.M)), 1)
        self.assertIn("resume eof", self.trace())
        viewer = self.terminal([sys.executable, str(TOOL), "attach", str(self.session)])
        self.wait(lambda: "FINISHED" in viewer.text(), "finished trace could not be opened")
        viewer.send("q")
        self.wait(lambda: viewer.process.poll() is not None, "finished viewer did not close")

    def test_run_cancelled_before_controller(self):
        source = self.work / "main.bas"
        source.write_text("10 PRINT 7:SYSTEM\n")
        guest = self.terminal([sys.executable, str(TOOL), "run", "--binary", str(BIN),
                               "--session", str(self.session), str(source)])
        self.wait(lambda: (self.session / "session.json").exists(), "session did not start")
        guest.process.send_signal(signal.SIGTERM)
        self.wait(lambda: guest.process.poll() is not None, "cancel left a blocked runner")
        self.assertEqual(json.loads((self.session / "result.json").read_text())["returncode"], 130)
        with self.assertRaises(OSError) as error:
            os.open(self.session / "commands", os.O_WRONLY | os.O_NONBLOCK)
        self.assertEqual(error.exception.errno, errno.ENXIO)

    def test_bad_executable_and_session_reuse(self):
        source = self.work / "main.bas"
        source.write_text("10 SYSTEM\n")
        command = [sys.executable, str(TOOL), "run", "--binary", str(self.work / "absent"),
                   "--session", str(self.session), str(source)]
        failed = subprocess.run(command, capture_output=True, env=ENV, timeout=5)
        self.assertEqual(failed.returncode, 2)
        self.assertEqual(json.loads((self.session / "result.json").read_text())["returncode"], 2)
        marker = self.session / "keep"
        marker.write_bytes(b"preserve")
        reused = subprocess.run(command, capture_output=True, env=ENV, timeout=5)
        self.assertEqual(reused.returncode, 2)
        self.assertIn(b"File exists", reused.stderr)
        self.assertEqual(marker.read_bytes(), b"preserve")
        self.assertEqual(self.session.stat().st_mode & 0o777, 0o700)


if __name__ == "__main__":
    unittest.main()
