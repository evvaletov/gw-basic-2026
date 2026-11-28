#!/usr/bin/env python3
"""Exercise Rugxulo's string converters in both DOS builds, including the editor."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import run_dos_runtime as dos

FIXTURES = dos.ROOT / "tests/fixtures/rugxulo"
RECORDS = 32
INPUT = dos.dos_text("; generated test input\n" + "\tVALUE     DW     0ABCDh\n" * RECORDS)
GROWTH = dos.dos_text('''10 A$=STRING$(200,65):A$=""
20 X=FRE("")
30 DIM H%(1023):H%(1023)=1234
40 OPEN "GROW.OUT" FOR OUTPUT AS #1:PRINT #1,H%(1023):CLOSE #1
50 SYSTEM
''')
EXPECTED = {
    "TIDY.B": {"TIDY.ASM": dos.dos_text("VALUE DW 0ABCDh\n" * RECORDS)},
    "HEXNUM.B": {"HEXNUM.OUT": dos.dos_text("\tVALUE     DW     0x0ABCD\n" * RECORDS)},
    "INVA86W.NEW": {
        "INV.A86": dos.dos_text("VALUE     DW     0ABCDh\n" * RECORDS),
        "INV.INC": dos.dos_text("EXTRN VALUE:W\n" * RECORDS),
    },
    "GROW.BAS": {"GROW.OUT": b" 1234 \r\n"},
}
CASES = [
    ("TIDY.B", "editor"),
    ("HEXNUM.B", "editor"),
    ("INVA86W.NEW", "merge"),
    ("TIDY.B", "clear-before-load"),
    ("TIDY.B", "clear-in-program"),
    ("INVA86W.NEW", "startup"),
    ("GROW.BAS", "editor"),
]


def wait_result(vm, prompt, name):
    def finished(lines):
        if prompt in lines or any("NULL assignment" in line for line in lines):
            return True
        # Wait for the error's Ok prompt, not a partially printed error line.
        return any(line.startswith("Out of ") and "Ok" in lines[index + 1:]
                   for index, line in enumerate(lines))
    return vm.wait(finished, name, 45)


def run_target(args, bits, boot):
    output = args.output / ("dos" + bits)
    output.mkdir(parents=True, exist_ok=True)
    binary = getattr(args, "dos" + bits)
    report = {"binary_sha256": dos.digest(binary), "status": "failed", "cases": []}
    try:
        with tempfile.TemporaryDirectory(prefix="gw-strings-") as tmp:
            work = Path(tmp)
            part = dos.prepare(work, boot, binary, args.dos4gw, {"source": "10 SYSTEM\n"})
            for index, (source, mode) in enumerate(CASES):
                directory = f"S{index}"
                dos.run(["mmd", "-i", part, "::" + directory])
                data = GROWTH if source == "GROW.BAS" else (FIXTURES / source).read_bytes()
                if mode == "clear-in-program":
                    data = b"0 CLEAR ,12288\r\n" + data
                files = {source: data, "INVADERS.ASM": INPUT}
                if mode == "merge":
                    # Reconstruct a LOAD/MERGE pair from the supplied merged source.
                    lines = data.splitlines(keepends=True)
                    files[source] = b"".join(line for line in lines if int(line.split()[0]) < 300)
                    files["MERGE.GW"] = b"".join(line for line in lines if int(line.split()[0]) >= 300)
                for name, contents in files.items():
                    path = work / name
                    path.write_bytes(contents)
                    dos.run(["mcopy", "-i", part, path, f"::{directory}/{name}"])
            vm = dos.VM(args.qemu, work, output, args.timeout)
            try:
                vm.wait(lambda lines: "GW-BOOT-READY" in lines and "C:\\>" in lines, "boot", 45)
                for index, (source, mode) in enumerate(CASES):
                    prompt = f"C:\\S{index}>"
                    vm.command(f"CD S{index}")
                    # Discard the previous editor's Ok/key bar before waiting
                    # for this launch; DOS/4GW startup can outlast key delivery.
                    vm.command("CLS")
                    vm.wait(lambda lines: prompt in lines and "Ok" not in lines,
                            f"directory-{index}")
                    if mode == "startup":
                        vm.command("BASIC " + source)
                    else:
                        vm.command("BASIC")
                        vm.wait(lambda lines: "Ok" in lines and "1LIST" in lines[-1], f"editor-{index}")
                        if mode == "clear-before-load":
                            vm.command("CLEAR ,12288")
                        vm.command('LOAD "' + source + '"')
                        if mode == "merge":
                            vm.command('MERGE "MERGE.GW"')
                        vm.command("RUN")
                    screen = wait_result(vm, prompt, f"result-{index}")
                    resized = True
                    if bits == "16" and mode == "clear-in-program" and "Out of memory in 0" in screen:
                        # A larger near pool may not fit after LOAD fragments the
                        # heap. CLEAR must report this at line 0 and retain the
                        # old pool so the unmodified converter still runs.
                        resized = False
                        vm.command("0")
                        vm.command("CLS")
                        vm.wait(lambda lines: not any("Out of memory" in line for line in lines),
                                f"cleared-{index}")
                        vm.command("RUN")
                        screen = wait_result(vm, prompt, f"recovered-{index}")
                    if prompt not in screen or any("NULL assignment" in line for line in screen):
                        raise AssertionError(f"DOS{bits} {source} ({mode}): guest error; see result-{index}.txt")
                    result = {"source": source, "mode": mode, "status": "ran"}
                    if mode == "clear-in-program":
                        result["resize"] = "accepted" if resized else "out of memory; recovered with original pool"
                    report["cases"].append(result)
                    vm.command("CD ..")
            finally:
                vm.close()
                # Copy files only after the guest stops; retain partial failures too.
                for index, (source, mode) in enumerate(CASES):
                    folder = output / f"S{index}"
                    folder.mkdir(exist_ok=True)
                    for name in EXPECTED[source]:
                        (folder / name).unlink(missing_ok=True)
                        result = subprocess.run(["mtype", "-i", part, f"::S{index}/{name}"],
                                                capture_output=True, timeout=30)
                        if result.returncode == 0:
                            (folder / name).write_bytes(result.stdout)
            for index, (source, mode) in enumerate(CASES):
                for name, expected in EXPECTED[source].items():
                    actual = output / f"S{index}" / name
                    if not actual.is_file() or actual.read_bytes() != expected:
                        raise AssertionError(f"DOS{bits} {source} ({mode}): {name} differs from expected bytes")
                report["cases"][index]["status"] = "passed"
                print(f"PASS DOS{bits} {source} ({mode})", flush=True)
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dos16", type=Path, default=dos.ROOT / "gwbasic16.exe")
    parser.add_argument("--dos32", type=Path, default=dos.ROOT / "gwbasic.exe")
    parser.add_argument("--dos4gw", type=Path, default=Path.home() / "openwatcom-v2/binw/dos4gw.exe")
    parser.add_argument("--qemu", default=shutil.which("qemu-system-i386") or "qemu-system-x86_64")
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/gwbasic-freedos")
    parser.add_argument("--output", type=Path, default=dos.ROOT / "build/dos-strings")
    parser.add_argument("--timeout", type=float, default=240)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.output = args.output.resolve()
    dos.check_qemu(args.qemu)
    boot = dos.boot_image(args.cache)
    for bits in ("16", "32"):
        run_target(args, bits, boot)


if __name__ == "__main__":
    main()
