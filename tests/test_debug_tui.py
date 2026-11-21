"""Exercise the real curses controller and interpreter in separate terminals."""
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import resource
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
    def __init__(self, command, cwd, file_limit=None, ignore_file_signal=False):
        self.master, self.slave = pty.openpty()
        self.screen = pyte.Screen(100, 28)
        self.stream = pyte.Stream(self.screen)
        self.resize(28, 100)
        self.before = termios.tcgetattr(self.slave)
        os.set_blocking(self.master, False)
        self.output = bytearray()
        def controlling_terminal():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            if file_limit is not None:
                signal.signal(signal.SIGXFSZ, signal.SIG_IGN if ignore_file_signal else signal.SIG_DFL)
                resource.setrlimit(resource.RLIMIT_FSIZE, (file_limit, file_limit))
        self.process = subprocess.Popen(command, stdin=self.slave, stdout=self.slave,
                                        stderr=self.slave, cwd=cwd, env=ENV,
                                        preexec_fn=controlling_terminal)

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.screen.resize(rows, columns)
        if hasattr(self, "process"):
            self.process.send_signal(signal.SIGWINCH)

    def drain(self):
        if self.master is None:
            return
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

    def hangup(self):
        os.close(self.master)
        self.master = None

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        if self.master is not None:
            os.close(self.master)
        os.close(self.slave)


class DebugTUI(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gw-debug-tui-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.session = self.work / "session"
        self.terminals = []

    def terminal(self, command, file_limit=None, ignore_file_signal=False):
        terminal = Terminal(command, self.work, file_limit, ignore_file_signal)
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
        self.command(controller, "watch A%", "value A% default integer=0")
        duplicate = subprocess.run([sys.executable, str(TOOL), "attach", str(self.session)],
                                   capture_output=True, env=ENV, timeout=5)
        self.assertEqual(duplicate.returncode, 2)
        self.assertIn(b"already has a controller", duplicate.stderr)
        controller.resize(5, 20)
        self.wait(lambda: "Resize" in controller.text(), "small terminal not handled")
        controller.resize(18, 50)
        self.wait(lambda: any("value A%" in line for line in controller.screen.display[12:14]),
                  "minimum viewport hides inspection events")
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
        self.wait(lambda: "VIEW ONLY" in viewer.text(), "finished trace could not be opened")
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

    def test_signal_at_readiness_publication_records_result(self):
        source = self.work / "main.bas"
        source.write_text("10 SYSTEM\n")
        code = '''import os, signal, sys
import debug_tui as app
original = app.write_json
def publish_and_cancel(path, data):
    original(path, data)
    if path.name == "session.json":
        os.kill(os.getpid(), signal.SIGTERM)
app.write_json = publish_and_cancel
sys.exit(app.main())
'''
        result = subprocess.run([sys.executable, "-c", code, "run", "--binary", str(BIN),
                                 "--session", str(self.session), str(source)],
                                env=dict(ENV, PYTHONPATH=str(ROOT / "tools")),
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 130, result.stderr)
        self.assertTrue((self.session / "session.json").exists())
        self.assertEqual(json.loads((self.session / "result.json").read_text())["returncode"], 130)

    def launch_waiting(self):
        gate = self.work / "start-basic"
        delayed = self.work / "delayed-basic"
        delayed.write_text("#!" + sys.executable + "\nimport os,sys,time\n"
                           f"while not os.path.exists({str(gate)!r}): time.sleep(.02)\n"
                           f"os.execv({str(BIN)!r}, [{str(BIN)!r}, *sys.argv[1:]])\n")
        delayed.chmod(0o755)
        source = self.work / "main.bas"
        source.write_text('10 PRINT "DETACHED":SYSTEM\n')
        guest = self.terminal([sys.executable, str(TOOL), "run", "--binary", str(delayed),
                               "--session", str(self.session), str(source)])
        self.wait(lambda: (self.session / "session.json").exists(), "session did not start")
        controller = self.terminal([sys.executable, str(TOOL), "attach", str(self.session)])
        self.wait(lambda: "Waiting for BASIC" in controller.text(), "controller did not wait for the FIFO")
        return guest, controller, gate

    def test_signal_as_process_creation_returns_reaps_child(self):
        source = self.work / "main.bas"
        source.write_text("10 SYSTEM\n")
        pidfile = self.work / "child.pid"
        code = '''import os, signal, subprocess, sys
from pathlib import Path
import debug_tui as app
def cancel_on_return(frame, event, arg):
    if frame.f_code is subprocess.Popen.__init__.__code__ and event == "return":
        sys.settrace(None)
        Path(os.environ["CHILD_PID_PATH"]).write_text(str(frame.f_locals["self"].pid))
        os.kill(os.getpid(), signal.SIGTERM)
    return cancel_on_return
sys.settrace(cancel_on_return)
sys.exit(app.main())
'''
        result = subprocess.run([sys.executable, "-c", code, "run", "--binary", str(BIN),
                                 "--session", str(self.session), str(source)],
                                env=dict(ENV, PYTHONPATH=str(ROOT / "tools"), CHILD_PID_PATH=str(pidfile)),
                                capture_output=True, timeout=5)
        pid = int(pidfile.read_text())
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            os.kill(pid, signal.SIGKILL)
            self.fail("cancellation orphaned the child during launch")
        self.assertEqual(result.returncode, 130, result.stderr)
        self.assertEqual(json.loads((self.session / "result.json").read_text())["returncode"], 130)
        with self.assertRaises(OSError) as error:
            os.open(self.session / "commands", os.O_WRONLY | os.O_NONBLOCK)
        self.assertEqual(error.exception.errno, errno.ENXIO)

    @unittest.skipUnless(Path("/proc/self/statm").exists(), "Linux address-space measurement")
    def test_source_size_rejection_uses_bounded_memory(self):
        source = self.work / "oversized.bas"
        with source.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024)
        code = '''import os, resource, sys
from pathlib import Path
import debug_tui as app
baseline = int(Path("/proc/self/statm").read_text().split()[0]) * os.sysconf("SC_PAGE_SIZE")
limit = baseline + 16 * 1024 * 1024
resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
sys.exit(app.main())
'''
        result = subprocess.run([sys.executable, "-c", code, "run", "--session", str(self.session), str(source)],
                                env=dict(ENV, PYTHONPATH=str(ROOT / "tools")), capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(b"at most 1 MiB", result.stderr)
        self.assertFalse(self.session.exists())

    def test_detach_before_interpreter_opens_fifo(self):
        guest, controller, gate = self.launch_waiting()
        controller.send("q")
        self.wait(lambda: controller.process.poll() is not None, "early detach did not close")
        gate.touch()
        self.wait(lambda: guest.process.poll() is not None, "early detach stranded BASIC at FIFO open")
        self.assertEqual(guest.process.returncode, 0)
        self.assertIn("resume eof", self.trace())
        self.assertIn(b"DETACHED", guest.output)

    def test_killed_controller_before_interpreter_opens_fifo(self):
        guest, controller, gate = self.launch_waiting()
        controller.process.kill()
        controller.process.wait(timeout=5)
        gate.touch()
        self.wait(lambda: guest.process.poll() is not None, "controller crash stranded BASIC at FIFO open")
        self.assertEqual(guest.process.returncode, 0)
        self.assertIn("resume eof", self.trace())
        self.assertIn(b"DETACHED", guest.output)

    def test_cancel_at_stop_terminates_guest_and_restores_both_terminals(self):
        guest, controller = self.launch('10 PRINT "MUST NOT EXECUTE":SYSTEM\n')
        controller.send("x")
        self.wait(lambda: guest.process.poll() is not None and controller.process.poll() is not None,
                  "cancel did not terminate the run")
        self.assertEqual(guest.process.returncode, 130)
        self.assertEqual(controller.process.returncode, 0)
        self.assertNotIn(b"MUST NOT EXECUTE", guest.output)
        self.assertEqual(termios.tcgetattr(guest.slave), guest.before)
        self.assertEqual(termios.tcgetattr(controller.slave), controller.before)

    def test_inspector_quit_key_restores_terminal_and_resumes_guest(self):
        guest, controller = self.launch('10 PRINT "RESUMED":SYSTEM\n')
        controller.send("\x1c")  # The terminal's literal Ctrl-\\ quit character.
        self.wait(lambda: controller.process.poll() is not None and guest.process.poll() is not None,
                  "quit key did not detach and resume")
        self.assertEqual(controller.process.returncode, 130)
        self.assertEqual(guest.process.returncode, 0)
        self.assertIn(b"RESUMED", guest.output)
        self.assertEqual(termios.tcgetattr(controller.slave), controller.before)
        self.assertEqual(controller.output.count(b"\x1b[?1049h"), controller.output.count(b"\x1b[?1049l"))

    def test_direct_monitor_default_signals_restore_raw_terminal(self):
        source = self.work / "main.bas"
        source.write_text('10 PRINT "MUST NOT EXECUTE":SYSTEM\n')
        for number in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT, signal.SIGQUIT):
            with self.subTest(signal=number):
                fifo, log = self.work / f"commands-{number}", self.work / f"events-{number}"
                os.mkfifo(fifo)
                guest = self.terminal([str(BIN), "--debug-script", str(fifo), "--debug-output", str(log), str(source)])
                fd = None
                def connect():
                    nonlocal fd
                    try:
                        fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                        return True
                    except OSError as exc:
                        if exc.errno != errno.ENXIO:
                            raise
                        return False
                self.wait(connect, "monitor did not open its FIFO")
                self.addCleanup(os.close, fd)
                self.wait(lambda: log.exists() and "stop 1 " in log.read_text(), "first stop missing")
                guest.process.send_signal(number)
                self.wait(lambda: guest.process.poll() is not None, "native monitor did not terminate")
                self.assertEqual(guest.process.returncode, -number)
                self.assertNotIn(b"MUST NOT EXECUTE", guest.output)
                self.assertNotIn(b"\x1b[?1049l", guest.output)
                self.assertEqual(termios.tcgetattr(guest.slave), guest.before)

    def test_terminate_runner_at_stop_restores_guest_terminal(self):
        guest, controller = self.launch('10 PRINT "MUST NOT EXECUTE":SYSTEM\n')
        guest.process.terminate()
        self.wait(lambda: guest.process.poll() is not None, "runner did not terminate at stop")
        self.assertEqual(guest.process.returncode, 130)
        self.assertNotIn(b"MUST NOT EXECUTE", guest.output)
        self.assertEqual(termios.tcgetattr(guest.slave), guest.before)
        controller.send("q")
        self.wait(lambda: controller.process.poll() is not None, "controller did not close")

    def test_closed_guest_terminal_still_records_exit(self):
        guest, controller = self.launch('10 PRINT "MUST NOT EXECUTE":SYSTEM\n')
        guest.hangup()
        self.wait(lambda: guest.process.poll() is not None, "closed terminal stranded the runner")
        result = json.loads((self.session / "result.json").read_text())
        self.assertEqual(result["returncode"], guest.process.returncode)
        self.assertIn(result["returncode"], (2, 130))
        self.wait(lambda: "BASIC exited" in controller.text(), "inspector did not receive the exit status")
        controller.send("q")
        self.wait(lambda: controller.process.poll() is not None, "inspector did not close")

    def test_cancel_after_end_restores_guest_screen(self):
        guest, controller = self.launch('10 PRINT "FINISHED"\n20 END\n')
        controller.send("c")
        self.wait(lambda: b"\x1b[?1049h" in guest.output and b"Ok" in guest.output,
                  "BASIC did not enter its command screen")
        controller.send("x")
        self.wait(lambda: guest.process.poll() is not None and controller.process.poll() is not None,
                  "cancel at the command screen did not finish")
        self.assertEqual(guest.process.returncode, 130)
        self.assertEqual(guest.output.count(b"\x1b[?1049h"), guest.output.count(b"\x1b[?1049l"))
        self.assertIn(b"\x1b[0m\x1b[0 q\x1b[?25h", guest.output)
        self.assertEqual(termios.tcgetattr(guest.slave), guest.before)

    def test_native_editor_exits_restore_screen_and_terminal(self):
        source = self.work / "main.bas"
        source.write_text("10 END\n")
        for number in (signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT, None):
            with self.subTest(signal=number):
                guest = self.terminal([str(BIN), str(source)])
                self.wait(lambda: b"\x1b[?1049h" in guest.output and b"Ok" in guest.output and
                          not termios.tcgetattr(guest.slave)[3] & termios.ICANON,
                          "BASIC did not enter its command screen")
                if number is None:
                    guest.send("SYSTEM\r")
                else:
                    guest.process.send_signal(number)
                self.wait(lambda: guest.process.poll() is not None, "editor ignored termination")
                self.assertEqual(guest.process.returncode, -number if number is not None else 0)
                self.assertEqual(guest.output.count(b"\x1b[?1049h"), guest.output.count(b"\x1b[?1049l"))
                self.assertIn(b"\x1b[0m\x1b[0 q\x1b[?25h", guest.output)
                self.assertEqual(termios.tcgetattr(guest.slave), guest.before)

    def test_monitor_read_survives_handled_signal_after_editor_run(self):
        source = self.work / "main.bas"
        source.write_text("10 END\n")
        fifo = self.work / "commands"
        log = self.work / "events"
        os.mkfifo(fifo)
        guest = self.terminal([str(BIN), "--debug-script", str(fifo), "--debug-output", str(log), str(source)])
        fd = None
        def connect():
            nonlocal fd
            try:
                fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                return True
            except OSError as exc:
                if exc.errno != errno.ENXIO:
                    raise
                return False
        self.wait(connect, "monitor did not open its FIFO")
        self.addCleanup(os.close, fd)
        os.write(fd, b"break 10\ncontinue\n")
        self.wait(lambda: b"Ok" in guest.output, "BASIC did not return to its editor")
        guest.send("RUN\r")
        self.wait(lambda: log.exists() and "stop 2 " in log.read_text(), "second RUN did not stop")
        os.write(fd, b"inspect ")
        time.sleep(.05)
        guest.process.send_signal(signal.SIGINT)
        time.sleep(.05)
        guest.drain()
        self.assertIsNone(guest.process.poll(), guest.output)
        self.assertNotIn(b"debugger I/O failure", guest.output)
        os.write(fd, b"A%\n")
        self.wait(lambda: "value A% default integer=0" in log.read_text(), "partial command was lost")
        guest.process.terminate()
        self.wait(lambda: guest.process.poll() is not None, "BASIC did not exit")
        self.assertEqual(guest.process.returncode, -signal.SIGTERM)

    def test_native_monitor_log_failure_restores_terminal(self):
        source = self.work / "main.bas"
        source.write_text("10 PRINT 7:SYSTEM\n")
        script = self.work / "commands"
        script.write_text("continue\n")
        for limit in (16, 64):
            for ignored in (False, True):
                with self.subTest(limit=limit, ignored=ignored):
                    guest = self.terminal([str(BIN), "--debug-script", str(script), "--debug-output",
                                           str(self.work / f"events-{limit}-{ignored}"), str(source)],
                                          file_limit=limit, ignore_file_signal=ignored)
                    self.wait(lambda: guest.process.poll() is not None, "log failure did not exit")
                    self.assertEqual(guest.process.returncode, 2)
                    self.assertEqual(guest.output.count(b"debugger I/O failure"), 1)
                    self.assertEqual(termios.tcgetattr(guest.slave), guest.before)
        source.write_text("10 END\n")
        script.write_text("break 10\ncontinue\n")
        guest = self.terminal([str(BIN), "--debug-script", str(script), "--debug-output",
                               str(self.work / "events-editor"), str(source)], file_limit=120)
        self.wait(lambda: b"Ok" in guest.output and not termios.tcgetattr(guest.slave)[3] & termios.ICANON,
                  "first run did not reach the editor")
        guest.send("RUN\r")
        self.wait(lambda: guest.process.poll() is not None, "editor log failure did not exit")
        self.assertEqual(guest.process.returncode, 2)
        self.assertEqual(guest.output.count(b"debugger I/O failure"), 1)
        self.assertGreaterEqual(guest.output.rfind(b"\x1b[?1049l"), 0)
        self.assertGreater(guest.output.rfind(b"debugger I/O failure"), guest.output.rfind(b"\x1b[?1049l"))
        self.assertEqual(termios.tcgetattr(guest.slave), guest.before)

    def test_failed_terminal_setup_allows_retry(self):
        source = self.work / "main.bas"
        source.write_text("10 SYSTEM\n")
        guest = self.terminal([sys.executable, str(TOOL), "run", "--binary", str(BIN),
                               "--session", str(self.session), str(source)])
        self.wait(lambda: (self.session / "session.json").exists(), "session did not start")
        failed = subprocess.run([sys.executable, str(TOOL), "attach", str(self.session)],
                                capture_output=True, env=dict(ENV, TERM="gwbasic-invalid-terminal"), timeout=5)
        self.assertEqual(failed.returncode, 2)
        self.assertFalse((self.session / "controller-started").exists())
        self.assertFalse((self.session / "detached").exists())
        controller = self.terminal([sys.executable, str(TOOL), "attach", str(self.session)])
        self.wait(lambda: "PAUSED" in controller.text(), "valid terminal could not retry the attachment")
        controller.send("q")
        self.wait(lambda: guest.process.poll() is not None, "retried run did not finish")
        self.assertEqual(guest.process.returncode, 0)

    def test_one_off_inspections_expire_at_the_next_stop(self):
        guest, controller = self.launch("10 A%=1\n20 A%=2\n30 SYSTEM\n")
        self.command(controller, "inspect A%", "value A% default integer=0")
        self.command(controller, "watch B%", "value B% default integer=0")
        controller.send("s")
        self.wait(lambda: "Stop 2 " in controller.text(), "next stop not displayed")
        self.wait(lambda: any("B% default" in row for row in controller.screen.display[9:13]), "watch did not refresh")
        self.assertFalse(any("A% default" in row for row in controller.screen.display[9:13]))
        controller.send("q")
        self.wait(lambda: guest.process.poll() is not None, "run did not finish")

    def test_last_duplicate_label_and_recent_values(self):
        guest, controller = self.launch('10 PRINT "OLD"\n10 PRINT "NEW"\n15 REM NOTE\f10 PRINT "FAKE"\n20 SYSTEM\n')
        self.wait(lambda: '> 10 PRINT "NEW"' in controller.text(), "discarded duplicate label highlighted")
        for index in range(16):
            self.command(controller, f"inspect I{index}%", f"value I{index}% default integer=0")
        controller.send(":inspect i0%\n")
        self.wait(lambda: self.trace().upper().count("VALUE I0%") == 2, "reinspection missing")
        self.command(controller, "inspect I16%", "value I16% default integer=0")
        self.wait(lambda: "I0% default" in "\n".join(controller.screen.display[9:13]) and
                  "I16% default" in "\n".join(controller.screen.display[9:13]), "recent values evicted or hidden")
        controller.send("q")
        self.wait(lambda: guest.process.poll() is not None, "duplicate-label program did not finish")
        self.assertIn(b"NEW", guest.output)
        self.assertNotIn(b"OLD", guest.output)
        self.assertNotIn(b"FAKE", guest.output)

    def test_killed_controller_cannot_replay_a_live_stop(self):
        guest, controller = self.launch("10 INPUT A%\n20 PRINT A%:SYSTEM\n")
        controller.send("c")
        self.wait(lambda: b"? " in guest.output, "BASIC input prompt missing")
        controller.process.kill()
        controller.process.wait(timeout=5)
        viewer = self.terminal([sys.executable, str(TOOL), "attach", str(self.session)])
        self.wait(lambda: "VIEW ONLY" in viewer.text(), "reopened controller treated an old stop as live")
        viewer.send("s")
        self.wait(lambda: "Controls are available when BASIC is paused" in viewer.text(), "read-only control was not rejected")
        guest.send("31\n")
        self.wait(lambda: guest.process.poll() is not None, "BASIC input was blocked after controller crash")
        self.assertEqual(guest.process.returncode, 0)
        self.assertEqual(len(re.findall(r"^stop ", self.trace(), re.M)), 1)
        viewer.send("q")
        self.wait(lambda: viewer.process.poll() is not None, "viewer did not close")

    def test_unmapped_stop_does_not_highlight_unrelated_entry_line(self):
        (self.work / "later.bas").write_text('99 PRINT "LATER":SYSTEM\n')
        guest, controller = self.launch('10 CHAIN "later.bas"\n20 SYSTEM\n')
        controller.send("s")
        self.wait(lambda: "line=99" in self.trace(), "CHAIN did not stop in the replacement program")
        self.wait(lambda: "line 99 " in controller.text(), "replacement stop not displayed")
        self.assertFalse(any(line.startswith("> ") for line in controller.screen.display[3:8]))
        controller.send("q")
        self.wait(lambda: guest.process.poll() is not None, "CHAIN program did not finish")
        self.assertIn(b"LATER", guest.output)

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
