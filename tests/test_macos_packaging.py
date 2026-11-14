#!/usr/bin/env python3
"""Reject macOS packages that would depend on the build host's libraries."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('macos_packager', REPO / 'pkg/build_macos.py')
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)


class MacOSPackagingTests(unittest.TestCase):
    def inspect(self, architecture='arm64', library='/usr/lib/libSystem.B.dylib', minimum='15.0'):
        replies = [architecture, f'test:\n\t{library} (compatibility version 1.0.0)',
                   f'Load command 10\n      cmd LC_BUILD_VERSION\n    minos {minimum}\n']
        with patch.object(PACKAGER, 'output', side_effect=replies):
            return PACKAGER.inspect_binary(Path('test'), 'arm64')

    def test_system_only_binary(self):
        self.assertEqual(self.inspect()['minimum_macos'], '15.0')

    def test_rejects_foreign_architecture(self):
        for architecture in ('x86_64', 'arm64 x86_64'):
            with self.subTest(architecture=architecture), self.assertRaises(ValueError):
                self.inspect(architecture=architecture)

    def test_rejects_external_or_relative_libraries(self):
        for library in ('/opt/homebrew/lib/libpulse.dylib', '/usr/local/lib/libpulse.dylib',
                        '@rpath/libpulse.dylib'):
            with self.subTest(library=library), self.assertRaises(ValueError):
                self.inspect(library=library)

    def test_rejects_newer_deployment_target(self):
        with self.assertRaises(ValueError):
            self.inspect(minimum='26.0')


if __name__ == '__main__':
    unittest.main(verbosity=2)
