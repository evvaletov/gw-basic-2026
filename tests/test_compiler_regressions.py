#!/usr/bin/env python3
"""Compare string/array semantics across codegen tiers and safety modes."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile


CASES = {
    "compact_functions": ('''10 REM comment: PRINT "BAD"
20 DEF    FNA(X)=X+256:PRINT "DEFINED"
30 PRINT FNA(41)
40 DEF FNB(Y)=Y+160
50 PRINT FNB(256)
60 DEF FNC(Z)=Z+1.5
70 PRINT FNC(1.5)
''', "", ["DEFINED", "297", "416", "3"]),
    "input_preserves_state": ('''10 X=19:S$="KEPT"
20 FOR J=1 TO 3
30 INPUT A
40 PRINT J;X;S$;A
50 NEXT J
60 PRINT J
''', "1\n2\n3\n", ["? 1 19 KEPT 1", "? 2 19 KEPT 2", "? 3 19 KEPT 3", "4"]),
    "backward_tab": ('''10 PRINT "ABCDE";TAB(3);"X"
20 PRINT "AB";TAB(3);"C"
30 LPRINT "ABCDE";TAB(3);"X"
40 LPRINT "AB";TAB(3);"C"
''', "", ["ABCDE", "X", "ABC"]),
    "grouped_strings": ('''10 A$="needle"
20 PRINT ((A$))
30 PRINT INSTR((A$),("e"))
40 PRINT INSTR((2),(A$),("e"))
50 PRINT ((A$)+("!"))
60 IF (A$)=("needle") THEN PRINT "MATCH"
70 PRINT (A$="needle")
80 PRINT (A$)="needle"
90 WRITE ((A$)),(LEFT$(A$,2))
100 PRINT LEN(((A$))),ASC(("A"))
110 IF ("a"+"b")=("ab") THEN PRINT "CONCAT"
''', "", ["needle", "2", "2", "needle!", "MATCH", "-1", "-1",
             '"needle","ne"', "6             65", "CONCAT"]),
    "array_delegation": ('''10 DIM A(2)
20 A(1)=7
30 PRINT USING "##";A(1)
40 INPUT "VALUE:",A(2)
50 PRINT A(2)
60 OPEN "array.dat" FOR OUTPUT AS #1
70 PRINT #1,A(1)
80 CLOSE #1
90 OPEN "array.dat" FOR INPUT AS #1
100 INPUT #1,A(2)
110 CLOSE #1
120 PRINT A(2)
''', "9\n", ["7", "VALUE: 9", "7"]),
    "base_and_layout": ('''10 OPTION BASE 1
20 DIM A%(2,3),B#(2)
30 A%(2,3)=19:B#(1)=3.5
40 PRINT A%(2,3)
50 PRINT A%(1,1)
60 PRINT USING "##";A%(2,3)
70 PRINT B#(1)
''', "", ["19", "0", "19", "3.5"]),
    "clear": ('''10 DIM A(2)
20 A(1)=7:X=4:S$="old"
30 CLEAR
40 PRINT A(1)
50 PRINT X
60 PRINT LEN(S$)
''', "", ["0", "0", "0"]),
    "erase_redim": ('''10 DIM A(2),B(2)
20 A(1)=8:B(1)=9
30 ERASE A
40 DIM A(4)
50 PRINT A(1)
60 PRINT B(1)
70 A(4)=10:PRINT A(4)
''', "", ["0", "9", "10"]),
    "conditional_dim": ('''10 IF 0 THEN DIM A(2)
20 A(10)=6
30 PRINT A(10)
''', "", ["6"]),
    "before_dim": ('''10 ON ERROR GOTO 100
20 A(1)=5
30 DIM A(2)
40 END
100 PRINT ERR
''', "", ["10"]),
    "repeated_dim": ('''10 ON ERROR GOTO 100
20 DIM A(2)
30 GOTO 20
100 PRINT ERR
''', "", ["10"]),
    "dynamic_dim": ('''10 N=3
20 DIM A(N)
30 A(3)=12
40 PRINT A(3)
''', "", ["12"]),
    "bounds": ('''10 ON ERROR GOTO 100
20 DIM A(2,2)
30 A(3,0)=9
40 END
100 PRINT ERR
''', "", ["9"]),
    "rank": ('''10 ON ERROR GOTO 100
20 DIM A(2,2)
30 PRINT A(1)
40 END
100 PRINT ERR
''', "", ["9"]),
    "subscript_rounding": ('''10 DIM A(3)
20 A(1.6)=8
30 PRINT A(2)
''', "", ["8"]),
    "option_after_dim": ('''10 ON ERROR GOTO 100
20 DIM A(2)
30 OPTION BASE 1
40 END
100 PRINT ERR
''', "", ["5"]),
    "string_array": ('''10 DIM A$(2)
20 A$(1)="needle"
30 PRINT ((A$(1)))
40 PRINT INSTR((A$(1)),("e"))
50 PRINT USING "!";A$(1)
''', "", ["needle", "2", "n"]),
    "numeric_tiers": ('''10 A=7:B=3
20 PRINT (A+B)*2
30 PRINT 7 MOD 3
40 PRINT (2^3)+1
50 PRINT (1/4)
60 IF A>B THEN PRINT "GT" ELSE PRINT "BAD"
''', "", ["20", "1", "9", "0.25", "GT"]),
    "delegated_else": ('''10 IF 0 THEN CLEAR ELSE PRINT "CLEAR ELSE"
20 IF 0 THEN OPTION BASE 1 ELSE PRINT "OPTION ELSE"
30 DIM A(2):A(1)=7
40 IF 0 THEN ERASE A ELSE PRINT A(1)
50 IF 1 THEN ERASE A ELSE PRINT "BAD ERASE"
60 PRINT A(1)
70 X=9:IF 1 THEN CLEAR ELSE PRINT "BAD CLEAR"
80 PRINT X
90 IF 1 THEN OPTION BASE 417-416 ELSE GOTO 9999
100 DIM B(2):B(1)=8:PRINT B(1)
110 IF 0 THEN PRINT TAB(3);"BAD TAB" ELSE PRINT "TAB ELSE"
120 IF 0 THEN PRINT USING "##";12 ELSE PRINT "USING ELSE"
130 IF 0 THEN OPTION BASE 0 ELSE Q=13:PRINT Q
140 IF 0 THEN ERASE B ELSE Y=14:PRINT Y
150 IF 1 THEN CLEAR ,1024,160:PRINT "CLEAR ARGS" ELSE PRINT "BAD ARGS"
160 IF 1 THEN CLEAR 1024,256:PRINT "ZERO BYTE" ELSE PRINT "BAD ZERO"
170 IF 1 THEN CLEAR 1024,416:PRINT "ELSE BYTE" ELSE PRINT "BAD ELSE"
''', "", ["CLEAR ELSE", "OPTION ELSE", "7", "0", "0", "8",
             "TAB ELSE", "USING ELSE", "13", "14", "CLEAR ARGS",
             "ZERO BYTE", "ELSE BYTE"]),
}


def normalized(text):
    return [" ".join(line.split()) for line in text.splitlines() if line.strip()]


def run(args, cwd, stdin="", timeout=15):
    result = subprocess.run(args, cwd=cwd, input=stdin, text=True,
                            capture_output=True, timeout=timeout)
    if result.returncode:
        raise AssertionError(f"{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    build = args.build.resolve()
    repo = Path(__file__).resolve().parents[1]
    pkg = subprocess.run(["pkg-config", "--libs", "libpulse-simple"], capture_output=True, text=True)
    libs = shlex.split(pkg.stdout) if pkg.returncode == 0 else []
    modes = [(f"-O{o}", inline, safe) for o in range(4)
             for inline in (False, True) for safe in (False, True)]
    count = 0
    with tempfile.TemporaryDirectory(prefix="gw-regress-") as temp:
        root = Path(temp)
        for name, (source, stdin, expected) in CASES.items():
            bas = root / f"{name}.bas"
            bas.write_text(source)
            want = normalized("\n".join(expected))
            actual = normalized(run([str(build / "gwbasic"), str(bas)], root, stdin))
            assert actual == want, (name, "interpreter", actual, want)
            if name == "backward_tab":
                assert (root / "LPT1.TXT").read_bytes() == b"ABCDE\n  X\nABC\n"
                (root / "LPT1.TXT").unlink()
            for opt, inline, safe in modes:
                c = root / f"{name}.c"
                exe = root / name
                flags = [opt] + (["--inline-arrays"] if inline else []) + (["--safe"] if safe else [])
                run([str(build / "gwbasic-compile"), str(bas), "-o", str(c),
                     "--max-output-size", "1", *flags], root)
                code = c.read_text()
                if name == "base_and_layout" and inline and not safe:
                    assert "_generation == gw_array_generation" in code, "optimization was not exercised"
                san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if args.sanitize else []
                run(["gcc", opt, *san, str(c), str(build / "libgwrt.a"),
                     "-I" + str(repo / "include"), "-lm", "-pthread", *libs, "-o", str(exe)], root)
                actual = normalized(run([str(exe)], root, stdin))
                assert actual == want, (name, flags, actual, want)
                if name == "backward_tab":
                    assert (root / "LPT1.TXT").read_bytes() == b"ABCDE\n  X\nABC\n"
                    (root / "LPT1.TXT").unlink()
                count += 1
            print(f"PASS {name} (16 compiler modes)", flush=True)

        # A malformed grouping must terminate with failure and remove partial C.
        bad = root / "bad.bas"
        bad.write_text('10 A$="x"\n20 PRINT (A$\n')
        c = root / "bad.c"
        result = subprocess.run([str(build / "gwbasic-compile"), str(bad), "-o", str(c),
                                 "--max-output-size", "1"], capture_output=True, text=True, timeout=5)
        assert result.returncode != 0 and not c.exists(), result
    print(f"Compiler regressions: {len(CASES)} interpreter cases, {count} compiled comparisons; malformed input rejected")


if __name__ == "__main__":
    main()
