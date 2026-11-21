#!/usr/bin/env python3
"""Check that the real site build satisfies the installed deployment contract."""
import argparse
from pathlib import Path
import tempfile

from deploy_site import unpack
from sync_publication import package_site


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('site', type=Path)
    args = parser.parse_args()
    data, manifest = package_site(args.site, '0' * 40, 0)
    with tempfile.TemporaryDirectory() as temp:
        if unpack(data, Path(temp)) != manifest:
            raise ValueError('Deployment manifest changed during validation')
    print('Validated deployment archive:', len(manifest['files']), 'files')


if __name__ == '__main__':
    main()
