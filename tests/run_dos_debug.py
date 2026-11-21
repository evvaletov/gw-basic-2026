#!/usr/bin/env python3
"""Exercise scripted debugging and guest screen preservation on DOS16/32."""
import json
import argparse
from pathlib import Path
import shutil
import tempfile

import run_dos_runtime as dos

SOURCE = '''5 DIM M%(2,3):M%(1,2)=17:M%(2,1)=29
10 CLS:PRINT "GUEST SCREEN"
20 A%=1:GOSUB 100
30 SCREEN 1
35 PSET (319,199),1:IF POINT(319,199)<>1 THEN ERROR 5
40 PSET (4,5),2
45 COLOR 17,0:COLOR ,1:SCREEN 1
46 IF INP(&H3D9)<>49 OR POINT(4,5)<>2 THEN ERROR 5
47 OUT &H3D9,63:PALETTE
48 IF INP(&H3D9)<>48 THEN ERROR 5
50 C%=POINT(4,5)
55 SCREEN 2:PSET (383,97),0:PSET (639,199),1
56 IF POINT(383,97)<>0 OR POINT(639,199)<>1 THEN ERROR 5
60 SCREEN 0:CLS:PRINT "GUEST SCREEN";A%;C%
70 OPEN "R",#1,"FIELD.BIN",8
75 FIELD #1,4 AS A$,4 AS B$
80 LSET A$="TEST":LSET B$="1234":PUT #1,1
85 CLOSE #1
90 OPEN "CHECK.TXT" FOR OUTPUT AS #2:PRINT #2,A%;C%:CLOSE #2:SYSTEM
100 A%=A%+1:RETURN
'''
COMMANDS = "watch A%\nwatch C%\nwatch A$\nwatch M%(1,2)\nwatch M%(2,1)\n" + "checkpoint guest\ninspect file 1 0 8\nstep\n"*64


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default=shutil.which("qemu-system-i386") or shutil.which("qemu-system-x86_64") or "/usr/libexec/qemu-kvm")
    parser.add_argument("--system-disk", type=Path, help="existing FreeDOS qcow2; always boot a disposable overlay")
    parser.add_argument("--dos16", type=Path, default=dos.ROOT/"gwbasic16.exe")
    parser.add_argument("--dos32", type=Path, default=dos.ROOT/"gwbasic.exe")
    parser.add_argument("--dos4gw", type=Path, default=Path.home()/"openwatcom-v2/binw/dos4gw.exe")
    args = parser.parse_args()
    qemu = args.qemu
    base_digest = dos.digest(args.system_disk) if args.system_disk else None
    if not args.system_disk: dos.check_qemu(qemu)
    boot = dos.boot_image(Path.home()/".cache/gwbasic-freedos")
    extender = args.dos4gw
    case = next(c for c in json.loads((dos.ROOT/"tests/compatibility/input-console.json").read_text())["cases"] if c["id"] == "input_edited")
    report = {}
    for mode, binary in [("16", args.dos16.resolve()), ("32", args.dos32.resolve())]:
        output = dos.ROOT/"build/dos-debug"/mode
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="gw-dos-debug-") as tmp:
            work = Path(tmp)
            part = dos.prepare(work, boot, binary, extender, case)
            dos.put(part, work, "DEBUG.BAS", dos.dos_text(SOURCE))
            dos.put(part, work, "CMD.TXT", dos.dos_text(COMMANDS))
            dos.put(part, work, "DBGTEST.BAT", dos.dos_text(
                "@ECHO OFF\nBASIC DEBUG.BAS\nIF ERRORLEVEL 1 GOTO FAILED\n"
                "REN CHECK.TXT PLAIN.TXT\nREN FIELD.BIN PLAIN.BIN\n"
                "IF EXIST CHECK.TXT GOTO FAILED\nIF EXIST FIELD.BIN GOTO FAILED\n"
                "BASIC --debug-script CMD.TXT --debug-output LOG.TXT DEBUG.BAS\n"
                "IF ERRORLEVEL 1 GOTO FAILED\nECHO DEBUG-DONE\nGOTO FINISHED\n"
                ":FAILED\nECHO DEBUG-FAILED\n:FINISHED\n"))
            overlay = None
            if args.system_disk:
                overlay = work/"system.qcow2"
                dos.run(["qemu-img", "create", "-f", "qcow2", "-F", "qcow2", "-b", args.system_disk.resolve(), overlay])
            vm = dos.VM(qemu, work, output, 120, system_disk=overlay)
            try:
                vm.wait(lambda ls: any(row.rstrip().endswith("C:\\>") for row in ls), "boot", 60)
                prompt = "C:\\>"
                if overlay:
                    vm.command("D:")
                    prompt = "D:\\>"
                    vm.wait(lambda ls: prompt in ls, "data-drive")
                vm.command("DBGTEST")
                lines = vm.wait(lambda ls: any(s in ls for s in ("DEBUG-DONE", "DEBUG-FAILED")) and prompt in ls, "done", 45)
                assert "DEBUG-DONE" in lines and "DEBUG-FAILED" not in lines, lines
                assert any("GUEST SCREEN" in line and "2" in line for line in lines), lines
                assert not any("checkpoint" in line or "value A%" in line for line in lines), lines
            finally:
                vm.close()
            captures = {}
            for name in ("CHECK.TXT", "PLAIN.TXT", "FIELD.BIN", "PLAIN.BIN", "LOG.TXT"):
                captures[name] = dos.run(["mtype", "-i", part, "::"+name])
                (output/name).write_bytes(captures[name])
            assert captures["CHECK.TXT"] == captures["PLAIN.TXT"]
            assert captures["FIELD.BIN"] == captures["PLAIN.BIN"] == b"TEST1234"
            assert [int(x) for x in captures["CHECK.TXT"].split()] == [2, 2]
            log = captures["LOG.TXT"].decode("ascii")
            assert "line=100 offset=0 depth=1" in log
            assert "value C% integer=2" in log
            assert "value M%(1,2) bounds=0:2,0:3 integer=17" in log
            assert "value M%(2,1) bounds=0:2,0:3 integer=29" in log
            assert "field A file=1 offset=0 length=4" in log
            assert "buffer offset=0 hex=5445535431323334" in log
            assert "error " not in log
            assert "checkpoint guest" in log
            report[mode] = {"status": "passed", "binary_sha256": dos.digest(binary),
                            "checks": ["calls", "watches", "checkpoints", "FIELD bytes", "graphics POINT",
                                       "CGA colour state", "same-mode pixels", "full-size framebuffers",
                                       "guest screen", "normal/debug equivalence"]}
            print("PASS DOS"+mode+" debug and screen preservation", flush=True)
    if args.system_disk:
        assert dos.digest(args.system_disk) == base_digest, "base disk changed"
        report["base_unchanged_sha256"] = base_digest
    (dos.ROOT/"build/dos-debug/report.json").write_text(json.dumps(report, indent=2)+"\n")


if __name__ == "__main__":
    main()
