#!/usr/bin/env python3
"""Exercise the real terminal editor, including the two GitHub input reports."""
import argparse
from pathlib import Path
import tempfile
import time
import unittest

import pexpect
import pyte


BUILD = Path(__file__).resolve().parents[1] / "build"


class InteractiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gw-tui-")
        self.root = Path(self.temp.name)
        self.child = pexpect.spawn(str(BUILD / "gwbasic"), cwd=self.temp.name,
                                   encoding="utf-8", dimensions=(25, 80), echo=False)
        self.screen = pyte.Screen(80, 25)
        self.stream = pyte.Stream(self.screen)
        self.wait_for(lambda: "Ok" in self.lines())

    def tearDown(self):
        self.child.terminate(force=True)
        self.temp.cleanup()

    def lines(self):
        return [line.rstrip() for line in self.screen.display]

    def pump(self):
        try:
            self.stream.feed(self.child.read_nonblocking(65536, timeout=0.1))
        except pexpect.TIMEOUT:
            pass

    def wait_for(self, predicate):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            self.pump()
            if predicate():
                return
        self.fail("Terminal did not reach expected state:\n" + "\n".join(self.lines()))

    def command(self, text):
        self.child.send(text + "\r")
        # Drain complete redraws before checking absence/count assertions.
        end = time.monotonic() + 0.25
        while time.monotonic() < end:
            self.pump()

    def load(self, source):
        (self.root / "test.bas").write_text(source)
        self.command('LOAD "test.bas"')

    def test_numbered_edits_do_not_print_ok(self):
        self.assertEqual(self.lines().count("Ok"), 1)
        for text in ['10 PRINT "HELLO"', '20 END', '20', '']:
            self.command(text)
            self.assertEqual(self.lines().count("Ok"), 1)
        self.command("RUN")
        self.assertIn("HELLO", self.lines())
        self.assertEqual(self.lines().count("Ok"), 2)
        self.command("PRINT 1/0")
        self.assertTrue(any("Division by zero" in line for line in self.lines()))
        self.assertEqual(self.lines().count("Ok"), 3)

    def test_auto_does_not_duplicate_line_number(self):
        self.command("AUTO 10,10")
        self.command('PRINT "AUTO WORKS"')
        self.command("")
        self.command("RUN")
        self.assertIn("AUTO WORKS", self.lines())
        self.assertFalse(any("Syntax error" in line for line in self.lines()))

    def test_numeric_and_string_input_exclude_prompt(self):
        self.load('10 INPUT "N:",N\n20 INPUT "NAME";N$\n'
                  '30 PRINT "NUMBER=";N\n40 PRINT "VALUE=[";N$;"]"\n')
        self.command("RUN")
        self.wait_for(lambda: any(line == "N:" for line in self.lines()))
        self.command("42")
        self.wait_for(lambda: "NAME?" in self.lines())
        self.command("Alice")
        self.assertIn("NUMBER= 42", self.lines())
        self.assertIn("VALUE=[Alice]", self.lines())
        self.assertIn("N:42", self.lines())

    def test_wrapped_line_input_and_prompt_boundary(self):
        self.load('10 LOCATE 24,70\n20 LINE INPUT "VALUE:";S$\n'
                  '30 OPEN "answer.txt" FOR OUTPUT AS #1\n'
                  '40 PRINT #1,S$\n50 CLOSE #1\n60 PRINT "SAVED"\n')
        self.command("RUN")
        # Home/Backspace must not erase the prompt. Preserve spaces and all
        # characters across wrapping and scrolling at the bottom of the screen.
        answer = "  " + "abcdefghijklmnop" * 10 + "  "
        self.child.send("\x1b[H\x7f" + answer + "\r")
        self.wait_for(lambda: (self.root / "answer.txt").exists() and "SAVED" in self.lines())
        self.assertEqual((self.root / "answer.txt").read_text().rstrip("\r\n"), answer)

    def test_input_editing(self):
        self.load('10 INPUT "N:",N\n20 PRINT "RESULT=";N\n')
        self.command("RUN")
        # 124 -> move left, delete 4, insert 3 -> 123.
        self.child.send("124\x1b[D\x1b[3~3\r")
        self.wait_for(lambda: "RESULT= 123" in self.lines())

    def test_input_break_returns_to_prompt(self):
        self.load('10 INPUT "N:",N\n20 PRINT "MUST NOT RUN"\n')
        self.command("RUN")
        self.child.send("\x03")
        self.wait_for(lambda: any("Break" in line for line in self.lines()))
        self.command('PRINT "READY"')
        self.assertIn("READY", self.lines())
        self.assertNotIn("MUST NOT RUN", self.lines())

    def test_issue_6_guessing_game(self):
        self.load('10 INPUT "Random seed? ",seed\n20 RANDOMIZE seed\n'
                  '30 num=INT(RND(1)*100)+1\n40 PRINT "Guess a number between 1 and 100"\n'
                  '50 INPUT "Your guess? ",guess\n60 IF guess=0 THEN END\n'
                  '70 IF guess<num THEN PRINT "Too low"\n80 IF guess>num THEN PRINT "Too high"\n'
                  '90 IF guess<>num THEN GOTO 50\n100 PRINT "That\'s right!"\n110 END\n')
        self.command("RUN")
        self.command("1")
        self.command("1")
        self.assertIn("Too low", self.lines())
        self.assertNotIn("That's right!", self.lines())
        self.command("100")
        self.assertIn("Too high", self.lines())
        self.command("76")
        self.assertIn("That's right!", self.lines())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=BUILD)
    args, remaining = parser.parse_known_args()
    BUILD = args.build.resolve()
    unittest.main(argv=[__file__, *remaining])
