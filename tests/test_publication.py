#!/usr/bin/env python3
"""Exercise deployment failure recovery and bounded Git mirror updates locally."""
from contextlib import contextmanager
import io
import gzip
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import unquote, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'publication'))
import deploy_site as deploy
import sync_publication as sync


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'live'
        self.state = self.base / 'state'
        self.payload = self.base / 'payload'
        self.state.mkdir()
        for directory in (self.root, self.payload):
            for name in deploy.PAGES + deploy.ASSETS + ('gwbasic/index.html', 'gwbasic/install.html', 'gwbasic/searchindex.js'):
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
        (self.root / 'favicon.svg').write_text('untouched favicon')
        (self.root / 'articles/other.html').write_text('unrelated article')
        (self.root / 'gwbasic/obsolete.html').write_text('stale page')
        (self.payload / 'gwbasic/index.html').write_text('new documentation')
        (self.payload / 'index.html').write_text('new navigation')
        self.before = dict(source_commit='a' * 40, files=deploy.inventory(self.root))
        deploy.save(self.state / 'current.json', self.before)
        self.prepare = patch.object(deploy, 'prepare')
        self.prepare.start()
        self.addCleanup(self.prepare.stop)

    def archive(self):
        return sync.package_site(self.payload, 'b' * 40, 123)[0]

    def assert_restored(self):
        self.assertEqual(deploy.inventory(self.root), self.before['files'])
        self.assertEqual(json.loads((self.state / 'current.json').read_text()), self.before)
        self.assertEqual((self.root / 'favicon.svg').read_text(), 'untouched favicon')
        self.assertEqual((self.root / 'articles/other.html').read_text(), 'unrelated article')

    def test_publish_removes_stale_docs_and_preserves_siblings_then_is_idempotent(self):
        data = self.archive()
        receipt = deploy.install(data, self.root, self.state)
        self.assertEqual(receipt['status'], 'installed')
        self.assertEqual(deploy.inventory(self.root), deploy.inventory(self.payload))
        self.assertFalse((self.root / 'gwbasic/obsolete.html').exists())
        self.assertEqual((self.root / 'favicon.svg').read_text(), 'untouched favicon')
        self.assertEqual((self.root / 'articles/other.html').read_text(), 'unrelated article')
        self.assertEqual(deploy.install(data, self.root, self.state)['status'], 'unchanged')
        self.assertEqual(len(list(self.state.glob('backup-*'))), 1)

    def test_manual_live_edit_is_not_overwritten(self):
        (self.root / 'index.html').write_text('manual edit')
        with self.assertRaisesRegex(ValueError, 'changed outside'):
            deploy.install(self.archive(), self.root, self.state)
        self.assertEqual((self.root / 'index.html').read_text(), 'manual edit')

    def test_reject_changed_historical_evidence(self):
        (self.payload / deploy.ASSETS[0]).write_text('altered article notebook')
        with self.assertRaisesRegex(ValueError, 'Historical article'):
            deploy.install(self.archive(), self.root, self.state)
        self.assert_restored()

    def test_reject_payload_manifest_mismatch(self):
        with patch.object(sync, 'inventory', return_value={}):
            bad = self.archive()
        with self.assertRaisesRegex(ValueError, 'Manifest'):
            deploy.install(bad, self.root, self.state)
        self.assert_restored()

    def test_reject_paths_links_and_unmanaged_files(self):
        for name, kind in (('../escape', tarfile.REGTYPE), ('/escape', tarfile.REGTYPE),
                           ('gwbasic/link', tarfile.SYMTYPE), ('favicon.svg', tarfile.REGTYPE),
                           ('gwbasic/.htaccess', tarfile.REGTYPE), ('gwbasic/payload.php', tarfile.REGTYPE)):
            with self.subTest(name=name):
                output = io.BytesIO()
                with tarfile.open(fileobj=output, mode='w:gz') as archive:
                    entry = tarfile.TarInfo(name)
                    entry.type = kind
                    entry.size = 1 if kind == tarfile.REGTYPE else 0
                    entry.linkname = '/etc/passwd' if kind == tarfile.SYMTYPE else ''
                    archive.addfile(entry, io.BytesIO(b'x'))
                with self.assertRaisesRegex(ValueError, 'Invalid archive'):
                    deploy.install(output.getvalue(), self.root, self.state)
                self.assert_restored()

    def test_expanded_tar_limit_includes_metadata(self):
        with patch.object(deploy, 'LIMIT', 1024):
            with self.assertRaisesRegex(ValueError, 'Expanded archive'):
                deploy.install(gzip.compress(b'\0' * 4096), self.root, self.state)
        self.assert_restored()

    def test_replace_file_preserves_a_preexisting_temporary_sibling(self):
        sibling = self.root / '.index.html.gwbasic-new'
        sibling.symlink_to(self.root / 'favicon.svg')
        deploy.install(self.archive(), self.root, self.state)
        self.assertTrue(sibling.is_symlink())
        self.assertEqual((self.root / 'favicon.svg').read_text(), 'untouched favicon')

    def test_failure_after_directory_switch_rolls_back(self):
        original = deploy.replace_file
        calls = []

        def fail_once(source, target):
            calls.append(target)
            if len(calls) == 2:
                raise OSError('injected file replacement failure')
            original(source, target)

        with patch.object(deploy, 'replace_file', side_effect=fail_once):
            with self.assertRaisesRegex(OSError, 'injected'):
                deploy.install(self.archive(), self.root, self.state)
        self.assert_restored()
        self.assertFalse((self.state / 'pending.json').exists())
        self.assertEqual(len(list(self.state.glob('backup-*'))), 0)

    def test_interrupted_deployment_is_recovered_on_next_run(self):
        original = deploy.recover
        calls = []

        def stop_recovery(root, state):
            calls.append(1)
            if len(calls) == 1:
                return original(root, state)
            raise RuntimeError('simulated process death before recovery')

        with patch.object(deploy, 'recover', side_effect=stop_recovery), patch.object(
                deploy, 'replace_file', side_effect=OSError('interrupted')):
            with self.assertRaisesRegex(RuntimeError, 'process death'):
                deploy.install(self.archive(), self.root, self.state)
        self.assertTrue((self.state / 'pending.json').exists())
        deploy.recover(self.root, self.state)
        self.assert_restored()

    def test_reject_symlinked_managed_directory(self):
        (self.root / 'gwbasic').rename(self.root / 'elsewhere')
        (self.root / 'gwbasic').symlink_to('elsewhere')
        with self.assertRaisesRegex(ValueError, 'Symlink'):
            deploy.install(self.archive(), self.root, self.state)

    def test_current_site_does_not_need_expired_ci_artifact(self):
        _, manifest = sync.package_site(self.payload, 'b' * 40, 123)
        deploy.save(self.state / 'website.json', manifest)
        with patch.object(sync, 'lease', no_lease), patch.object(sync, 'verify_public') as verify, patch.object(sync, 'run') as run:
            self.assertEqual(sync.deploy_site(self.state, 'b' * 40)['status'], 'verified-current')
            verify.assert_called_once()
            run.assert_not_called()

    def test_pending_or_failed_ci_never_touches_host(self):
        for status, conclusion in [('in_progress', ''), ('completed', 'failure')]:
            with self.subTest(status=status), patch.object(sync, 'run', return_value=json.dumps([
                    dict(databaseId=123, status=status, conclusion=conclusion, headSha='b' * 40)])), patch.object(sync, 'lease') as lease:
                if status == 'completed':
                    with self.assertRaisesRegex(RuntimeError, 'did not succeed'):
                        sync.deploy_site(self.state, 'b' * 40)
                else:
                    self.assertEqual(sync.deploy_site(self.state, 'b' * 40)['status'], 'waiting-for-docs')
                lease.assert_not_called()

    def test_successful_backup_retention_is_bounded(self):
        for version in range(7):
            (self.payload / 'index.html').write_text('navigation ' + str(version))
            deploy.install(self.archive(), self.root, self.state)
        self.assertEqual(len(list(self.state.glob('backup-*'))), 5)

    def test_controller_installs_exact_artifact_and_verifies_public_bytes(self):
        import shutil

        def command(args, **kwargs):
            if args[:3] == ['gh', 'run', 'list']:
                return json.dumps([dict(databaseId=123, status='completed', conclusion='success', headSha='b' * 40)])
            if args[:3] == ['gh', 'run', 'download']:
                shutil.copytree(self.payload, args[-1], dirs_exist_ok=True)
                return b''
            if args[:2] == ['git', 'ls-remote']:
                return 'b' * 40 + '\trefs/heads/main\n'
            if args[0] == 'ssh':
                return json.dumps(deploy.install(kwargs['input'], self.root, self.state)).encode()
            raise AssertionError(args)

        def public(url, **kwargs):
            return io.BytesIO((self.root / unquote(urlsplit(url).path).lstrip('/')).read_bytes())

        with patch.object(sync, 'run', side_effect=command), patch.object(sync, 'lease', no_lease), patch.object(sync.urllib.request, 'urlopen', side_effect=public):
            result = sync.deploy_site(self.state, 'b' * 40)
            self.assertEqual(result['status'], 'installed')
            cached = json.loads((self.state / 'website.json').read_text())
            self.assertEqual(cached['files'], deploy.inventory(self.payload))
            self.assertEqual(result['verified_files'], len(cached['files']))
            self.assertEqual(sync.deploy_site(self.state, 'b' * 40)['status'], 'verified-current')

    def test_main_advance_after_artifact_download_defers_install(self):
        import shutil
        calls = []

        def command(args, **kwargs):
            calls.append(args)
            if args[:3] == ['gh', 'run', 'list']:
                return json.dumps([dict(databaseId=123, status='completed', conclusion='success', headSha='b' * 40)])
            if args[:3] == ['gh', 'run', 'download']:
                shutil.copytree(self.payload, args[-1], dirs_exist_ok=True)
                return b''
            if args[:2] == ['git', 'ls-remote']:
                return 'c' * 40 + '\trefs/heads/main\n'
            raise AssertionError('Unexpected mutation: ' + repr(args))

        with patch.object(sync, 'run', side_effect=command), patch.object(sync, 'lease', no_lease):
            self.assertEqual(sync.deploy_site(self.state, 'b' * 40)['status'], 'main-advanced')
        self.assert_restored()
        self.assertFalse((self.state / 'website.json').exists())

    def test_public_verification_quotes_names_and_rejects_wrong_bytes(self):
        manifest = dict(files={'gwbasic/a #b.html': '0' * 64})
        with patch.object(sync.urllib.request, 'urlopen', return_value=io.BytesIO(b'wrong')) as get:
            with self.assertRaisesRegex(RuntimeError, 'Public HTTPS bytes differ'):
                sync.verify_public(manifest)
            self.assertEqual(get.call_args.args[0], 'https://valetov.com/gwbasic/a%20%23b.html')


@contextmanager
def no_lease(*args):
    yield lambda: None


class MirrorTests(unittest.TestCase):
    def test_sync_and_divergence_with_real_git(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            source, target, state = (base / name for name in ('source', 'target.git', 'state'))
            state.mkdir()

            def git(*args):
                return subprocess.check_output(['git', *map(str, args)], stderr=subprocess.DEVNULL, text=True).strip()

            git('init', '-b', 'main', source)
            git('-C', source, 'config', 'user.email', 'test@example.invalid')
            git('-C', source, 'config', 'user.name', 'Test')
            git('-C', source, 'commit', '--allow-empty', '-m', 'first')
            first = git('-C', source, 'rev-parse', 'HEAD')
            git('-C', source, 'tag', 'v1')
            git('-C', source, 'update-ref', 'refs/bugs/test', first)
            git('-C', source, 'update-ref', 'refs/identities/test', first)
            git('-C', source, 'branch', 'unrelated')
            git('init', '--bare', target)
            with patch.object(sync, 'ORIGIN', str(source)), patch.object(sync, 'SDF', str(target)), patch.object(sync, 'lease', no_lease):
                result = sync.sync_refs(state)
                self.assertEqual(result['refs'], 4)
                self.assertEqual(git('--git-dir', target, 'rev-parse', 'refs/heads/main'), first)
                self.assertFalse(git('--git-dir', target, 'for-each-ref', 'refs/heads/unrelated'))
                git('-C', source, 'commit', '--allow-empty', '-m', 'second')
                second = git('-C', source, 'rev-parse', 'HEAD')
                git('--git-dir', target, 'fetch', source, 'main')
                git('--git-dir', target, 'update-ref', 'refs/tags/v1', second)
                with self.assertRaises(subprocess.CalledProcessError):
                    sync.sync_refs(state)
                self.assertEqual(git('--git-dir', target, 'rev-parse', 'refs/tags/v1'), second)
                self.assertEqual(git('--git-dir', target, 'rev-parse', 'refs/heads/main'), first)


if __name__ == '__main__':
    unittest.main(verbosity=2)
