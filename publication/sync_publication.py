#!/usr/bin/env python3
"""Synchronise published GitHub refs to SDF and deploy a successful Docs build."""
from contextlib import contextmanager
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import select
import shlex
import subprocess
import tarfile
import tempfile
import time
import urllib.request
from urllib.parse import quote

from deploy_site import inventory, save

REPO = 'evvaletov/gw-basic-2026'
ORIGIN = 'https://github.com/' + REPO + '.git'
SDF = 'ssh://git@git.sdf.org:2222/evv/gw-basic-2026.git'
STATE = Path.home() / '.local/state/gwbasic-publication'
LEASE = '/home/evaletov/.claude-skills-shared/browser-workflow/scripts/resource_lease.py'
REFS = ('refs/heads/main', 'refs/tags/v*', 'refs/bugs/*', 'refs/identities/*')


def run(args, **kwargs):
    return subprocess.check_output([str(a) for a in args], timeout=240, **kwargs)


def ssh(state):
    return ['ssh', '-F', str(Path.home() / '.ssh/config'), '-x',
            '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15',
            '-o', 'StrictHostKeyChecking=yes',
            '-o', 'ControlMaster=no', '-o', 'ControlPath=none',
            '-o', 'GlobalKnownHostsFile=/dev/null', '-o', 'VerifyHostKeyDNS=no',
            '-o', 'UserKnownHostsFile=' + str(state / 'known_hosts'),
            '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=3']


@contextmanager
def lease(state, resource):
    command = shlex.join(['python3', LEASE, '--resource', resource,
                          '--owner', 'gwbasic-publication-asteroid01', '--expected-seconds', '300'])
    process = subprocess.Popen(ssh(state) + ['starfield01', command], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True)

    def response():
        if not select.select([process.stdout], [], [], 30)[0]:
            raise TimeoutError('Lease authority did not respond')
        line = process.stdout.readline()
        if not line:
            raise RuntimeError('Central lease authority closed the connection')
        return json.loads(line)

    def contact():
        process.stdin.write('renew 300\n')
        process.stdin.flush()
        if response().get('state') != 'HELD':
            raise RuntimeError('Central resource lease could not be renewed')
        process.stdin.write('status\n')
        process.stdin.flush()
        if response().get('state') != 'HELD':
            raise RuntimeError('Central resource lease is not held')

    try:
        if response().get('state') != 'ACQUIRED':
            raise RuntimeError('Central resource lease unavailable: ' + resource)
        yield contact
    finally:
        if process.poll() is None:
            try:
                process.stdin.write('release\n')
                process.stdin.flush()
                process.communicate(timeout=15)
            except (OSError, subprocess.TimeoutExpired):
                process.kill()
                process.communicate()


def refs(repository):
    output = run(['git', '-C', repository, 'for-each-ref', '--format=%(refname) %(objectname)', *REFS], text=True)
    return dict(line.split() for line in output.splitlines())


def sync_refs(state):
    repository = state / 'source.git'
    if not repository.exists():
        run(['git', 'init', '--bare', repository])
    # This private cache follows GitHub; no developer checkout is read or reset.
    run(['git', '-C', repository, 'fetch', '--no-tags', '--prune', ORIGIN,
         *['+' + ref + ':' + ref for ref in REFS]])
    wanted = refs(repository)
    if 'refs/heads/main' not in wanted:
        raise RuntimeError('Canonical main is missing')
    env = dict(os.environ, GIT_SSH_COMMAND=shlex.join(ssh(state)))
    with lease(state, 'repository/git.sdf.org/evv/gw-basic-2026') as contact:
        contact()
        # No forced updates, deletion or unrelated branches. Divergence fails.
        run(['git', '-C', repository, 'push', '--atomic', SDF,
             *[name + ':' + name for name in sorted(wanted)]], env=env)
        contact()
        actual = {name: sha for sha, name in
                  (line.split() for line in run(['git', 'ls-remote', SDF, *REFS], env=env, text=True).splitlines())}
        if any(actual.get(name) != sha for name, sha in wanted.items()):
            raise RuntimeError('SDF readback differs from canonical refs')
    return dict(status='synchronised', source_commit=wanted['refs/heads/main'], refs=len(wanted))


def package_site(site, sha, run_id):
    files = inventory(site)
    # Include every file, so the server rejects anything outside its allowlist.
    all_files = sorted(site.rglob('*'))
    if any(p.is_symlink() for p in all_files):
        raise ValueError('Symlink in CI artifact')
    manifest = dict(source_commit=sha, workflow_run=run_id, files=files)
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w:gz') as archive:
        for path in all_files:
            if path.is_file():
                archive.add(path, arcname=path.relative_to(site).as_posix(), recursive=False)
        raw = json.dumps(manifest).encode()
        entry = tarfile.TarInfo('deployment.json')
        entry.size = len(raw)
        archive.addfile(entry, io.BytesIO(raw))
    return output.getvalue(), manifest


def verify_public(manifest, contact=None, names=None):
    for number, name in enumerate(names if names is not None else manifest['files']):
        expected = manifest['files'][name]
        if contact and number % 10 == 0:
            contact()
        with urllib.request.urlopen('https://valetov.com/' + quote(name, safe='/'), timeout=20) as response:
            actual = hashlib.sha256(response.read()).hexdigest()
        if actual != expected:
            raise RuntimeError('Public HTTPS bytes differ: ' + name)


def deploy_site(state, sha):
    last = state / 'website.json'
    if last.exists():
        manifest = json.loads(last.read_text())
        if manifest['source_commit'] == sha:
            pages = ('index.html', 'gwbasic/index.html', 'gwbasic/install.html',
                     'articles/gw-basic-2026/index.html')
            verify_public(manifest, names=pages)
            return dict(status='verified-current', source_commit=sha,
                        workflow_run=manifest['workflow_run'], verified_files=len(pages))
    runs = json.loads(run(['gh', 'run', 'list', '--repo', REPO, '--workflow', 'docs.yml',
                          '--branch', 'main', '--event', 'push', '--commit', sha, '--limit', '1',
                          '--json', 'databaseId,conclusion,status,headSha'], text=True))
    if not runs or runs[0]['status'] != 'completed':
        return dict(status='waiting-for-docs', source_commit=sha)
    build = runs[0]
    if build['conclusion'] != 'success' or build['headSha'] != sha:
        raise RuntimeError('Latest Docs run for canonical main did not succeed')
    run_id = build['databaseId']
    with tempfile.TemporaryDirectory(prefix='site-', dir=state) as temp:
        site = Path(temp)
        run(['gh', 'run', 'download', str(run_id), '--repo', REPO,
             '--name', 'valetov-static-site', '--dir', site])
        data, manifest = package_site(site, sha, run_id)
        with lease(state, 'website/valetov.com') as contact:
            current = run(['git', 'ls-remote', ORIGIN, 'refs/heads/main'], text=True).split()[0]
            if current != sha:
                return dict(status='main-advanced', source_commit=sha)
            contact()
            receipt = json.loads(run(ssh(state) + ['zurichaws', 'sudo -n -u apache /usr/local/libexec/gwbasic-deploy'], input=data))
            contact()
            verify_public(manifest, contact)
            contact()
        save(state / 'website.json', dict(manifest, receipt=receipt))
        return dict(receipt, workflow_run=run_id, verified_files=len(manifest['files']))


def main():
    os.umask(0o077)
    STATE.mkdir(parents=True, exist_ok=True)
    with (STATE / 'lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = dict(started=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
        errors = []
        try:
            result['sdf'] = sync_refs(STATE)
        except Exception as exc:
            errors.append('SDF: ' + str(exc))
        # A mirror outage must not suppress an otherwise valid website update.
        try:
            sha = run(['git', 'ls-remote', ORIGIN, 'refs/heads/main'], text=True).split()[0]
            result['website'] = deploy_site(STATE, sha)
        except Exception as exc:
            errors.append('Website: ' + str(exc))
        result.update(status='failed' if errors else 'ok', errors=errors,
                      finished=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
        save(STATE / 'status.json', result)
        print(json.dumps(result), flush=True)
        if errors:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
