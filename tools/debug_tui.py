#!/usr/bin/env python3
"""Control the BASIC interpreter from a second POSIX terminal."""
import argparse
from collections import deque
import curses
import errno
import fcntl
import json
import os
from pathlib import Path
import re
import select
import shlex
import signal
import stat
import subprocess
import sys
import termios


def interrupted(signum, frame):
    raise KeyboardInterrupt


def write_json(path, data):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data) + "\n")
    temporary.replace(path)


def run(args):
    with args.source.open("rb") as source_file:
        source = source_file.read(1024 * 1024 + 1)
    if len(source) > 1024 * 1024 or b"\0" in source or any(b > 127 for b in source):
        raise ValueError("the source pane needs ASCII BASIC, at most 1 MiB")
    session = args.session.absolute()
    session.mkdir(mode=0o700)  # A fresh directory prevents accidental log reuse.
    child = None
    result = 2
    cancelled = False
    saved_terminal = termios.tcgetattr(sys.stdin) if sys.stdin.isatty() else None

    def cancel(signum, frame):
        nonlocal cancelled
        cancelled = True

    # A signal must not interrupt the assignment after Popen has made a child.
    previous = {sig: signal.signal(sig, cancel)
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT)}
    try:
        (session / "source.bas").write_bytes(source)
        os.mkfifo(session / "commands", 0o600)
        write_json(session / "session.json", {"source": str(args.source.absolute())})
        command = shlex.join([sys.executable, str(Path(__file__).resolve()), "attach", str(session)])
        print(f"Debugger: in another terminal run\n  {command}\n"
              "Waiting for the controller; x cancels there, q detaches.", file=sys.stderr, flush=True)
        if not cancelled:
            child = subprocess.Popen([str(args.binary.absolute()), "--debug-script", str(session / "commands"),
                                      "--debug-output", str(session / "events"), str(session / "source.bas")])
        while child is not None:
            if cancelled or (session / "cancel").exists():
                result = 130
                break
            try:
                result = child.wait(timeout=.1)
                break
            except subprocess.TimeoutExpired:
                # A killed controller cannot publish its detach marker. The
                # kernel lock establishes whether the first inspector is gone.
                if (session / "controller-started").exists() and not (session / "events").exists():
                    with (session / "controller.lock").open("a") as lock:
                        try:
                            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except BlockingIOError:
                            pass
                        else:
                            (session / "detached").touch()
                # Detaching before the interpreter opens its FIFO still means
                # resume: briefly connect a writer so fopen can return EOF.
                if (session / "detached").exists() and not (session / "events").exists():
                    try:
                        fd = os.open(session / "commands", os.O_WRONLY | os.O_NONBLOCK)
                        os.close(fd)
                    except OSError as exc:
                        if exc.errno != errno.ENXIO:
                            raise
        if cancelled:
            result = 130
    except KeyboardInterrupt:
        result = 130
    finally:
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if saved_terminal is not None:
            try:
                termios.tcsetattr(sys.stdin, termios.TCSANOW, saved_terminal)
            except termios.error as exc:
                try:
                    os.write(sys.stderr.fileno(), f"debugger: cannot restore program terminal: {exc}\n".encode())
                except OSError:
                    pass  # The terminal may have closed; still record the result.
                result = 2
        result = result if result >= 0 else 128 - result
        write_json(session / "result.json", {"returncode": result})
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    return result


def printable(text):
    # Source names and diagnostics must not inject terminal control sequences.
    return "".join(c if 32 <= ord(c) < 127 else "?" for c in text)


class Controller:
    def __init__(self, session):
        self.session = session
        self.fd = None
        self.log = None
        self.pending = b""
        self.events = deque(maxlen=120)
        self.values = {}
        self.scroll = 0
        self.location = None
        self.paused = False
        self.cancelling = False
        self.started = False
        self.notice = None
        self.finished = (session / "controller-started").exists()
        self.message = ("Read-only session; start a fresh run to debug again" if self.finished else
                        "Waiting for BASIC to open the command channel")
        self.source = [line.rstrip("\r") for line in (session / "source.bas").read_bytes().decode("ascii").split("\n")]
        self.rows = {int(m[1]): row for row, line in enumerate(self.source)
                     if (m := re.match(r"[ \t]*(\d+)", line))}

    def close(self):
        cancelled = (self.session / "cancel").exists()
        if cancelled and self.fd is not None:
            # Cancellation owns the writer until BASIC is gone. An inspector
            # interruption must not turn a pending cancel into FIFO EOF/resume.
            previous = {sig: signal.signal(sig, signal.SIG_IGN)
                        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT)}
            try:
                poller = select.poll()
                poller.register(self.fd, select.POLLERR | select.POLLHUP)
                while not (self.session / "result.json").exists():
                    if poller.poll(100):
                        break
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
        if self.started and not self.finished and not cancelled:
            (self.session / "detached").touch()
        if self.fd is not None:
            os.close(self.fd)  # EOF disables the monitor, including breakpoints.
            self.fd = None
        if self.log:
            self.log.close()

    def poll(self):
        result = self.session / "result.json"
        notice = None
        if result.exists():
            self.finished = True
            self.paused = False
            code = json.loads(result.read_text())["returncode"]
            notice = f"BASIC exited with status {code}; q closes the inspector"
        elif (self.session / "detached").exists():
            self.finished = True
            self.paused = False
            notice = "Controller detached; start a fresh session to debug again"
        if notice is not None and notice != self.notice:
            self.message = self.notice = notice
        if self.fd is None and not self.finished:
            try:
                self.fd = os.open(self.session / "commands", os.O_WRONLY | os.O_NONBLOCK)
                self.message = "Connected; waiting for the first stop"
            except OSError as exc:
                if exc.errno != errno.ENXIO:
                    raise
        if self.fd is not None and not self.finished:
            poller = select.poll()
            poller.register(self.fd, select.POLLERR | select.POLLHUP)
            if poller.poll(0):
                self.finished = True
                self.paused = False
                self.message = "BASIC closed the command channel; q closes the inspector"
        if self.log is None:
            try:
                self.log = (self.session / "events").open("rb")
            except FileNotFoundError:
                return
        # Bound work and retained history even when the trace is large.
        self.pending += self.log.read(65536)
        records = self.pending.split(b"\n")
        self.pending = records.pop()
        if len(self.pending) > 8192:
            raise ValueError("debug event exceeds 8192 bytes")
        for record in records:
            text = record.decode("ascii", errors="replace").rstrip("\r")
            self.events.append(text)
            stop = re.fullmatch(r"stop (\d+) line=(\d+) offset=(\d+) depth=(\d+)", text)
            if stop:
                self.location = tuple(int(x) for x in stop.groups())
                self.values.clear()
                self.paused = self.fd is not None and not self.finished and not self.cancelling
                if self.paused:
                    self.message = "Paused; offset counts token bytes"
            elif text.startswith("value "):
                _, name, value = text.split(" ", 2)
                name = name.upper()
                self.values.pop(name, None)
                self.values[name] = value
                if len(self.values) > 16:
                    del self.values[next(iter(self.values))]

    def send(self, command):
        if not self.paused or self.fd is None or self.finished:
            self.message = "Controls are available when BASIC is paused"
            return
        try:
            data = command.encode("ascii")
        except UnicodeEncodeError:
            self.message = "Commands use ASCII"
            return
        if not data or len(data) > 254 or any(c < 32 or c > 126 for c in data):
            self.message = "Use one command, at most 254 printable ASCII characters"
            return
        try:
            os.write(self.fd, data + b"\n")
        except BlockingIOError:
            self.message = "Command channel busy; try again"
            return
        except BrokenPipeError:
            self.finished = True
            self.paused = False
            self.message = "BASIC has closed the command channel"
            return
        self.events.append("> " + command)
        if command.strip() in ("step", "continue"):
            self.paused = False
            self.message = "Running; enter BASIC input in program terminal"
        if command.startswith("unwatch "):
            self.values.pop(command[8:].strip().upper(), None)

    def draw(self, screen, entry):
        screen.erase()
        height, width = screen.getmaxyx()

        def put(row, text, attr=0):
            if 0 <= row < height and width > 1:
                try:
                    screen.addnstr(row, 0, printable(text), width - 1, attr)
                except curses.error:
                    pass  # A resize can invalidate the dimensions mid-draw.

        if height < 18 or width < 50:
            put(0, "Resize to 50x18 or larger; q detaches")
            put(1, self.message)
        else:
            status = "VIEW ONLY" if self.finished else "CANCELLING" if self.cancelling else "PAUSED" if self.paused else "RUNNING / WAITING"
            put(0, "GW-BASIC debugger   " + status, curses.A_BOLD)
            location = self.location
            put(1, f"Stop {location[0]}  line {location[1]}  token offset {location[2]}  GOSUB depth {location[3]}" if location else "No stop yet")
            put(2, "Entry source (initial snapshot)")
            current = self.rows.get(location[1]) if location else None
            start = max(0, current - 2) if current is not None else 0
            for row, line in enumerate(self.source[start:start + 5], 3):
                put(row, ("> " if start + row - 3 == current else "  ") + line,
                    curses.A_REVERSE if start + row - 3 == current else 0)
            put(8, "Values at this stop (newest visible; max 16)", curses.A_BOLD)
            value_rows = min(len(self.values), max(1, (height - 15) // 3))
            for row, (name, value) in enumerate(list(self.values.items())[-value_rows:], 9):
                put(row, name + " " + value)
            event_start = 10 + value_rows
            put(event_start, "Events (Up/Down scroll)", curses.A_BOLD)
            available = max(0, height - event_start - 5)
            stop = len(self.events) - self.scroll
            visible = list(self.events)[max(0, stop - available):stop]
            for row, event in enumerate(visible, event_start + 1):
                put(row, event)
            put(height - 3, self.message)
            put(height - 2, "s step c cont b brk w/u watch i inspect p snap :")
        hint = "q close (read-only)" if self.finished else "q detach/resume | x cancel the BASIC run"
        if self.cancelling:
            hint = "Waiting for BASIC to exit before closing"
        put(height - 1, ":" + entry if entry is not None else hint)
        screen.refresh()

    def interact(self, screen):
        try:
            curses.curs_set(0)
        except curses.error:
            pass  # Some terminal descriptions cannot hide the cursor.
        screen.timeout(100)
        # A later controller may inspect the log but must not replay a stale
        # stop as live control after the first controller exits or crashes.
        (self.session / "controller-started").touch()
        self.started = True
        entry = None
        prefixes = {"b": "break ", "w": "watch ", "u": "unwatch ", "i": "inspect ", "p": "checkpoint ", ":": ""}
        while True:
            self.poll()
            if self.cancelling and self.finished:
                return
            self.draw(screen, entry)
            try:
                key = screen.get_wch()
            except curses.error:
                continue
            if entry is not None:
                if key == "\x1b":
                    entry = None
                elif key in ("\n", "\r", curses.KEY_ENTER):
                    self.send(entry)
                    entry = None
                    self.scroll = 0
                elif key in ("\b", "\x7f", curses.KEY_BACKSPACE):
                    entry = entry[:-1]
                elif isinstance(key, str) and 32 <= ord(key) < 127 and len(entry) < 254:
                    entry += key
            elif key == "q" and not self.cancelling:
                return
            elif key == "x" and not self.finished:
                (self.session / "cancel").touch()
                self.cancelling = True
                self.paused = False
                self.message = "Cancelling the BASIC run"
            elif key in ("s", "c"):
                self.send("step" if key == "s" else "continue")
                self.scroll = 0
            elif key in prefixes:
                entry = prefixes[key]
            elif key == curses.KEY_UP:
                self.scroll = max(0, min(len(self.events) - 1, self.scroll + 1))
            elif key == curses.KEY_DOWN:
                self.scroll = max(0, self.scroll - 1)


def attach(args):
    session = args.session.absolute()
    info = session.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("session must be an owned private directory (mode 0700)")
    if not stat.S_ISFIFO((session / "commands").stat().st_mode):
        raise ValueError("session command channel is not a FIFO")
    # Readiness is published after the source and FIFO have been created.
    json.loads((session / "session.json").read_text())
    with (session / "controller.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("this session already has a controller") from None
        if (session / "controller-started").exists():
            # Reopening is read-only. Mark the departed live controller even
            # if this viewer acquires the lock before the supervisor checks it.
            (session / "detached").touch()
        controller = Controller(session)
        def interrupt_controller(signum, frame):
            # The durable request is authoritative even if a signal arrives
            # between publishing it and updating the display flag.
            if not (session / "cancel").exists():
                interrupted(signum, frame)
        previous = {sig: signal.signal(sig, interrupt_controller)
                    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT)}
        try:
            curses.wrapper(controller.interact)
        finally:
            controller.close()
            for sig, handler in previous.items():
                signal.signal(sig, handler)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    launch = commands.add_parser("run", help="run BASIC here; attach the controller in another terminal")
    launch.add_argument("--binary", type=Path, default=Path("build/gwbasic"))
    launch.add_argument("--session", type=Path, required=True, help="new private session directory")
    launch.add_argument("source", type=Path, help="ASCII BASIC source")
    launch.set_defaults(action=run)
    client = commands.add_parser("attach", help="inspect an existing session in this terminal")
    client.add_argument("session", type=Path)
    client.set_defaults(action=attach)
    args = parser.parse_args()
    try:
        return args.action(args)
    except (OSError, ValueError, curses.error, termios.error) as exc:
        print(f"debugger: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
