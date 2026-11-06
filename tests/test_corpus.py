#!/usr/bin/env python3
"""Failure-injection checks for the curated corpus runner and reference adapter."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

import corpus_reference
import run_corpus


class CorpusTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gw corpus test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "project"
        self.tests = self.repo / "tests"
        self.corpus = self.tests / "corpus"
        self.build = self.repo / "build"
        self.corpus.mkdir(parents=True)
        self.build.mkdir()
        shutil.copy2(Path(__file__).with_name("run_corpus.py"), self.tests)
        self.config = {"output": "  OK\n\n"}
        self.executable(self.build / "gwbasic", "import json,sys\nfrom pathlib import Path\nc=json.loads(Path(sys.argv[1]).read_text())\nsys.stdout.write(c['output'])\nsys.stderr.write(c.get('stderr',''))\nsys.exit(c.get('exit',0))\n")
        self.executable(self.build / "gwbasic-compile", "import json,sys\nfrom pathlib import Path\nc=json.loads(Path(sys.argv[1]).read_text())\nPath(sys.argv[sys.argv.index('-o')+1]).write_text(json.dumps(c))\nsys.exit(c.get('compile_exit',0))\n")
        self.cc = self.root / "fake cc"
        self.executable(self.cc, "import json,sys\nfrom pathlib import Path\nc=json.loads(Path(sys.argv[2]).read_text())\np=Path(sys.argv[-1])\nif not c.get('no_exe'):\n p.write_text('#!'+sys.executable+'\\nimport sys\\nsys.stdout.write('+repr(c.get('compiled_output',c['output']))+')\\n')\n p.chmod(0o755)\nsys.exit(c.get('cc_exit',0))\n")
        (self.build / "libgwrt.a").touch()
        self.fixture()

    @staticmethod
    def executable(path, code):
        path.write_text('#!' + sys.executable + '\n' + code)
        path.chmod(0o755)

    def fixture(self, **changes):
        self.config.update(changes)
        (self.corpus / 'case.bas').write_text(json.dumps(self.config))
        (self.corpus / 'input.txt').write_text('')
        (self.corpus / 'expected.txt').write_text('  OK\n\n')
        case = {'id': 'case', 'source': 'case.bas', 'input': 'input.txt', 'expected': 'expected.txt'}
        (self.corpus / 'fixtures.json').write_text(json.dumps({'cases': [case]}))
        ref = {'id': 'case'}
        for key, hash_key in [('source', 'source_sha256'), ('input', 'input_sha256'), ('expected', 'sha256')]:
            ref[hash_key] = run_corpus.digest(self.corpus / case[key])
        (self.corpus / 'reference.json').write_text(json.dumps({'cases': [ref]}))
        (self.corpus / 'sources.json').write_text(json.dumps({'revision': 'test', 'files': {'case.bas': {'sha256': ref['source_sha256']}}}))

    def run_suite(self, *flags):
        import shlex
        result = subprocess.run([sys.executable, str(self.tests / 'run_corpus.py'), '--mode', 'O0',
                                 '--report', str(self.root / 'report.json'), *flags],
                                env=dict(os.environ, CC=shlex.quote(str(self.cc))), cwd=self.root,
                                capture_output=True, text=True, timeout=15)
        return result

    def test_exact_match_and_crlf_trailing_space_normalization(self):
        self.fixture(output='  OK  \r\n\n', compiled_output='  OK\t\n\n')
        result = self.run_suite()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads((self.root / 'report.json').read_text())['passed'], 1)

    def test_shared_wrong_output_cannot_pass_parity(self):
        for output in ('WRONG\n', 'OK\n\n', '  OK\n', 'Syntax error\n'):
            with self.subTest(output=output):
                self.fixture(output=output)
                result = self.run_suite()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('mismatch', result.stdout)

    def test_compiled_only_mismatch(self):
        self.fixture(output='  OK\n\n', compiled_output='WRONG\n')
        result = self.run_suite()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('interpreter=match O0=FAIL', result.stdout)

    def test_nonzero_transpiler_with_partial_c_fails(self):
        self.fixture(compile_exit=7)
        result = self.run_suite()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        row = json.loads((self.root / 'report.json').read_text())['rows'][0]
        self.assertEqual(row['compiled']['O0']['transpile']['status'], 'exit:7')
        self.assertNotIn('cc', row['compiled']['O0'])

    def test_failed_cc_and_missing_executable(self):
        for changes, status in (({'cc_exit': 4}, 'exit:4'),
                                ({'cc_exit': 0, 'no_exe': True}, 'missing-output')):
            self.fixture(**changes)
            result = self.run_suite()
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            row = json.loads((self.root / 'report.json').read_text())['rows'][0]
            self.assertEqual(row['interpreter']['reference']['status'], 'match')
            self.assertEqual(row['compiled']['O0']['cc']['status'], status)

    def test_runtime_status_and_stderr(self):
        for changes in ({'output': '  OK\n\n', 'exit': 9}, {'exit': 0, 'stderr': 'error'}):
            self.fixture(**changes)
            result = self.run_suite()
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_unknown_empty_duplicate_and_stale_fixtures(self):
        self.assertNotEqual(self.run_suite('--case', 'missing').returncode, 0)
        fixtures = self.corpus / 'fixtures.json'
        case = json.loads(fixtures.read_text())['cases'][0]
        for cases in ([], [case, case]):
            fixtures.write_text(json.dumps({'cases': cases}))
            self.assertNotEqual(self.run_suite().returncode, 0)
        self.fixture()
        (self.corpus / 'expected.txt').write_text('WRONG')
        self.assertNotEqual(self.run_suite().returncode, 0)
        self.fixture()
        (self.corpus / 'case.bas').write_text('CHANGED')
        self.assertNotEqual(self.run_suite().returncode, 0)

    def test_missing_command_is_reported(self):
        status, output = run_corpus.execute([str(self.root / 'missing')], self.root, 'missing')
        self.assertEqual(status['status'], 'spawn-error')
        self.assertEqual(output, '')

    def test_timeout_kills_term_resistant_group(self):
        script = self.root / 'hang'
        self.executable(script, "import signal,time\nsignal.signal(signal.SIGTERM,signal.SIG_IGN)\ntime.sleep(60)\n")
        start = time.monotonic()
        status, _ = run_corpus.execute([str(script)], self.root, 'hang', timeout=0.3)
        self.assertEqual(status['status'], 'timeout')
        self.assertLess(time.monotonic() - start, 5)

    def test_reference_adapter_preserves_strings_and_comments(self):
        source = '10 PRINT "INPUT:END";:INPUT "NAME";A$\n20 REM PRINT:END\n30 DEF FNA(X)=X+256:PRINT FNA(41)\n40 END\n'
        actual = corpus_reference.adapt(source)
        self.assertIn('PRINT #9, "INPUT:END";:PRINT #9,"NAME";"? ";:INPUT #8,A$', actual)
        self.assertIn('20 REM PRINT:END\r\n', actual)
        self.assertIn('DEF FNA(X)=X+256:PRINT #9, FNA(41)', actual)
        self.assertIn('PRINT #9,"__CORPUS_DONE__":CLOSE:SYSTEM', actual)
        self.assertIn('ON ERROR GOTO 65500', actual)

    def test_reference_adapter_rejects_unsupported_io(self):
        for source in ('10 OPEN "X" FOR OUTPUT AS #1', '10 INPUT #1,A', '10 LINE INPUT A$', '0 PRINT 1'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                corpus_reference.adapt(source)


if __name__ == '__main__':
    unittest.main()
