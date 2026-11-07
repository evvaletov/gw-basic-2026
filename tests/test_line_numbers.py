#!/usr/bin/env python3
"""Exercise ASCII label boundaries and editing/loading round trips."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
SOURCE = '''0 PRINT "ZERO"
32767 PRINT "SIGNED MAX"
32768 PRINT "HIGH"
40000 PRINT "FORTY"
65529 PRINT "LAST"
'''
EXPECTED = ["ZERO", "SIGNED MAX", "HIGH", "FORTY", "LAST"]


class LineNumberTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gw-lines-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "source.bas").write_text(SOURCE)

    def run_basic(self, commands="", filename=None):
        argv = [str(BUILD / "gwbasic")]
        if filename:
            argv.append(filename)
        result = subprocess.run(argv, cwd=self.root, input=commands, text=True,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result)
        self.assertNotIn("Sanitizer", result.stderr)
        self.assertNotIn("runtime error:", result.stderr)
        return [line.strip() for line in result.stdout.splitlines() if line.strip()]

    def test_cli_ascii(self):
        self.assertEqual(self.run_basic(filename="source.bas"), EXPECTED)

    def test_direct_and_label_without_space(self):
        self.assertEqual(self.run_basic(SOURCE + '40000PRINT "REPLACED"\nRUN\nSYSTEM\n'),
                         EXPECTED[:3] + ["REPLACED", "LAST"])

    def test_load_merge_and_save_round_trips(self):
        (self.root / "more.bas").write_text('40000 PRINT "MERGED"\n')
        commands = ('LOAD "source.bas"\nMERGE "more.bas"\n'
                    'SAVE "ascii.bas",A\nSAVE "binary.bas"\n'
                    'NEW\nLOAD "ascii.bas"\nRUN\n'
                    'NEW\nLOAD "binary.bas"\nRUN\nSYSTEM\n')
        expected = EXPECTED[:3] + ["MERGED", "LAST"]
        self.assertEqual(self.run_basic(commands), expected * 2)
        self.assertRegex((self.root / "ascii.bas").read_text(), r'65529 PRINT +"LAST"')

    def test_high_list_and_delete(self):
        lines = self.run_basic('LOAD "source.bas"\nLIST 32768-65529\n'
                               'DELETE 32768-65529\nRUN\nSYSTEM\n')
        self.assertEqual([" ".join(line.split()) for line in lines], ['32768 PRINT "HIGH"', '40000 PRINT "FORTY"',
                                 '65529 PRINT "LAST"'] + EXPECTED[:2])

    def test_renum_high_targets_and_back(self):
        source = ('10 GOSUB 100:ON 2 GOTO 50,60\n50 PRINT "BAD":END\n'
                  '60 IF 0 THEN 50 ELSE 70\n70 PRINT "DONE":END\n'
                  '100 PRINT "SUB":RETURN\n')
        commands = (source + 'RENUM 40000,0,100\nSAVE "high.bas",A\n'
                    'NEW\nLOAD "high.bas"\nRUN\nRENUM 10,0,10\n'
                    'SAVE "low.bas",A\nNEW\nLOAD "low.bas"\nRUN\nSYSTEM\n')
        self.assertEqual(self.run_basic(commands), ["SUB", "DONE"] * 2)
        high = " ".join((self.root / "high.bas").read_text().split())
        self.assertIn("GOSUB 40400", high)
        self.assertIn("GOTO 40100,40200", high)
        self.assertIn("THEN 40100 ELSE 40300", high)

    def test_invalid_labels_do_not_wrap_or_store(self):
        for label in ("65530", "65535", "65536", "999999999999999999999999999"):
            with self.subTest(label=label):
                source = '0 PRINT "SAFE"\n' + label + ' PRINT "BAD"\n'
                (self.root / "invalid.bas").write_text(source)
                for commands, filename in [(source + 'RUN\nSYSTEM\n', None),
                                           ('', 'invalid.bas'),
                                           ('LOAD "invalid.bas"\nRUN\nSYSTEM\n', None)]:
                    self.assertEqual(self.run_basic(commands, filename), ["Syntax error", "SAFE"])
                result = subprocess.run([str(BUILD / "gwbasic-compile"), "invalid.bas"],
                                        cwd=self.root, text=True, capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Line number out of range", result.stderr)
                self.assertFalse(result.stdout)

    def test_binary_round_trip_after_long_renum(self):
        source = '1 PRINT "DONE":END\n10 ON 1 GOTO ' + ','.join(['1'] * 70) + '\n'
        commands = (source + 'RENUM 40000,0,10\nSAVE "long.bas"\n'
                    'NEW\nLOAD "long.bas"\nRUN 40010\nSYSTEM\n')
        self.assertEqual(self.run_basic(commands), ["DONE"])
        self.assertGreater((self.root / "long.bas").stat().st_size, 400)

    def test_running_renum_stops_and_preserves_values(self):
        commands = ('10 A=42:DIM B(1):B(1)=7:RENUM 40000:GOTO 30\n'
                    '20 PRINT "BAD"\n30 PRINT "BAD":END\n'
                    'RUN\nPRINT A;B(1)\nSYSTEM\n')
        self.assertEqual([" ".join(line.split()) for line in self.run_basic(commands)], ["42 7"])

    def test_renum_preserves_function_and_data_anchors(self):
        commands = ('10 DEF FNA(X)=X+1:GOTO 30\n'
                    '20 DATA 11,22:GOTO 30\n30 READ A:END\n'
                    'RUN\nRENUM 40000\nPRINT FNA(4)\nREAD B:PRINT B\nSYSTEM\n')
        self.assertEqual(self.run_basic(commands), ["5", "22"])

    def test_empty_range_renum_stops(self):
        commands = ('10 RENUM 40000,20:PRINT "BAD"\n'
                    'RUN\nPRINT "NEXT COMMAND"\nSYSTEM\n')
        self.assertEqual(self.run_basic(commands), ["NEXT COMMAND"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=BUILD)
    args, rest = parser.parse_known_args()
    BUILD = args.build.resolve()
    unittest.main(argv=[__file__] + rest)
