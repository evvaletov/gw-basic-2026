#!/usr/bin/env python3
"""Check CGA colours and immediate redraws in interpreted and native output."""
from pathlib import Path
import json
import re
import shlex
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
BUILD = REPO / "build"
SIXEL = re.compile(r"\x1bPq(.*?)\x1b\\", re.S)
COLOUR = re.compile(r"#(\d+);2;(\d+);(\d+);(\d+)")

# Microsoft GW-BASIC Interpreter User's Reference (1986), SCREEN tables 3 and 4,
# printed pages 225-226 (PDF pages 227-228):
# https://ftpmirror.your.org/pub/misc/bitsavers/pdf/microsoft/gw-basic/410130013-320-R01-0686_Microsoft_GW-BASIC_Interpreter_Users_Reference_1986.pdf#page=227
# Sixel uses integer RGB percentages; light cyan/magenta have a 33% component.
DEFAULTS = {
    1: {0: (0, 0, 0), 1: (33, 100, 100), 2: (100, 33, 100), 3: (100, 100, 100)},
    2: {0: (0, 0, 0), 1: (100, 100, 100)},
}
OVERRIDES = {
    1: {0: (66, 0, 0), 1: (0, 66, 0), 2: (66, 0, 0), 3: (66, 33, 0)},
    2: {0: (66, 0, 0), 1: (0, 66, 0)},
}
RGBI = [(0, 0, 0), (0, 0, 66), (0, 66, 0), (0, 66, 66),
        (66, 0, 0), (66, 0, 66), (66, 33, 0), (66, 66, 66),
        (33, 33, 33), (33, 33, 100), (33, 100, 33), (33, 100, 100),
        (100, 33, 33), (100, 33, 100), (100, 100, 33), (100, 100, 100)]


def palette(*indices):
    return {attr: RGBI[colour] for attr, colour in enumerate(indices)}


def pixels(mode):
    return [f"PSET ({colour},0),{colour}" for colour in DEFAULTS[mode]]


def points(mode):
    return "PRINT " + ";".join(f"POINT({colour},0)" for colour in DEFAULTS[mode])


class GraphicsPalette(unittest.TestCase):
    def run_hosted(self, source, host, flags, sanitize=False):
        with tempfile.TemporaryDirectory(prefix="gw-palette-host-") as directory:
            root = Path(directory)
            bas, generated, provider, exe = [root / name for name in
                                             ("case.bas", "case.c", "host.c", "case")]
            bas.write_text(source)
            provider.write_text(host)
            subprocess.run([str(BUILD / "gwbasic-compile"), str(bas), "-o", str(generated),
                            "--main-name", "palette_entry", *flags], check=True,
                           capture_output=True, timeout=30)
            self.assertNotIn("gw_run_loop();", generated.read_text())
            pkg = subprocess.run(["pkg-config", "--libs", "libpulse-simple"],
                                 capture_output=True, text=True)
            libs = shlex.split(pkg.stdout) if pkg.returncode == 0 else []
            checks = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if sanitize else []
            subprocess.run(["gcc", flags[0], *checks, str(generated), str(provider), str(BUILD / "libgwrt.a"),
                            "-I" + str(REPO / "include"), "-lm", "-pthread", *libs, "-o", str(exe)],
                           check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(exe)], cwd=root, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            return " ".join(SIXEL.sub("", result.stdout).split())

    def check_program(self, statements, expected_palettes, expected_text,
                      redraws=None, same_pixels=False, trap_errors=False,
                      error_handler="RESUME NEXT"):
        if redraws is None:
            redraws = [True] * len(expected_palettes)
        with tempfile.TemporaryDirectory(prefix="gw-palette-") as directory:
            root = Path(directory)
            source = root / "palette.bas"
            lines = (["ON ERROR GOTO 60000"] if trap_errors else []) + [*statements, "END"]
            source.write_text("".join(f"{10 * (i + 1)} {s}\n" for i, s in enumerate(lines))
                              + ("60000 " + error_handler + "\n" if trap_errors else ""))

            def run(command):
                result = subprocess.run(command, cwd=root, text=True,
                                        capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout

            run([str(BUILD / "gwbasic-compile"), str(source), "-c", "--keep-c",
                 "--runtime", str(REPO)])
            self.assertNotIn("gw_run_loop();", Path(str(source) + ".c").read_text())
            for backend, command in [
                ("interpreter", [str(BUILD / "gwbasic"), str(source)]),
                ("native", [str(source.with_suffix(""))]),
            ]:
                with self.subTest(backend=backend):
                    output = run(command)
                    # A marker immediately after COLOR/OUT/PALETTE must see the
                    # changed frame without a later drawing command hiding a lag.
                    sections = output.split("SNAP\n")
                    self.assertEqual(len(sections), len(expected_palettes) + 1)
                    last_frame = None
                    payload = None
                    for section, expected, redraw in zip(sections, expected_palettes, redraws):
                        frames = SIXEL.findall(section)
                        self.assertEqual(bool(frames), redraw, "unexpected/missing immediate redraw")
                        if frames:
                            last_frame = frames[-1]
                        self.assertIsNotNone(last_frame)
                        actual = {int(a): (int(r), int(g), int(b))
                                  for a, r, g, b in COLOUR.findall(last_frame)}
                        self.assertEqual(actual, expected)
                        current = COLOUR.sub("", last_frame)
                        if same_pixels and payload is not None:
                            self.assertEqual(current, payload, "palette change altered pixel attributes")
                        payload = current
                    text = SIXEL.sub("", output).replace("SNAP\n", "")
                    self.assertEqual(" ".join(text.split()), expected_text)

    def test_pinned_cga_reference_frames(self):
        reference = json.loads((REPO / "tests/compatibility/cga-color.json").read_text())
        statements, expected, redraws = [], [], []
        for case in reference["cases"]:
            # Keep the reference's palette/mode sequence. Four single pixels
            # replace its four stripes; the test compares the attribute map.
            command = case["command"].replace("GOSUB 64000", ":".join(pixels(1)))
            statements.extend([command, 'PRINT "SNAP"'])
            expected.append({i: tuple(c * 100 // 255 for c in rgb)
                             for i, rgb in enumerate(case["rgb"])})
            redraws.append(command != "SCREEN 1")
        self.check_program(statements, expected, "", redraws=redraws, same_pixels=True)

    def test_screen_1_defaults(self):
        self.check_program(
            ["SCREEN 1", *pixels(1), "PSET (4,0)", 'PRINT "SNAP"',
             points(1), "PRINT POINT(4,0)"],
            [DEFAULTS[1]], "0 1 2 3 3")

    def test_screen_2_defaults(self):
        self.check_program(
            ["SCREEN 2", "PSET (1,0)", 'PRINT "SNAP"', points(2)],
            [DEFAULTS[2]], "0 1")

    def test_explicit_palette_and_reset(self):
        for mode in (1, 2):
            with self.subTest(mode=mode):
                remap = [f"PALETTE {a},{2 * a if a else 4}" for a in DEFAULTS[mode]]
                values = " ".join(str(a) for a in DEFAULTS[mode])
                self.check_program(
                    [f"SCREEN {mode}", *remap, *pixels(mode), 'PRINT "SNAP"',
                     points(mode), "PALETTE", "PSET (0,1),0", 'PRINT "SNAP"',
                     points(mode)],
                    [OVERRIDES[mode], DEFAULTS[mode]], f"{values} {values}")

    def test_screen_changes_discard_palette_overrides(self):
        for start, modes in [(1, [2, 1]), (2, [1, 2]),
                             (1, [0, 1]), (2, [0, 2])]:
            with self.subTest(start=start, modes=modes):
                statements = [f"SCREEN {start}", "PALETTE 0,4", "PALETTE 1,1"]
                expected = []
                for mode in modes:
                    statements.append(f"SCREEN {mode}")
                    if mode:
                        statements.extend([*pixels(mode), 'PRINT "SNAP"'])
                        expected.append(DEFAULTS[mode])
                self.check_program(statements, expected, "")

    def test_same_screen_preserves_pixels_and_palette(self):
        for mode in (1, 2):
            expected = dict(DEFAULTS[mode])
            expected[0] = RGBI[4]
            self.check_program(
                [f"SCREEN {mode}", *pixels(mode), "PALETTE 0,4", 'PRINT "SNAP"',
                 f"SCREEN {mode}", 'PRINT "SNAP"', points(mode)],
                [expected, expected], " ".join(map(str, DEFAULTS[mode])),
                redraws=[True, False], same_pixels=True)

    def test_color_background_palette_and_intensity(self):
        self.check_program(
            ["SCREEN 1", *pixels(1), 'PRINT "SNAP"',
             "COLOR 17,0", 'PRINT "SNAP"', "COLOR ,1", 'PRINT "SNAP"',
             "COLOR 1", 'PRINT "SNAP"', points(1), "PSET (4,0)", "PRINT POINT(4,0)"],
            [DEFAULTS[1], palette(1, 10, 12, 14), palette(1, 11, 13, 15),
             palette(1, 3, 5, 7)], "0 1 2 3 3", same_pixels=True)

    def test_color_omissions_byte_arguments_and_expressions(self):
        self.check_program(
            ["SCREEN 1", *pixels(1), 'PRINT "SNAP"',
             "COLOR ,", 'PRINT "SNAP"', "COLOR ,,", 'PRINT "SNAP"',
             "COLOR 255,2", 'PRINT "SNAP"', "COLOR ,255,", 'PRINT "SNAP"',
             "B%=17:P%=2:COLOR B%,P%", 'PRINT "SNAP"',
             "DIM C%(2):C%(1)=1:COLOR C%(1),P%+1", 'PRINT "SNAP"',
             "IF 1 THEN COLOR 16,0 ELSE COLOR 0,1", 'PRINT "SNAP"', points(1)],
            [DEFAULTS[1], DEFAULTS[1], DEFAULTS[1], palette(15, 10, 12, 14),
             palette(15, 11, 13, 15), palette(1, 10, 12, 14), palette(1, 3, 5, 7),
             palette(0, 10, 12, 14)], "0 1 2 3",
            redraws=[True, False, False, True, True, True, True, True], same_pixels=True)

    def test_out_palette_bits_and_bios_shadow(self):
        self.check_program(
            ["SCREEN 1", *pixels(1), 'PRINT "SNAP"',
             "OUT &H3D9,0", 'PRINT "SNAP"', "OUT &H3D9,16", 'PRINT "SNAP"',
             "OUT &H3D9,32", 'PRINT "SNAP"', "OUT &H3D9,48", 'PRINT "SNAP"',
             "OUT &H3D9,63", 'PRINT "SNAP"', "PRINT INP(&H3D9)",
             "COLOR ,0", 'PRINT "SNAP"', "PRINT INP(&H3D9)", points(1)],
            [DEFAULTS[1], palette(0, 2, 4, 6), palette(0, 10, 12, 14),
             palette(0, 3, 5, 7), DEFAULTS[1], palette(15, 11, 13, 15),
             palette(0, 10, 12, 14)], "63 16 0 1 2 3", same_pixels=True)

    def test_color_fn_reads_current_globals(self):
        self.check_program(
            ["DEF FNB(X)=B%+X", "DEF FNP=P%", "B%=17:P%=0",
             "SCREEN 1", *pixels(1), "COLOR FNB(0),FNP", 'PRINT "SNAP"',
             "B%=0:P%=1:COLOR FNB(1),FNP", 'PRINT "SNAP"', points(1)],
            [palette(1, 10, 12, 14), palette(1, 3, 5, 7)],
            "0 1 2 3", same_pixels=True)

    def test_color_trailing_comments(self):
        self.check_program(
            ["SCREEN 1", *pixels(1), "PSET (1,0),1 ' comment: PRINT 999",
             "COLOR 17,0 ' comment: COLOR 0,1",
             'PRINT "SNAP"', "COLOR ,1 ' keep background", 'PRINT "SNAP"',
             "COLOR , ' unchanged", 'PRINT "SNAP"', points(1)],
            [palette(1, 10, 12, 14), palette(1, 11, 13, 15), palette(1, 11, 13, 15)],
            "0 1 2 3", redraws=[True, True, False], same_pixels=True)

    def test_color_fractional_arguments(self):
        self.check_program(
            ["SCREEN 1", *pixels(1), "COLOR 255.4", 'PRINT "SNAP"',
             "COLOR 0.5", 'PRINT "SNAP"', "COLOR &HD7,0", 'PRINT "SNAP"', points(1)],
            [palette(15, 11, 13, 15), palette(1, 3, 5, 7), palette(7, 10, 12, 14)],
            "0 1 2 3", same_pixels=True)

    def test_color_preserves_strings_during_collection(self):
        for tail in ("", ",256"):
            with self.subTest(error=bool(tail)):
                self.check_program(
                    ["SCREEN 1", 'G$=STRING$(24,88)', 'A$="KEPT"', 'G$=""',
                     "ON ERROR GOTO 100", 'COLOR FRE("")*0' + tail,
                     'B$=STRING$(80,89)', "PRINT A$", "END", "RESUME NEXT"],
                    [], "KEPT")

    def test_nested_collection_and_temporary_error_unwind(self):
        self.check_program(
            ["SCREEN 1", "ON ERROR GOTO 100", "FOR I=1 TO 180",
             'COLOR VAL(LEFT$("1",1/0)),0', "NEXT",
             'COLOR VAL(LEFT$("1",1+0*FRE("")+0*LEN(STRING$(80,50)))),0',
             'PRINT "SNAP"', "PRINT INP(&H3D9)", "END", "RESUME NEXT"],
            [{0: RGBI[1]}], "1")

    def test_fn_parameters_restored_after_nested_error(self):
        reference = json.loads((REPO / "tests/compatibility/cga-color.json").read_text())
        self.check_program(
            ["X%=7", "DEF FNA(X%)=1/0", "SCREEN 1", "COLOR FNA(17),0", "PRINT X%"],
            [], str(reference["fn_error_probe"]["value"]), trap_errors=True)
        self.check_program(
            ['X%=7:S$="SAVED"',
             'DEF FNB$(S$)=LEFT$(S$,1+0*FRE("")+1/0)',
             'DEF FNA(X%)=LEN(FNB$("INNER"))', "SCREEN 1",
             "COLOR FNA(17),0", 'PRINT X%;S$;INP(&H3D9)'],
            [], "7 SAVED 48", trap_errors=True)

    def test_fn_string_parameters_survive_collection(self):
        self.check_program(
            ['S$="SAVED"', 'DEF FNB$(S$)=LEFT$(S$,1+0*FRE(""))',
             "SCREEN 1", 'COLOR VAL(FNB$("1")),0',
             'PRINT S$;INP(&H3D9)'], [], "SAVED 1")

    def test_fn_string_argument_has_one_collector_owner(self):
        self.check_program(
            ['DEF FNA(S$)=FRE("")', 'A=FNA("")',
             'B=FNA(STRING$(20,65))', "PRINT A-B"], [], "20")
        self.check_program(
            ["CLEAR 64", 'DEF FNA(S$)=FRE("")',
             'PRINT FNA(STRING$(40,65))'], [], "24")

    def test_hosted_entries_start_with_fresh_graphics(self):
        for error in (False, True):
            source = ('10 SCREEN 1\n20 PRINT POINT(5,5);INP(&H3D9)\n'
                      '30 PSET (5,5),2:COLOR 1,0\n40 ' + ("ERROR 5" if error else "END") + "\n")
            host = ('#include "gwrt.h"\nint palette_entry(int,char**);\nint main(void){\n'
                    'for(int i=0;i<2;i++){if(palette_entry(0,NULL)!=' + str(int(error)) +
                    ' || gfx_active() || gfx_get_mode() || gfx_cga_color_in())return 1;}return 0;}\n')
            expected = "0 48" + (" Illegal function call in 40" if error else "")
            for flags in (["-O0"], ["-O3", "--safe", "--inline-arrays"]):
                with self.subTest(error=error, flags=flags):
                    self.assertEqual(self.run_hosted(source, host, flags), expected + " " + expected)

    def test_foreign_functions_in_color_arguments(self):
        source = ("10 '$EXTERN Tint(INTEGER) AS INTEGER\n"
                  "20 '$EXTERN Echo(STRING) AS STRING\n"
                  '30 SCREEN 1:PRINT Tint(0.6):COLOR Tint(LEN(Echo("x"))-0.4),0\n'
                  '40 PRINT INP(&H3D9)\n50 END\n')
        host = ('#include "gwrt.h"\nint palette_entry(int,char**);\n'
                'int16_t Tint(int16_t x){return 17+x;}\n'
                'const char *Echo(const char *s){return s;}\n'
                'int main(void){return palette_entry(0,NULL);}\n')
        for dynamic in (False, True):
            for flags in (["-O0"], ["-O3", "--safe", "--inline-arrays"]):
                with self.subTest(dynamic=dynamic, flags=flags):
                    prefix = ("5 IF 0 THEN DEFINT Z\n" if dynamic else
                              "5 '$EXTERN Unused(INTEGER) AS INTEGER\n")
                    self.assertEqual(self.run_hosted(prefix + source, host, flags), "18 18")

    def test_foreign_function_used_only_by_open(self):
        source = ("10 '$EXTERN FileName() AS STRING\n"
                  "11 '$EXTERN Unused() AS STRING\n"
                  '12 DATA Unused,"Unused"\n'
                  "13 REM Unused()\n"
                  '20 OPEN FileName() FOR OUTPUT AS #1\n'
                  '30 PRINT #1,"OK":CLOSE #1:END\n')
        host = ('#include "gwrt.h"\n#include <string.h>\nint palette_entry(int,char**);\n'
                'const char *FileName(void){return "output.txt";}\n'
                'int main(void){if(palette_entry(0,NULL))return 1;'
                'FILE *f=fopen("output.txt","r");char s[32];'
                'if(!f || !fgets(s,sizeof s,f))return 2;fclose(f);return strncmp(s,"OK",2)!=0;}\n')
        for flags in (["-O0"], ["-O3", "--safe", "--inline-arrays"]):
            with self.subTest(flags=flags):
                self.assertEqual(self.run_hosted(source, host, flags), "")

    def test_unused_foreign_names_in_declarations_and_destinations(self):
        host = '#include "gwrt.h"\nint palette_entry(int,char**);int main(void){return palette_entry(0,NULL);}\n'
        cases = [
            ('20 DEFINT Q\n30 PRINT 42\n', "42"),
            ('20 DEF FNQ=17\n30 PRINT FNQ\n', "17"),
            ('20 Q=42\n30 PRINT 17\n', "17"),
            ('20 DEF FNA(Q)=17\n30 PRINT FNA(42)\n', "17"),
            ('20 OPEN "x" FOR OUTPUT AS #1\n30 CLOSE:PRINT 17\n', "17"),
            ('20 FOR Q=1 TO 2:PRINT 42:NEXT Q\n', "42 42"),
            ('20 Q$="12":MID$(Q$,1)="X":PRINT 17\n', "17"),
            ('20 Q$="12":LSET Q$="X":RSET Q$="X":PRINT 17\n', "17"),
            ('20 OPEN "R",#1,"field.bin",4\n30 FIELD #1,4 AS Q$\n40 CLOSE:PRINT 17\n', "17"),
            ('20 DIM Q%(10):SCREEN 1:PSET (0,0),1\n30 GET (0,0)-(0,0),Q%\n'
             '40 PUT (1,1),Q%:SCREEN 0:PRINT 17\n', "17"),
            ('20 OPEN "x" FOR OUTPUT AS #1:CLOSE\n30 NAME "x" AS "y":PRINT 17\n', "17"),
            ('20 DEF SEG=&H40:PRINT 17\n', "17"),
            ('20 SCREEN 1:LINE (0,0)-(2,2),1,B:LINE (3,0)-(5,2),1,BF\n'
             '30 SCREEN 0:PRINT 17\n', "17"),
            ('20 SAVE "saved.bas",A:PRINT 17\n', "17"),
        ]
        for source, expected in cases:
            with self.subTest(source=source):
                prefix = ("10 '$EXTERN Q() AS INTEGER\n11 '$EXTERN Output() AS INTEGER\n"
                          "12 '$EXTERN As() AS STRING\n13 '$EXTERN Seg() AS INTEGER\n"
                          "14 '$EXTERN B() AS INTEGER\n15 '$EXTERN BF() AS INTEGER\n"
                          "16 '$EXTERN A() AS INTEGER\n17 '$EXTERN P() AS INTEGER\n")
                self.assertEqual(self.run_hosted(prefix + source, host, ["-O0"]), expected)

    def test_foreign_calls_in_bounds_subscripts_and_fn_bodies(self):
        source = ("10 '$EXTERN Getn() AS INTEGER\n"
                  '20 DIM A%(Getn)\n30 A%(Getn)=42\n'
                  '40 DEF FNA=Getn\n50 READ A%(Getn):DATA 17\n'
                  '60 PRINT A%(FNA);Getn\n')
        host = ('#include "gwrt.h"\nint palette_entry(int,char**);'
                'int16_t Getn(void){return 2;}int main(void){return palette_entry(0,NULL);}\n')
        for flags in (["-O0"], ["-O3", "--safe", "--inline-arrays"]):
            with self.subTest(flags=flags):
                self.assertEqual(self.run_hosted(source, host, flags), "17 2")
        for body, expected in [
            ('20 S$="abc":MID$(S$,Getn,1)="X":PRINT S$\n', "aXc"),
            ('20 OPEN "R",#1,"field.bin",4\n30 FIELD #1,Getn AS S$\n'
             '40 LSET S$="XY":PRINT S$:CLOSE\n', "XY"),
        ]:
            with self.subTest(body=body):
                source = "10 '$EXTERN Getn() AS INTEGER\n" + body
                self.assertEqual(self.run_hosted(source, host, ["-O0"]), expected)
        source = ("10 '$EXTERN As() AS STRING\n"
                  '20 OPEN "x" FOR OUTPUT AS #1:CLOSE\n'
                  '30 NAME As AS "y":PRINT "RENAMED"\n')
        host = ('#include "gwrt.h"\nint palette_entry(int,char**);'
                'const char *As(void){return "x";}int main(void){return palette_entry(0,NULL);}\n')
        self.assertEqual(self.run_hosted(source, host, ["-O0"]), "RENAMED")

    def test_foreign_integer_error_releases_string_arguments(self):
        source = ("10 '$EXTERN Mix(STRING,INTEGER) AS INTEGER\n"
                  "20 SCREEN 1:ON ERROR GOTO 100\n"
                  '30 FOR I=1 TO 180:PRINT Mix("x",32768)\n'
                  '40 COLOR Mix("x",32768),0\n'
                  '50 NEXT:PRINT "DONE":END\n100 RESUME NEXT\n')
        host = ('#include "gwrt.h"\n#include <stdlib.h>\nint palette_entry(int,char**);\n'
                'int16_t Mix(const char *s,int16_t n){(void)s;(void)n;abort();}\n'
                'int main(void){return palette_entry(0,NULL);}\n')
        self.assertEqual(self.run_hosted(source, host, ["-O0"], sanitize=True), "DONE")

    def test_palette_and_color_redraw_without_drawing(self):
        self.check_program(
            ["SCREEN 1", *pixels(1), 'PRINT "SNAP"',
             "PALETTE 0,4", 'PRINT "SNAP"', "PALETTE 1,1", 'PRINT "SNAP"',
             "COLOR ,0", 'PRINT "SNAP"', "PALETTE", 'PRINT "SNAP"', points(1)],
            [DEFAULTS[1], palette(4, 11, 13, 15), palette(4, 1, 13, 15),
             palette(0, 10, 12, 14), DEFAULTS[1]], "0 1 2 3", same_pixels=True)

    def test_screen2_out_changes_foreground(self):
        self.check_program(
            ["SCREEN 2", *pixels(2), 'PRINT "SNAP"',
             "OUT &H3D9,4", 'PRINT "SNAP"', "PALETTE", 'PRINT "SNAP"', points(2)],
            [DEFAULTS[2], palette(0, 4), DEFAULTS[2]], "0 1", same_pixels=True)

    def test_color_errors_match_reference(self):
        # Error numbers come from the pinned Microsoft-source executable.
        # Separate tests cover trapped errors and native hosted reentry.
        reference = json.loads((REPO / "tests/compatibility/cga-color.json").read_text())
        rows = [*reference["mode_probe"]["cases"],
                *(dict(mode=1, **row) for row in reference["argument_probe"]["cases"]),
                *(dict(mode=1, **row) for row in reference["extended_probe"]["cases"]),
                *(dict(mode=2, **row) for row in reference["mode2_probe"]["cases"]),
                *(dict(mode=0, **row) for row in reference["text_probe"]["cases"])]
        cases = {(row["mode"], row["args"]): row["error"] for row in rows if row["error"]}
        for (mode, args), number in cases.items():
            with self.subTest(mode=mode, args=args), tempfile.TemporaryDirectory(prefix="gw-color-error-") as temp:
                source = Path(temp) / "error.bas"
                source.write_text(f'10 SCREEN {mode}\n20 COLOR {args}\n30 PRINT "UNREACHED"\n')
                subprocess.run([str(BUILD / "gwbasic-compile"), str(source), "-c", "--keep-c",
                                "--runtime", str(REPO)], check=True, capture_output=True, timeout=30)
                self.assertNotIn("gw_run_loop();", Path(str(source) + ".c").read_text())
                for status, command in [(0, [str(BUILD / "gwbasic"), str(source)]),
                                        (1, [str(source.with_suffix(""))])]:
                    run = subprocess.run(command, capture_output=True, text=True, timeout=30)
                    self.assertEqual(run.returncode, status)
                    self.assertIn(number, (5, 6))
                    error = "Overflow" if number == 6 else "Illegal function call"
                    self.assertEqual(run.stdout.strip(), error + " in 20")

    def test_text_color_output(self):
        self.check_program(["SCREEN 0", "COLOR 7,0", 'PRINT "OK"'], [],
                           "\x1b[37m\x1b[40mOK")

    def test_recorded_probe_latches_after_success_and_error(self):
        reference = json.loads((REPO / "tests/compatibility/cga-color.json").read_text())
        for name in ("mode_probe", "argument_probe", "extended_probe", "mode2_probe"):
            with self.subTest(probe=name):
                statements, expected = [], []
                for row in reference[name]["cases"]:
                    if row["args"] == "1 2":
                        continue  # Recorded, existing numeric-space lexical difference.
                    mode = row.get("mode", 2 if name == "mode2_probe" else 1)
                    if name != "mode_probe":
                        statements.append("SCREEN 0")
                    statements.extend([f"SCREEN {mode}", "E=0", "COLOR " + row["args"], "PRINT E;INP(&H3D9)"])
                    expected.extend([str(row["error"]), str(row["bios_colour"])])
                self.check_program(statements, [], " ".join(expected), trap_errors=True,
                                   error_handler="E=ERR:RESUME NEXT")


if __name__ == "__main__":
    unittest.main()
