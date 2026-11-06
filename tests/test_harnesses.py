#!/usr/bin/env python3
"""Exercise the shell harnesses with controlled executables and fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPTS = ("run_tests.sh", "run_compiler_tests.sh")
TESTS = Path(__file__).resolve().parent
PROGRAM = '''import os, signal, sys, time
if config.get("ignore_term"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
time.sleep(config.get("sleep", 0))
sys.stdout.write(os.getcwd() if config.get("print_cwd") else config.get("output", "OK\\n"))
sys.stderr.write(config.get("stderr", ""))
sys.exit(config.get("exit", 0))
'''


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gw harness ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "project"
        self.tests = self.root / "tests"
        self.programs = self.tests / "programs"
        self.expected = self.tests / "expected"
        self.build = self.root / "build"
        for directory in (self.programs, self.expected, self.build):
            directory.mkdir(parents=True, exist_ok=True)
        for name in (*SCRIPTS, "harness_common.sh"):
            shutil.copy2(TESTS / name, self.tests / name)
        self.write_executable(self.build / "gwbasic", (
            "import json, sys\nfrom pathlib import Path\n"
            "config = json.loads(Path(sys.argv[-1]).read_text())\n" + PROGRAM
        ))
        self.write_executable(self.build / "gwbasic-compile", f'''
import json, sys, time
from pathlib import Path
source = Path(sys.argv[-1])
config = json.loads(source.read_text())
time.sleep(config.get("compile_sleep", 0))
sys.stderr.write(config.get("compile_stderr", ""))
if config.get("compile_exit"):
    sys.exit(config["compile_exit"])
if not config.get("no_exe"):
    target = source.with_suffix("")
    target.write_text({('#!' + sys.executable + chr(10))!r} + "config = " + repr(config) + "\\n" + {PROGRAM!r})
    target.chmod(0o755)
''')
        (self.build / "libgwrt.a").touch()

    @staticmethod
    def write_executable(path, code):
        path.write_text(f"#!{sys.executable}\n{code}")
        path.chmod(0o755)

    def fixture(self, name="sample", expected="OK\n", **config):
        (self.programs / f"{name}.bas").write_text(json.dumps(config))
        if expected is not None:
            (self.expected / f"{name}.expected").write_text(expected)

    def run_harness(self, script, **overrides):
        env = dict(os.environ, GW_TEST_TIMEOUT="0.5", GW_TEST_COMPILE_TIMEOUT="0.5")
        env.update(overrides)
        # Invoke outside the project to test working-directory independence.
        return subprocess.run(["bash", str(self.tests / script)],
                              cwd=self.temp.name, env=env, text=True,
                              capture_output=True, timeout=15)

    def assert_result(self, result, success, *messages):
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0 if success else 1, output)
        for message in messages:
            self.assertIn(message, output)

    def test_normalized_match(self):
        self.fixture(output="  OK  \r\n\n", expected="  OK\t\n\n")
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assert_result(self.run_harness(script), True, "1 passed, 0 failed")

    def test_wrong_expected_output_fails_with_diff(self):
        self.fixture(output="ACTUAL\n", expected="EXPECTED\n")
        for script in SCRIPTS:
            with self.subTest(script=script):
                result = self.run_harness(script)
                self.assert_result(result, False, "0 passed, 1 failed", "-EXPECTED", "+ACTUAL")
                self.assertNotIn("PASS  sample.bas", result.stdout)

    def test_leading_spaces_remain_significant(self):
        self.fixture(output=" OK\n", expected="OK\n")
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assert_result(self.run_harness(script), False, "0 passed, 1 failed")

    def test_runtime_failure_shows_diagnostics(self):
        self.fixture(exit=7, stderr="runtime failed\n")
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assert_result(self.run_harness(script), False,
                                   "RUN-FAIL", "exit 7", "runtime failed")

    def test_runtime_timeout_fails(self):
        self.fixture(sleep=20)
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assert_result(self.run_harness(script), False, "RUN-TIMEOUT", "limit 0.5s")

    def test_term_resistant_runtime_is_killed(self):
        self.fixture(sleep=20, ignore_term=True)
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assert_result(self.run_harness(script), False, "RUN-KILLED")

    def test_slow_audio_fixture_gets_extra_time(self):
        self.fixture(name="play_music", sleep=0.7)
        self.assert_result(self.run_harness("run_tests.sh"), True, "PASS  play_music.bas")

    def test_timer_fixture_gets_extra_time(self):
        self.fixture(name="on_timer", sleep=0.7)
        self.assert_result(self.run_harness("run_tests.sh"), True, "PASS  on_timer.bas")

    def test_compile_failure_shows_diagnostics(self):
        self.fixture(compile_exit=9, compile_stderr="compiler failed\n")
        self.assert_result(self.run_harness("run_compiler_tests.sh"), False,
                           "COMPILE-FAIL", "exit 9", "compiler failed")

    def test_compile_timeout_fails(self):
        self.fixture(compile_sleep=20)
        self.assert_result(self.run_harness("run_compiler_tests.sh"), False, "COMPILE-TIMEOUT")

    def test_missing_executable_fails(self):
        self.fixture(no_exe=True)
        self.assert_result(self.run_harness("run_compiler_tests.sh"), False, "NO-EXE")

    def test_expected_skips_are_reported(self):
        self.fixture()
        self.fixture(name="chain_target", exit=99)
        self.fixture(name="common_target", exit=99)
        self.fixture(name="play_music")
        self.assert_result(self.run_harness("run_tests.sh"), True,
                           "2 passed, 0 failed (2 skipped)", "SKIP  chain_target.bas")
        self.assert_result(self.run_harness("run_compiler_tests.sh"), True,
                           "1 passed, 0 failed (3 skipped)", "SKIP  play_music.bas")

    def test_missing_expected_is_explicit_smoke_test(self):
        self.fixture(expected=None)
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assert_result(self.run_harness(script), True, "[no expected]")

    def test_empty_or_all_skipped_suite_fails(self):
        for only_skips in (False, True):
            if only_skips:
                self.fixture(name="chain_target", exit=99)
            for script in SCRIPTS:
                with self.subTest(script=script, only_skips=only_skips):
                    self.assert_result(self.run_harness(script), False, "no runnable")

    def test_runtime_uses_project_directory(self):
        self.fixture(print_cwd=True, expected=str(self.root))
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assert_result(self.run_harness(script), True, "1 passed, 0 failed")

    def test_invalid_timeout_is_rejected(self):
        self.fixture()
        for setting in ("GW_TEST_TIMEOUT", "GW_TEST_COMPILE_TIMEOUT"):
            for value in ("0", "bad"):
                for script in SCRIPTS:
                    with self.subTest(script=script, setting=setting, value=value):
                        self.assert_result(self.run_harness(script, **{setting: value}), False,
                                           f"{setting} must be a positive number")

    def test_comparison_tool_failure_fails(self):
        self.fixture()
        fake_bin = self.root / "fake-bin"
        fake_bin.mkdir()
        self.write_executable(fake_bin / "sed", "import sys\nprint('normalization failed')\nsys.exit(2)\n")
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assert_result(self.run_harness(script, PATH=f"{fake_bin}:{os.environ['PATH']}"),
                                   False, "COMPARE-FAIL")


if __name__ == "__main__":
    unittest.main()
