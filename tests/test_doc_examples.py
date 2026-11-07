#!/usr/bin/env python3
"""Prove that documentation checks reject representative broken examples."""
from dataclasses import replace
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from run_doc_examples import EXAMPLES, REPO, load_snippets, parse_snippets, run_command, run_example


class DocumentationChecks(unittest.TestCase):
    def setUp(self):
        self.snippets = load_snippets(REPO)

    def example(self, name):
        return next(example for example in EXAMPLES if example.name == name)

    def change(self, name, old, new):
        snippet = self.snippets[name]
        self.assertIn(old, snippet.text)
        self.snippets[name] = replace(snippet, text=snippet.text.replace(old, new))

    def run_case(self, name):
        run_example(self.example(name), self.snippets, REPO, REPO / 'build')

    def test_wrong_documented_binary_path_fails(self):
        self.change('readme-run', 'build/gwbasic', './gwbasic')
        with self.assertRaisesRegex(AssertionError, 'readme-run.*exit 127'):
            self.run_case('readme-interpreter')

    def test_missing_run_is_an_output_failure(self):
        self.change('guide-pipe', r'\nRUN\n', r'\n')
        with self.assertRaisesRegex(AssertionError, 'output mismatch'):
            self.run_case('guide-pipe')

    def test_pipeline_failure_is_not_hidden(self):
        self.snippets['guide-pipe'] = replace(self.snippets['guide-pipe'], text='false | true\n')
        with self.assertRaisesRegex(AssertionError, 'exit 1'):
            self.run_case('guide-pipe')

    def test_c_driver_abi_mismatch_fails_before_execution(self):
        self.change('guide-c-driver', 'extern int run_basic_greet(int argc, char **argv);',
                    'extern void run_basic_greet(void);')
        self.change('guide-c-driver', 'run_basic_greet(0, NULL)', 'run_basic_greet()')
        with self.assertRaisesRegex(AssertionError, 'conflicting types'):
            self.run_case('guide-c-host')

    def test_c_provider_abi_mismatch_fails_before_execution(self):
        self.change('guide-extern-library', 'int16_t     Getn(void)', 'double      Getn(void)')
        with self.assertRaisesRegex(AssertionError, 'conflicting types'):
            self.run_case('guide-extern')

    def test_fortran_symbol_mismatch_fails_linking(self):
        self.change('guide-fortran-driver', 'name="run_basic_greet"', 'name="missing_basic_greet"')
        with self.assertRaisesRegex(AssertionError, 'missing_basic_greet'):
            self.run_case('guide-fortran-host')

    def test_marker_errors_fail_instead_of_dropping_coverage(self):
        valid = '<!-- docs-test: example -->\n```bash\ntrue\n```\n'
        for text, diagnostic in [
            (valid + valid, 'duplicate'),
            (valid.replace('example -->', 'BAD -->'), 'malformed'),
            (valid.replace('```bash', '```python'), 'fenced block'),
            (valid.removesuffix('```\n'), 'unclosed'),
            (valid.replace('true\n', ''), 'empty'),
            (valid.replace('true\n', ' \t\n'), 'empty'),
            (valid.replace('\n```\n', '\n    ```\n'), 'closing fence'),
            (valid.replace('\n```\n', '\n``` trailing\n'), 'closing fence'),
        ]:
            with self.subTest(diagnostic=diagnostic), self.assertRaisesRegex(ValueError, diagnostic):
                parse_snippets(text, 'README.md')

    def test_missing_snippets_fail_cli_and_write_report(self):
        with tempfile.TemporaryDirectory(prefix='gw-doc-missing-') as temporary:
            root = Path(temporary)
            (root / 'docs').mkdir()
            (root / 'README.md').write_text('No examples\n')
            (root / 'docs/getting-started.md').write_text('No examples\n')
            report = root / 'report.json'
            result = subprocess.run([sys.executable, str(REPO / 'tests/run_doc_examples.py'),
                                     '--repo', str(root), '--report', str(report)],
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('inventory mismatch', result.stdout)
            self.assertFalse(json.loads(report.read_text())['success'])

    def test_timeout_kills_descendant_process(self):
        with tempfile.TemporaryDirectory(prefix='gw-doc-timeout-') as temporary:
            root = Path(temporary)
            child = 'import time; time.sleep(0.6); open("survived", "w").close()'
            parent = f'import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",{child!r}]); time.sleep(30)'
            with self.assertRaisesRegex(AssertionError, 'timeout'):
                run_command([sys.executable, '-c', parent], root, timeout=0.2)
            time.sleep(0.7)
            self.assertFalse((root / 'survived').exists())

    def test_normal_exit_kills_descendants(self):
        with tempfile.TemporaryDirectory(prefix='gw-doc-children-') as temporary:
            root = Path(temporary)
            # A handshake ensures the child is alive before its parent exits.
            child = ('import pathlib,time; pathlib.Path("ready").touch(); '
                     'time.sleep(0.6); pathlib.Path("survived").touch()')
            parent = ('import pathlib,subprocess,sys,time; '
                      f'subprocess.Popen([sys.executable,"-c",{child!r}]); '
                      '\nwhile not pathlib.Path("ready").exists(): time.sleep(0.01)\n'
                      'print("parent done",flush=True)')
            self.assertEqual(run_command([sys.executable, '-c', parent], root), 'parent done\n')
            time.sleep(0.7)
            self.assertFalse((root / 'survived').exists())

    def test_noisy_output_is_bounded_while_running(self):
        with tempfile.TemporaryDirectory(prefix='gw-doc-noise-') as temporary:
            for descriptor in (1, 2):
                with self.subTest(descriptor=descriptor), patch('run_doc_examples.MAX_LOG_BYTES', 65536):
                    source = f'import os\nwhile True: os.write({descriptor},b"x"*16384)'
                    with self.assertRaisesRegex(AssertionError, 'output exceeds 65536') as error:
                        run_command([sys.executable, '-c', source], Path(temporary), timeout=5)
                    self.assertLess(len(str(error.exception)), 66000)

    def test_duplex_input_and_large_artifacts(self):
        with tempfile.TemporaryDirectory(prefix='gw-doc-stream-') as temporary:
            root = Path(temporary)
            source = ('import os,sys; os.write(2,b"x"*65536); '
                      'data=sys.stdin.buffer.read(); open("artifact","wb").write(b"z"*2097152); '
                      'print(len(data))')
            self.assertEqual(run_command([sys.executable, '-c', source], root,
                                          input_text='a'*262144), '262144\n')
            self.assertEqual((root / 'artifact').stat().st_size, 2097152)


if __name__ == '__main__':
    unittest.main()
