#!/usr/bin/env python3
"""Compare control-flow, string and array semantics across compiler modes."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


CASES = {
    "native_resume_after_open_else": ('''10 ON ERROR GOTO 100
20 ERROR 5:PRINT "DONE":END
100 IF 0 THEN OPEN "unused" FOR INPUT AS #1 ELSE RESUME NEXT
''', "", ["DONE"]),
    "native_resume_last_line": ('''10 ON ERROR GOTO 100:GOTO 200
100 PRINT "TRAPPED":RESUME NEXT
200 ERROR 5
''', "", ["TRAPPED"]),
    "resume_text_only": ('''10 PRINT "RESUME":DATA RESUME
20 REM RESUME
30 END
''', "", ["RESUME"]),
    # Project port emulation, not an original-hardware reference transcript.
    "compiled_port_statements": ('''10 P=&H61:V=4
20 OUT P,V
30 PRINT INP(P)
40 WAIT P,4
50 WAIT P,1,1
60 MOTOR V
70 OUT &H61,0
80 PRINT INP(P)
90 END
''', "", ["4", "0"]),
    "if_nested_goto_boundary": ('''10 IF 0 THEN IF 0 THEN END ELSE GOTO 20 ELSE PRINT "OUTER"
20 END
''', "", ["OUTER"]),
    "if_nested_gosub_boundary": ('''10 IF 0 THEN IF 0 THEN END ELSE GOSUB 100 ELSE PRINT "OUTER"
20 END
100 PRINT "BAD":RETURN
''', "", ["OUTER"]),
    "if_goto_else": ('''10 FOR X=0 TO 1
20 IF X GOTO 50 ELSE PRINT "ELSE"
30 GOTO 60
50 PRINT "THEN"
60 NEXT X:END
''', "", ["ELSE", "THEN"]),
    "if_gosub_else": ('''10 FOR X=0 TO 1
20 IF X THEN GOSUB 100 ELSE PRINT "ELSE"
30 NEXT X:END
100 PRINT "THEN":RETURN
''', "", ["ELSE", "THEN"]),
    "if_numeric_false": ('''10 INPUT X
20 IF X THEN 50 ELSE 60
30 PRINT "FALLTHROUGH":END
50 PRINT "THEN":END
60 PRINT "ELSE":END
''', "0\n", ["? ELSE"]),
    "if_numeric_true": ('''10 INPUT X
20 IF X THEN 50 ELSE 60
30 PRINT "FALLTHROUGH":END
50 PRINT "THEN":END
60 PRINT "ELSE":END
''', "1\n", ["? THEN"]),
    "if_numeric_spaces": ('''10 X=0
20 IF X THEN 50    ELSE 60
30 PRINT "FALLTHROUGH":END
50 PRINT "THEN":END
60 PRINT "ELSE":END
''', "", ["ELSE"]),
    "if_numeric_else_assignment": ('''10 X=0
20 IF X THEN 50 ELSE A$="ANSWER":B=7
30 PRINT A$;B:END
50 PRINT "BAD":END
''', "", ["ANSWER 7"]),
    "if_numeric_else_error": ('''10 ON ERROR GOTO 100
20 X=0
30 IF X THEN 50 ELSE 999
40 PRINT "FALLTHROUGH":END
50 PRINT "BAD":END
100 PRINT ERR;ERL:END
''', "", ["8 30"]),
    "if_statement_else_error": ('''10 ON ERROR GOTO 100
20 X=0
30 IF X THEN PRINT "BAD" ELSE 999
40 PRINT "FALLTHROUGH":END
100 PRINT ERR;ERL:END
''', "", ["8 30"]),
    "if_numeric_else_untaken": ('''10 ON ERROR GOTO 100
20 X=1
30 IF X THEN 50 ELSE 999
40 PRINT "FALLTHROUGH":END
50 PRINT "THEN":END
100 PRINT "BAD ERROR";ERR;ERL:END
''', "", ["THEN"]),
    "if_statement_else_untaken": ('''10 ON ERROR GOTO 100
20 X=1
30 IF X THEN PRINT "THEN" ELSE 999
40 END
100 PRINT "BAD ERROR";ERR;ERL:END
''', "", ["THEN"]),
    "if_nested_numeric": ('''10 FOR X=0 TO 1:FOR Y=0 TO 1
20 IF X THEN IF Y THEN 50 ELSE 60 ELSE 70
30 PRINT "FALLTHROUGH":END
50 PRINT "TT":GOTO 80
60 PRINT "TF":GOTO 80
70 PRINT "F"
80 NEXT Y:NEXT X:END
''', "", ["F", "F", "TF", "TT"]),
    "if_nested_statements": ('''10 FOR X=0 TO 1:FOR Y=0 TO 1
20 IF X THEN IF Y THEN PRINT "TT" ELSE PRINT "TF" ELSE PRINT "F"
30 NEXT Y:NEXT X:END
''', "", ["F", "F", "TF", "TT"]),
    "if_else_nested": ('''10 FOR X=0 TO 1:FOR Y=0 TO 1
20 IF X THEN PRINT "T" ELSE IF Y THEN 50 ELSE 60
30 GOTO 70
50 PRINT "FT":GOTO 70
60 PRINT "FF"
70 NEXT Y:NEXT X:END
''', "", ["FF", "FT", "T", "T"]),
    "if_nested_error": ('''10 ON ERROR GOTO 100
20 X=1:Y=0
30 IF X THEN IF Y THEN PRINT "BAD" ELSE 999 ELSE PRINT "BAD OUTER"
40 END
100 PRINT ERR;ERL:END
''', "", ["8 30"]),
    "empty_string_io": ('''10 A$=""
20 OPEN "EMPTY.TXT" FOR OUTPUT AS #1
30 PRINT #1,A$
40 WRITE #1,A$
50 CLOSE #1
60 PRINT "DONE"
''', "", ["DONE"]),
    "locate_coordinates": ('''10 LOCATE 5,7
20 PRINT "MARK"
''', "", ["\x1b[5;7HMARK"]),
    "alternate_next": ('''10 FOR I=1 TO 3
20 IF I=1 THEN NEXT I
30 PRINT I
40 NEXT I
''', "", ["2", "3"]),
    "clear_gosub_state": ('''10 GOSUB 100
20 PRINT "BAD"
30 END
100 CLEAR
110 ON ERROR GOTO 200
120 RETURN
200 PRINT ERR
''', "", ["3"]),
    "clear_for_state": ('''10 FOR I=1 TO 2
20 CLEAR
30 ON ERROR GOTO 90
40 NEXT I
50 END
90 PRINT ERR
''', "", ["1"]),
    "empty_for_and_types": ('''10 FOR I=5 TO 1:PRINT I:NEXT I
20 FOR J#=1 TO 5 STEP -1:PRINT J#:NEXT J#
30 FOR K%=1 TO 3 STEP 2:PRINT K%:NEXT K%
40 PRINT I;J#;K%
''', "", ["1", "3", "5 1 5"]),
    "empty_skips_resume": ('''10 FOR I=2 TO 1
20 RESUME NEXT
25 OPEN "SKIPPED.TXT" FOR OUTPUT AS #1
30 PRINT "NEXT";129;130;256
40 NEXT I
50 PRINT "DONE"
''', "", ["DONE"]),
    "empty_skips_implicit_deftype": ('''10 FOR I=5 TO 1
20 DEFINT I
30 NEXT I
40 PRINT "DONE"
''', "", ["DONE"]),
    "empty_skips_deftype": ('''10 FOR I!=5 TO 1
20 DEFINT I
30 NEXT I
40 PRINT "DONE"
''', "", ["DONE"]),
    "empty_next_error_line": ('''10 ON ERROR GOTO 100
20 FOR I=2 TO 1
30 NEXT I,J
40 END
100 PRINT ERR;ERL
''', "", ["1 30"]),
    "empty_wrong_next": ('''5 ON ERROR GOTO 100
10 FOR I=5 TO 1
20 PRINT "BAD"
30 NEXT J
40 END
100 PRINT ERR;ERL
''', "", ["1 30"]),
    "empty_nested_next": ('''10 FOR I=1 TO 2
20 FOR J=2 TO 1:PRINT "BAD"
30 NEXT J,I
40 PRINT I;J
50 FOR A=5 TO 1
60 FOR B=1 TO 2:PRINT "NEXT";256
70 NEXT B
80 GOTO 100:NEXT A
90 PRINT "DONE"
100 END
''', "", ["3 2", "DONE"]),
    "mixed_for_while": ('''10 N=0
20 WHILE N<2
30 FOR W%=1 TO 2
40 N=N+1:PRINT N;W%
50 NEXT W%
60 WEND
''', "", ["1 1", "2 2"]),
    "forward_return_target": ('''10 GOTO 50
20 PRINT "SUB"
30 RETURN
50 GOSUB 20
60 PRINT "DONE"
''', "", ["SUB", "DONE"]),
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


# Original Microsoft GW-BASIC captures supersede the former permissive FOR
# expectations. Keep source and reference output together for reproduction.
REFERENCE = json.loads((Path(__file__).parent / "compatibility/semantics.json").read_text())
NATIVE_REFERENCES = {case["id"] for case in REFERENCE["cases"] if case.get("native")}
for case in REFERENCE["cases"]:
    CASES[case["id"]] = (case["source"], case["stdin"], case["output"].splitlines())


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
            if name == "empty_string_io":
                assert (root / "EMPTY.TXT").read_bytes() == b'\n""\n'
                (root / "EMPTY.TXT").unlink()
            for opt, inline, safe in modes:
                c = root / f"{name}.c"
                exe = root / name
                flags = [opt] + (["--inline-arrays"] if inline else []) + (["--safe"] if safe else [])
                run([str(build / "gwbasic-compile"), str(bas), "-o", str(c),
                     "--max-output-size", "1", *flags], root)
                code = c.read_text()
                if name == "deftype_prefix" or name.startswith("native_resume_") or name in NATIVE_REFERENCES:
                    assert "gw_run_loop();" not in code, "native compilation was not exercised"
                if name == "resume_text_only":
                    assert "_gw_stmt" not in code, "non-code RESUME enabled statement tracking"
                if name == "deftype_branch_else":
                    assert "gw_run_loop();" in code, "runtime type selection was not exercised"
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
                if name == "empty_string_io":
                    assert (root / "EMPTY.TXT").read_bytes() == b'\n""\n'
                    (root / "EMPTY.TXT").unlink()
                count += 1
            print(f"PASS {name} (16 compiler modes)", flush=True)

        # Embedded runtime programs must clean up between hosted entry calls.
        bas = root / "hosted.bas"
        bas.write_text(CASES["compat_deftype_string"][0])
        c, exe, host = root / "hosted.c", root / "hosted", root / "host.c"
        run([str(build / "gwbasic-compile"), str(bas), "-o", str(c),
             "--main-name", "typed_entry"], root)
        host.write_text('int typed_entry(int, char **);\n'
                        'int main(void) { if (typed_entry(0, 0)) return 1; return typed_entry(0, 0); }\n')
        run(["gcc", "-O2", *san, str(c), str(host), str(build / "libgwrt.a"),
             "-I" + str(repo / "include"), "-lm", "-pthread", *libs, "-o", str(exe)], root)
        assert normalized(run([str(exe)], root)) == normalized("\n".join(CASES["compat_deftype_string"][2])) * 2

        # Column and ERR are execution state too, including after an error exit.
        for source, expected in [
            ('10 PRINT TAB(5);"X";\n20 DEFINT A\n30 END\n', "    X    X"),
            ('10 PRINT ERR\n20 DEFINT A\n30 ERROR 5\n',
             " 0 \n\nIllegal function call in 30\n" * 2),
        ]:
            bas.write_text(source)
            host.write_text('int typed_entry(int, char **);\n'
                            'int main(void) { typed_entry(0, 0); typed_entry(0, 0); return 0; }\n')
            run([str(build / "gwbasic-compile"), str(bas), "-o", str(c),
                 "--main-name", "typed_entry"], root)
            run(["gcc", "-O2", *san, str(c), str(host), str(build / "libgwrt.a"),
                 "-I" + str(repo / "include"), "-lm", "-pthread", *libs, "-o", str(exe)], root)
            assert run([str(exe)], root) == expected

        # Entry declarations remain usable with native foreign functions.
        bas.write_text("10 '$EXTERN Plusone(INTEGER) AS INTEGER\n20 DEFINT A\n"
                       "30 A=2.5:PRINT Plusone(A)\n40 END\n")
        host.write_text('#include <stdint.h>\nint16_t Plusone(int16_t x) { return x+1; }\n')
        run([str(build / "gwbasic-compile"), str(bas), "-o", str(c)], root)
        run(["gcc", "-O2", *san, str(c), str(host), str(build / "libgwrt.a"),
             "-I" + str(repo / "include"), "-lm", "-pthread", *libs, "-o", str(exe)], root)
        assert normalized(run([str(exe)], root)) == ["4"]
        bas.write_text(bas.read_text().replace("20 DEFINT A", "20 IF 1 THEN DEFINT A"))
        result = subprocess.run([str(build / "gwbasic-compile"), str(bas), "-o", str(c)],
                                capture_output=True, text=True, timeout=5)
        assert result.returncode != 0 and not c.exists() and "$EXTERN" in result.stderr, result
        print("PASS runtime entry cleanup and DEFTYPE/FFI diagnostics", flush=True)

        # Preserve native-build coverage for the original listings that exposed
        # nonlexical FOR/NEXT paths. This does not claim complete gameplay.
        for name in ("awari", "checkers"):
            for opt in ("-O0", "-O1", "-O2", "-O3"):
                c, exe = root / (name + ".c"), root / name
                run([str(build / "gwbasic-compile"), str(repo / "tests/corpus" / (name + ".bas")),
                     "-o", str(c), "--max-output-size", "1", opt], root)
                run(["gcc", opt, *san, str(c), str(build / "libgwrt.a"),
                     "-I" + str(repo / "include"), "-lm", "-pthread", *libs, "-o", str(exe)], root)
            print(f"PASS {name}.bas (4 native builds; gameplay not exercised)", flush=True)

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
