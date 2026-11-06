#!/usr/bin/env python3
"""Exercise the compiler's GCC launch with literal paths and failure statuses."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
COMPILER = Path(os.environ.get('GWBASIC_COMPILER', REPO / 'build/gwbasic-compile')).resolve()


class CompilerPaths(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gw-compiler-paths-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = self.root / "runtime space ' $() ;"
        self.runtime.mkdir()
        (self.runtime / 'include').symlink_to(REPO / 'include', target_is_directory=True)
        (self.runtime / 'build').symlink_to(REPO / 'build', target_is_directory=True)

    def compile(self, source, *options, env=None):
        return subprocess.run([str(COMPILER), str(source), '--runtime', str(self.runtime), *options],
                              cwd=self.root, env=env, text=True, capture_output=True, timeout=60)

    def test_real_gcc_literal_paths_and_output_modes(self):
        for name in ["a space ' quote $() ;.bas", '$(touch INJECTED).bas', '`touch BACKTICK`.bas',
                     'no-extension', '.hidden', 'has.dots/source']:
            with self.subTest(name=name):
                source = self.root / name
                source.parent.mkdir(parents=True, exist_ok=True)
                program = '10 PRINT "PATH OK"\n20 END\n'
                source.write_text(program)
                result = self.compile(source, '-c', '--keep-c')
                self.assertEqual(result.returncode, 0, result.stderr)
                output = source.with_suffix('') if source.suffix else Path(str(source) + '.out')
                actual = subprocess.check_output([str(output)], text=True, timeout=5)
                self.assertEqual(actual.strip(), 'PATH OK')
                self.assertEqual(source.read_text(), program)
                self.assertTrue(Path(str(source) + '.c').is_file())
                result = self.compile(source, '--emit-obj', '-o', str(self.root / "generated ' C.c"))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(source.with_suffix('.o').is_file())
        self.assertFalse((self.root / 'INJECTED').exists())
        self.assertFalse((self.root / 'BACKTICK').exists())

    def fake_gcc(self, body):
        directory = self.root / 'tools'; directory.mkdir()
        gcc = directory / 'gcc'
        gcc.write_text('#!/usr/bin/env python3\n' + body)
        gcc.chmod(0o755)
        return dict(os.environ, PATH=str(directory) + os.pathsep + os.environ['PATH'])

    def test_gcc_arguments_and_sanitizer_flags(self):
        source = self.root / "test ' path.bas"
        source.write_text('10 END\n')
        env = self.fake_gcc('import sys,json\nopen("argv.json","w").write(json.dumps(sys.argv[1:]))\n')
        result = self.compile(source, '-c', '-O3', '--safe=sanitize', env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads((self.root / 'argv.json').read_text())
        self.assertEqual(args[:3], ['-O3', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])
        self.assertEqual(args[args.index('-o') + 1], str(source.with_suffix('')))
        self.assertIn(str(source) + '.c', args)
        self.assertEqual(args[args.index('-I') + 1], str(self.runtime / 'include'))
        self.assertEqual(args[args.index('-L') + 1], str(self.runtime / 'build'))

    def test_gcc_failure_propagates(self):
        source = self.root / 'bad.bas'; source.write_text('10 END\n')
        env = self.fake_gcc('import sys\nsys.exit(23)\n')
        result = self.compile(source, '-c', env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('gcc failed (exit 23)', result.stderr)
        self.assertNotIn('Compiled:', result.stderr)
        self.assertTrue(Path(str(source) + '.c').exists())

    def test_gcc_signal_propagates(self):
        source = self.root / 'signal.bas'; source.write_text('10 END\n')
        env = self.fake_gcc('import os,signal\nos.kill(os.getpid(),signal.SIGTERM)\n')
        self.assertNotEqual(self.compile(source, '-c', env=env).returncode, 0)

    def test_missing_gcc_is_actionable(self):
        source = self.root / 'missing.bas'; source.write_text('10 END\n')
        result = self.compile(source, '-c', env=dict(os.environ, PATH=str(self.root)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Cannot execute gcc', result.stderr)


if __name__ == '__main__':
    unittest.main()
