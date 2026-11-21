#!/usr/bin/env python3
"""Check SCREEN defaults and PALETTE resets in interpreted and native Sixel output."""
from pathlib import Path
import re
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


def pixels(mode):
    return [f"PSET ({colour},0),{colour}" for colour in DEFAULTS[mode]]


def points(mode):
    return "PRINT " + ";".join(f"POINT({colour},0)" for colour in DEFAULTS[mode])


class GraphicsPalette(unittest.TestCase):
    def check_program(self, statements, expected_palettes, expected_text):
        with tempfile.TemporaryDirectory(prefix="gw-palette-") as directory:
            root = Path(directory)
            source = root / "palette.bas"
            source.write_text("".join(f"{10 * (i + 1)} {s}\n"
                                      for i, s in enumerate([*statements, "END"])))

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
                    # Each marker follows a drawing command, which flushes a frame.
                    # Check the last frame before each marker; intermediate frames
                    # depend on how many pixels the BASIC program has drawn so far.
                    sections = output.split("SNAP\n")
                    self.assertEqual(len(sections), len(expected_palettes) + 1)
                    for section, expected in zip(sections, expected_palettes):
                        frames = SIXEL.findall(section)
                        self.assertTrue(frames, "missing Sixel frame")
                        actual = {int(a): (int(r), int(g), int(b))
                                  for a, r, g, b in COLOUR.findall(frames[-1])}
                        self.assertEqual(actual, expected)
                    text = SIXEL.sub("", output).replace("SNAP\n", "")
                    self.assertEqual(" ".join(text.split()), expected_text)

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
        for start, modes in [(1, [1]), (2, [2]), (1, [2, 1]), (2, [1, 2]),
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


if __name__ == "__main__":
    unittest.main()
