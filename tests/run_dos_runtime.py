#!/usr/bin/env python3
"""Run both DOS builds in disposable FreeDOS/QEMU disks without root or KVM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = "FD14-FloppyEdition.zip"
URL = "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/" + ARCHIVE
# Upstream verify.txt; the extracted boot member is independently pinned too.
ARCHIVE_SHA = "45b1fa7c52dd996c3bfa5e352ffcd410781b952a6ad629f15a4c9ec4bbaefc5a"
BOOT_SHA = "552f7cbb0625960c050a3e682a4d2121cc2ffdfa9dc9d592ccd49edf179333dc"
BOOT_MEMBER = "144m/x86BOOT.img"


class Unavailable(RuntimeError):
    pass


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv):
    return subprocess.run([str(x) for x in argv], check=True, capture_output=True,
                          timeout=30).stdout


def check_qemu(qemu):
    devices = run([qemu, "-device", "help"])
    if b'name "isa-fdc"' not in devices or b'name "floppy"' not in devices:
        raise Unavailable("QEMU lacks PC floppy support; use a full qemu-system-x86 build")
    return run([qemu, "--version"]).decode("utf-8").splitlines()[0]


def boot_image(cache):
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ARCHIVE
    if not archive.exists():
        # Atomic publication prevents a partial download becoming a cache hit.
        with tempfile.TemporaryDirectory(dir=cache) as tmp:
            download = Path(tmp) / ARCHIVE
            deadline = time.monotonic() + 120
            with urllib.request.urlopen(URL, timeout=30) as response, download.open("wb") as stream:
                # read1 returns after one buffered/socket read, so a trickling
                # response cannot hide inside a full-megabyte read indefinitely.
                while block := response.read1(1024 * 1024):
                    if time.monotonic() > deadline or stream.tell() + len(block) > 32 * 1024 * 1024:
                        raise RuntimeError("FreeDOS download exceeded time/size limit")
                    stream.write(block)
            if digest(download) != ARCHIVE_SHA:
                raise RuntimeError("FreeDOS archive SHA256 mismatch")
            os.replace(download, archive)
    if digest(archive) != ARCHIVE_SHA:
        raise RuntimeError("FreeDOS archive SHA256 mismatch")
    with zipfile.ZipFile(archive) as bundle:
        boot = bundle.read(BOOT_MEMBER)
    if hashlib.sha256(boot).hexdigest() != BOOT_SHA:
        raise RuntimeError("FreeDOS boot image SHA256 mismatch")
    return boot


def put(image, work, name, data):
    path = work / name
    path.write_bytes(data)
    run(["mcopy", "-o", "-i", image, path, "::" + name])


def dos_text(text):
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii")


def prepare(work, boot, binary, extender, case):
    floppy = work / "boot.img"
    floppy.write_bytes(boot)
    put(floppy, work, "FDCONFIG.SYS", dos_text(
        "FILES=40\nBUFFERS=20\nLASTDRIVE=Z\n"
        "SHELL=A:\\FREEDOS\\BIN\\COMMAND.COM A:\\FREEDOS\\BIN /E:2048 /P=A:\\FDAUTO.BAT\n"))
    put(floppy, work, "FDAUTO.BAT", dos_text(
        "@ECHO OFF\nSET PATH=A:\\FREEDOS\\BIN;C:\\\nC:\nCLS\nECHO GW-BOOT-READY\n"))
    disk = work / "data.img"
    # Whole cylinders and matching CHS/LBA/BPB geometry also work with the
    # floppy edition's real-mode kernel; sentinel CHS values can stall InitDisk.
    sectors, start = 130 * 16 * 63, 63
    with disk.open("wb") as stream:
        stream.truncate(sectors * 512)
        mbr = bytearray(512)
        mbr[446:462] = struct.pack("<B3sB3sII", 0, bytes([1, 1, 0]), 0x06,
                                   bytes([15, 63, 129]), start, sectors - start)
        mbr[510:512] = b"\x55\xaa"
        stream.write(mbr)
    partition = str(disk) + "@@" + str(start * 512)
    run(["mformat", "-i", partition, "-h", 16, "-s", 63,
         "-H", start, "-T", sectors - start, "::"])
    for source, name in [(binary, "BASIC.EXE"), (extender, "DOS4GW.EXE")]:
        run(["mcopy", "-i", partition, source, "::" + name])
    smoke = (ROOT / "tests/dos_smoke.bas").read_text().replace("480 END", "480 SYSTEM")
    put(partition, work, "SMOKE.BAS", dos_text(smoke))
    put(partition, work, "HIGH.BAS", dos_text((ROOT / "tests/dos_runtime.bas").read_text()))
    put(partition, work, "INPUT.BAS", dos_text(case["source"]))
    put(partition, work, "RUNTEST.BAT", dos_text(
        "@ECHO OFF\nBASIC SMOKE.BAS\nBASIC HIGH.BAS\nECHO GW-BATCH-DONE\n"))
    return partition


class VM:
    def __init__(self, qemu, work, output, timeout):
        self.work, self.output = work, output
        self.deadline = time.monotonic() + timeout
        self.proc = self.sock = self.fp = None
        self.log = (output / "qemu.log").open("wb")
        sockpath = work / "qmp.sock"
        try:
            self.proc = subprocess.Popen([
                qemu, "-machine", "pc", "-accel", "tcg", "-cpu", "max", "-m", "32",
                "-nodefaults", "-device", "VGA",
                "-drive", f"file={work}/boot.img,format=raw,if=floppy,index=0",
                "-drive", f"file={work}/data.img,format=raw,if=ide,index=0",
                "-boot", "a", "-display", "none", "-serial", "null", "-monitor", "none",
                "-qmp", f"unix:{sockpath},server=on,wait=off", "-nic", "none", "-no-reboot"],
                stdout=self.log, stderr=subprocess.STDOUT)
            end = min(self.deadline, time.monotonic() + 10)
            while not sockpath.exists():
                if time.monotonic() > end or self.proc.poll() is not None:
                    raise RuntimeError("QEMU did not start; see qemu.log")
                time.sleep(0.05)
            self.sock = socket.socket(socket.AF_UNIX)
            self.sock.settimeout(5)
            while True:
                try:
                    self.sock.connect(str(sockpath))
                    break
                except ConnectionRefusedError:
                    if time.monotonic() > end or self.proc.poll() is not None:
                        raise
                    time.sleep(0.05)
            self.fp = self.sock.makefile("rwb", buffering=0)
            self.fp.readline()
            self.qmp("qmp_capabilities")
        except BaseException:
            self.close()
            raise

    def qmp(self, execute, arguments=None):
        if time.monotonic() > self.deadline:
            raise TimeoutError("DOS runtime deadline exceeded")
        self.fp.write((json.dumps({"execute": execute, "arguments": arguments or {}}) + "\n").encode())
        while True:
            if time.monotonic() > self.deadline:
                raise TimeoutError("DOS runtime deadline exceeded")
            line = self.fp.readline()
            if not line:
                raise RuntimeError("QEMU monitor closed")
            reply = json.loads(line)
            if "error" in reply:
                raise RuntimeError(str(reply["error"]))
            if "return" in reply:
                return reply["return"]

    def memory(self, address, size):
        path = self.work / "memory.bin"
        result = self.qmp("human-monitor-command", {
            "command-line": f'pmemsave {address} {size} "{path}"'})
        if result:
            raise RuntimeError(result)
        data = path.read_bytes()
        if len(data) != size:
            raise RuntimeError("Short QEMU memory capture")
        return data

    def screen(self):
        raw = self.memory(0xB8000, 4000)
        return [raw[i:i + 160:2].decode("cp437").rstrip() for i in range(0, 4000, 160)]

    def wait(self, predicate, name, timeout=20):
        end = min(self.deadline, time.monotonic() + timeout)
        while time.monotonic() < end:
            lines = self.screen()
            (self.output / "last-screen.txt").write_text("\n".join(lines) + "\n")
            if predicate(lines):
                (self.output / (name + ".txt")).write_text("\n".join(lines) + "\n")
                return lines
            time.sleep(0.1)
        raise TimeoutError(f"DOS screen timeout: {name}; see last-screen.txt")

    def key(self, key, drain=True):
        self.qmp("send-key", {"keys": [{"type": "qcode", "data": k} for k in key.split("-")],
                              "hold-time": 25})
        time.sleep(0.05)
        if drain:
            end = min(self.deadline, time.monotonic() + 5)
            while time.monotonic() < end:
                head, tail = struct.unpack("<HH", self.memory(0x41A, 4))
                if head == tail:
                    return
                time.sleep(0.03)
            raise TimeoutError("DOS keyboard did not consume input")

    def text(self, text):
        symbols = {" ": "spc", "\n": "ret", '"': "shift-apostrophe", ":": "shift-semicolon",
                   ";": "semicolon", ",": "comma", ".": "dot", "=": "equal", "-": "minus",
                   "(": "shift-9", ")": "shift-0"}
        for ch in text:
            self.key(symbols.get(ch, "shift-" + ch.lower() if ch.isupper() else ch))

    def command(self, text):
        self.text(text + "\n")

    def close(self):
        if self.proc:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=5)
        if self.fp:
            self.fp.close()
        if self.sock:
            self.sock.close()
        self.log.close()


def normalized(text):
    return [" ".join(line.split()) for line in text.replace("\r", "").splitlines() if line.strip()]


def check_output(actual, expected, name):
    if normalized(actual) != normalized(expected):
        raise AssertionError(f"{name} output mismatch: {actual!r}; expected {expected!r}")


def run_target(args, mode, boot, case, report):
    output = args.output / ("dos" + mode)
    output.mkdir(exist_ok=True)
    checks = []
    report["targets"][mode] = {"status": "failed", "checks": checks,
                               "binary_sha256": digest(getattr(args, "dos" + mode))}
    with tempfile.TemporaryDirectory(prefix="gw-dos-") as temp:
        work = Path(temp)
        part = prepare(work, boot, getattr(args, "dos" + mode), args.dos4gw, case)
        vm = VM(args.qemu, work, output, args.timeout)
        captures = {}
        try:
            def check(name, predicate, timeout=20):
                vm.wait(predicate, name, timeout)
                checks.append(name)
                print(f"PASS DOS{mode} {name}", flush=True)

            check("boot", lambda ls: "GW-BOOT-READY" in ls and "C:\\>" in ls, 45)
            vm.command("RUNTEST")
            check("batch-return", lambda ls: "GW-BATCH-DONE" in ls and "C:\\>" in ls, 45)
            vm.command("BASIC")
            check("editor", lambda ls: "Ok" in ls and "1LIST" in ls[-1])
            vm.command('40000 PRINT "HIGH EDIT"')
            vm.command('65529 PRINT "REMOVE ME"')
            vm.command("65529")
            vm.command("RUN")
            check("high-edit-delete", lambda ls: "HIGH EDIT" in ls and "REMOVE ME" not in ls)
            vm.command('LOAD "INPUT.BAS"')
            vm.command("RUN")
            check("input-prompt", lambda ls: any("PRE" in line for line in ls))
            vm.text(case["answer"])
            for key in case["keys"]:
                vm.key(key)
            vm.key("ret")
            check("input-cursor", lambda ls: "DONE11" in ls and all(
                ls[int(row)] == expected for row, expected in case["rows"].items()))
            vm.command("NEW")
            vm.command("CLS")
            vm.command("10 GOTO 10")
            vm.command("RUN")
            time.sleep(0.2)
            vm.key("ctrl-c", drain=False)
            check("compute-break", lambda ls: any("Break" in line for line in ls))
            vm.command('PRINT "READY AGAIN"')
            check("break-recovery", lambda ls: "READY AGAIN" in ls)
            vm.command("SYSTEM")
            check("system-return", lambda ls: "C:\\>" in ls)
        finally:
            vm.close()
            # Preserve partial output on guest failures before deleting its disk.
            capture_errors = {}
            for filename in ("OUT.TXT", "HIGH.TXT", "O11.TXT"):
                try:
                    data = run(["mtype", "-i", part, "::" + filename]).decode("ascii").rstrip("\x1a")
                    captures[filename] = data
                    (output / filename).write_text(data)
                except (subprocess.SubprocessError, OSError, UnicodeError) as error:
                    capture_errors[filename] = str(error)
            report["targets"][mode]["captured_files"] = list(captures)
            report["targets"][mode]["capture_errors"] = capture_errors
        for filename, expected in [
            ("OUT.TXT", (ROOT / "tests/expected/dos_smoke.expected").read_text()),
            ("HIGH.TXT", (ROOT / "tests/expected/dos_runtime.expected").read_text()),
        ]:
            if filename not in captures:
                raise AssertionError("Missing guest output: " + filename)
            data = captures[filename]
            check_output(data, expected, filename)
            checks.append("saved-" + filename)
        if "O11.TXT" not in captures:
            raise AssertionError("Missing saved input: O11.TXT")
        saved = captures["O11.TXT"]
        lines = saved.splitlines()
        if [int(float(x)) for x in lines[0].split()] != case["positions"] or lines[1] != case["value"]:
            raise AssertionError("Saved input/cursor differs from original reference")
        checks.append("saved-input")
    report["targets"][mode]["status"] = "passed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dos16", type=Path, default=ROOT / "gwbasic16.exe")
    parser.add_argument("--dos32", type=Path, default=ROOT / "gwbasic.exe")
    parser.add_argument("--dos4gw", type=Path, default=Path(os.environ.get("WATCOM", str(Path.home() / "openwatcom-v2"))) / "binw/dos4gw.exe")
    parser.add_argument("--qemu", default=shutil.which("qemu-system-i386") or shutil.which("qemu-system-x86_64") or "/usr/libexec/qemu-kvm")
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/gwbasic-freedos")
    parser.add_argument("--output", type=Path, default=ROOT / "build/dos-runtime")
    parser.add_argument("--timeout", type=float, default=120, help="maximum seconds per target")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"status": "failed", "targets": {}, "freedos_url": URL,
              "archive_sha256": ARCHIVE_SHA, "boot_sha256": BOOT_SHA}
    started = time.monotonic()
    status = 1
    try:
        for tool in (args.qemu, "mcopy", "mformat", "mtype"):
            if not shutil.which(tool):
                raise Unavailable("Missing required tool: " + tool)
        for path in (args.dos16, args.dos32, args.dos4gw):
            if not path.is_file():
                raise Unavailable("Missing required binary: " + str(path))
        report["qemu_version"] = check_qemu(args.qemu)
        report["dos4gw_sha256"] = digest(args.dos4gw)
        boot = boot_image(args.cache)
        cases = json.loads((ROOT / "tests/compatibility/input-console.json").read_text())["cases"]
        case = next(case for case in cases if case["id"] == "input_edited")
        report["input_reference"] = case["id"]
        for mode in ("16", "32"):
            run_target(args, mode, boot, case, report)
        report["status"] = "passed"
        status = 0
    except Exception as error:
        report["status"] = "unavailable" if isinstance(error, Unavailable) else "failed"
        report["error"] = str(error)
        status = 2 if isinstance(error, Unavailable) else 1
        print(f'{report["status"].upper()}: {error}', flush=True)
    finally:
        report["seconds"] = round(time.monotonic() - started, 2)
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
