#!/usr/bin/env python3
"""Ensure missing tools, corrupt fixtures, and failed guests cannot pass CI."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_dos_runtime as runner


class RuntimeHarnessTests(unittest.TestCase):
    def test_high_line_fixture_matches_original_capture(self):
        capture = json.loads((runner.ROOT / "tests/compatibility/dos-runtime-reference.json").read_text())
        self.assertEqual(runner.digest(runner.ROOT / capture["source"]), capture["source_sha256"])
        runner.check_output((runner.ROOT / capture["expected_file"]).read_text(),
                            capture["output"], "original high-line capture")

    def test_missing_qemu_is_nonzero_and_unavailable(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "evidence"
            result = subprocess.run([sys.executable, str(Path(runner.__file__)),
                                     "--qemu", str(Path(tmp) / "missing-qemu"),
                                     "--output", str(output)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            report = json.loads((output / "report.json").read_text())
            self.assertEqual(report["status"], "unavailable")
            self.assertEqual(report["targets"], {})

    def test_corrupt_cached_distribution_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp)
            (cache / runner.ARCHIVE).write_bytes(b"wrong distribution")
            with self.assertRaisesRegex(RuntimeError, "SHA256 mismatch"):
                runner.boot_image(cache)

    def test_incomplete_emulator_is_unavailable(self):
        with patch.object(runner, "run", return_value=b'name "VGA"\n'):
            with self.assertRaisesRegex(runner.Unavailable, "floppy support"):
                runner.check_qemu("qemu")

    def test_wrong_or_empty_output_is_rejected(self):
        for actual in ("", "DONE\n", "ARITH\n 7\nDONE\n", "ERROR\nARITH\n 8\nDONE\n"):
            with self.subTest(actual=actual), self.assertRaises(AssertionError):
                runner.check_output(actual, "ARITH\n 8\nDONE\n", "smoke")
        runner.check_output("ARITH\r\n 8 \r\nDONE\r\n", "ARITH\n8\nDONE\n", "smoke")

    def test_guest_failure_cleans_up_and_records_nonzero(self):
        for error in (TimeoutError("guest hung"), RuntimeError("QEMU exited")):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as tmp:
                args = ["run_dos_runtime.py", "--output", tmp]
                with patch.object(sys, "argv", args), patch.object(runner.shutil, "which", return_value="tool"), \
                     patch.object(Path, "is_file", return_value=True), patch.object(runner, "digest", return_value="hash"), \
                     patch.object(runner, "boot_image", return_value=b"boot"), \
                     patch.object(runner, "check_qemu", return_value="qemu"), \
                     patch.object(runner, "prepare", return_value="partition"), \
                     patch.object(runner, "run", return_value=b"partial guest output\r\n"), \
                     patch.object(runner, "VM") as vm:
                    vm.return_value.wait.side_effect = error
                    self.assertEqual(runner.main(), 1)
                    vm.return_value.close.assert_called_once()
                report = json.loads((Path(tmp) / "report.json").read_text())
                self.assertEqual(report["status"], "failed")
                self.assertEqual(report["targets"]["16"]["status"], "failed")
                self.assertNotIn("32", report["targets"])
                self.assertEqual((Path(tmp) / "dos16/HIGH.TXT").read_text(), "partial guest output\n")
                self.assertEqual(report["targets"]["16"]["captured_files"],
                                 ["OUT.TXT", "HIGH.TXT", "O11.TXT"])


if __name__ == "__main__":
    unittest.main()
