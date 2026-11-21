#!/usr/bin/python3 -I
"""Install a checked static-site archive on the existing Linux Apache host."""
import argparse
import ctypes
import fcntl
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

CONFIG = Path('/etc/gwbasic-publication.json')
PAGES = ('articles/gw-basic-2026/index.html', 'sitemap.xml', 'robots.txt', 'index.html')
ASSETS = tuple('articles/gw-basic-2026/' + name for name in
               ('examples.ipynb', 'graphics.png', 'squares-output.txt', 'squares.bas.c', 'verification.json'))
LIMIT = 32 * 1024 * 1024
STATIC_SUFFIXES = {'.html', '.js', '.css', '.png', '.svg', '.gif', '.ico', '.txt',
                   '.woff', '.woff2', '.ttf', '.eot', '.map', '.inv'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    """Only the managed paths; sibling pages and favicons are never touched."""
    for directory in (root, root / 'gwbasic', root / 'articles', root / 'articles/gw-basic-2026'):
        if directory.is_symlink():
            raise ValueError('Symlink in managed site: ' + str(directory))
    paths = [root / name for name in PAGES + ASSETS]
    paths += list((root / 'gwbasic').rglob('*'))
    result = {}
    for path in paths:
        if path.is_symlink():
            raise ValueError('Symlink in managed site: ' + str(path))
        if path.is_file():
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def save(path, data):
    temp = path.with_suffix('.tmp')
    with temp.open('w') as stream:
        stream.write(json.dumps(data, indent=2) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)
    directory = os.open(path.parent, os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def exchange(first, second):
    """Linux renameat2 exchanges whole directories without a missing-page gap."""
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.renameat2(-100, os.fsencode(first), -100, os.fsencode(second), 2):
        raise OSError(ctypes.get_errno(), 'renameat2 exchange failed')


def prepare(path, reference):
    for item in [path, *path.rglob('*')] if path.is_dir() else [path]:
        os.chmod(item, 0o755 if item.is_dir() else 0o644)
    subprocess.run(['chcon', '-R', '--reference=' + str(reference), str(path)], check=True,
                   stdout=subprocess.DEVNULL)


def replace_file(source, target, reference):
    fd, name = tempfile.mkstemp(prefix='page-', dir=source.parent)
    os.close(fd)
    temp = Path(name)
    try:
        shutil.copy2(source, temp)
        prepare(temp, reference)
        os.replace(temp, target)
    finally:
        temp.unlink(missing_ok=True)


def unpack(data, target):
    """Write regular files ourselves; tar paths and links never reach extractall."""
    seen = set()
    total = 0
    # Bound headers and PAX metadata too, before tarfile interprets them.
    with gzip.GzipFile(fileobj=io.BytesIO(data)) as compressed:
        raw = compressed.read(LIMIT + 1)
    if len(raw) > LIMIT:
        raise ValueError('Expanded archive exceeds limit')
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as archive:
        for entry in archive:
            name = entry.name
            parts = PurePosixPath(name).parts
            static = (name.startswith('gwbasic/') and
                      not any(part.startswith('.') for part in parts) and
                      PurePosixPath(name).suffix.lower() in STATIC_SUFFIXES)
            allowed = name in PAGES + ASSETS or name == 'deployment.json' or static
            if (not entry.isfile() or not parts or '..' in parts or name.startswith('/')
                    or str(PurePosixPath(name)) != name or name in seen or not allowed):
                raise ValueError('Invalid archive member: ' + name)
            seen.add(name)
            total += entry.size
            if entry.size < 0 or total > LIMIT or len(seen) > 2000:
                raise ValueError('Site archive exceeds limits')
            path = target / name
            path.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(entry) as source, path.open('wb') as dest:
                shutil.copyfileobj(source, dest)
    meta = json.loads((target / 'deployment.json').read_text())
    hashes = inventory(target)
    if hashes != meta['files'] or seen != set(hashes) | {'deployment.json'}:
        raise ValueError('Manifest does not match payload')
    required_docs = ('gwbasic/index.html', 'gwbasic/install.html', 'gwbasic/demos.html',
                     'gwbasic/language-reference.html', 'gwbasic/searchindex.js')
    if not set(PAGES + ASSETS + required_docs) <= set(hashes):
        raise ValueError('Required site files are missing')
    if len(meta['source_commit']) != 40 or any(c not in '0123456789abcdef' for c in meta['source_commit']):
        raise ValueError('Invalid source commit')
    return meta


def recover(root, state):
    journal = state / 'pending.json'
    if not journal.exists():
        return
    pending = json.loads(journal.read_text())
    backup = state / pending['backup']
    if inventory(root) != pending['before']['files']:
        with tempfile.TemporaryDirectory(prefix='restore-', dir=state) as temp:
            docs = Path(temp) / 'gwbasic'
            shutil.copytree(backup / 'gwbasic', docs, copy_function=shutil.copy2)
            prepare(docs, root / 'index.html')
            exchange(docs, root / 'gwbasic')
            for name in PAGES:
                replace_file(backup / name, root / name, root / 'index.html')
    if inventory(root) != pending['before']['files']:
        raise RuntimeError('Rollback verification failed; keep pending.json and backup')
    save(state / 'current.json', pending['before'])
    journal.unlink()
    shutil.rmtree(backup)


def install(data, root, state):
    if root.stat().st_dev != state.stat().st_dev:
        raise ValueError('State and site must be on the same filesystem')
    recover(root, state)
    # A killed process cannot run TemporaryDirectory cleanup. The host lock
    # makes these private, reserved paths unambiguously orphaned on re-entry.
    for pattern in ('stage-*', 'restore-*'):
        for orphan in state.glob(pattern):
            shutil.rmtree(orphan)
    for backup in state.glob('backup-*'):
        if not (backup / 'completed.json').exists():
            shutil.rmtree(backup)
    before = json.loads((state / 'current.json').read_text())
    if inventory(root) != before['files']:
        raise ValueError('Managed live files changed outside deployment; reconcile first')
    with tempfile.TemporaryDirectory(prefix='stage-', dir=state) as temp:
        payload = Path(temp)
        meta = unpack(data, payload)
        if any(meta['files'][name] != before['files'][name] for name in ASSETS):
            raise ValueError('Historical article evidence changed; requires a separate reviewed update')
        if meta['files'] == before['files']:
            save(state / 'current.json', meta)
            return dict(status='unchanged', source_commit=meta['source_commit'])
        prepare(payload / 'gwbasic', root / 'index.html')
        backup = state / ('backup-' + str(time.time_ns()))
        backup.mkdir(mode=0o700)
        try:
            shutil.copytree(root / 'gwbasic', backup / 'gwbasic', copy_function=shutil.copy2)
            for name in PAGES:
                target = backup / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(root / name, target)
            save(backup / 'baseline.json', before)
            if inventory(root) != before['files']:
                raise ValueError('Live files changed during staging; reconcile first')
            save(state / 'pending.json', dict(backup=backup.name, before=before))
            exchange(payload / 'gwbasic', root / 'gwbasic')
            for name in PAGES:
                replace_file(payload / name, root / name, root / 'index.html')
            if inventory(root) != meta['files']:
                raise RuntimeError('Published files differ from manifest')
            save(state / 'current.json', meta)
            save(backup / 'completed.json', dict(source_commit=meta['source_commit']))
            (state / 'pending.json').unlink()
        except BaseException:
            recover(root, state)
            if backup.exists():
                shutil.rmtree(backup)
            raise
        for old in sorted(state.glob('backup-*'))[:-5]:
            shutil.rmtree(old)
        return dict(status='installed', source_commit=meta['source_commit'], backup=str(backup))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--initialise', action='store_true', help='Record the reviewed existing live files once')
    args = parser.parse_args()
    os.umask(0o077)
    config = json.loads(CONFIG.read_text())
    root, state = Path(config['root']), Path(config['state'])
    if not root.is_absolute() or not state.is_absolute() or root in state.parents or state in root.parents or root == state:
        raise ValueError('Site and state must be separate absolute paths')
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (state / 'lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.initialise:
            if (state / 'current.json').exists() or (state / 'pending.json').exists():
                raise ValueError('Already initialised or recovery pending')
            files = inventory(root)
            if not set(PAGES + ASSETS + ('gwbasic/index.html',)) <= set(files):
                raise ValueError('Existing site is incomplete')
            save(state / 'current.json', dict(source_commit=None, files=files))
            print(json.dumps(dict(status='initialised', files=len(files))))
        else:
            data = sys.stdin.buffer.read(LIMIT + 1)
            if len(data) > LIMIT:
                raise ValueError('Compressed archive exceeds limit')
            print(json.dumps(install(data, root, state)))


if __name__ == '__main__':
    main()
