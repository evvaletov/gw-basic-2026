"""Install the GW-BASIC kernel into Jupyter."""

import argparse
import shutil
import sys
import tempfile
from pathlib import Path


def install_kernel(user=True, prefix=None):
    from jupyter_client.kernelspec import KernelSpecManager

    kernel_json = Path(__file__).parent / 'kernel.json'

    with tempfile.TemporaryDirectory() as td:
        dest = Path(td) / 'gwbasic'
        dest.mkdir()
        shutil.copy(kernel_json, dest / 'kernel.json')

        ksm = KernelSpecManager()
        ksm.install_kernel_spec(
            str(dest), kernel_name='gwbasic', user=user, prefix=prefix,
        )

    print('GW-BASIC kernel installed successfully.')
    print('Run "jupyter kernelspec list" to verify.')


def main():
    parser = argparse.ArgumentParser(description='Install the GW-BASIC Jupyter kernel')
    parser.add_argument('--user', action='store_true', default=True,
                        help='Install for the current user (default)')
    parser.add_argument('--sys-prefix', action='store_true',
                        help='Install into sys.prefix (for virtualenvs)')
    parser.add_argument('--prefix', type=str, default=None,
                        help='Install into a specific prefix')
    args = parser.parse_args()

    if args.sys_prefix:
        install_kernel(user=False, prefix=sys.prefix)
    elif args.prefix:
        install_kernel(user=False, prefix=args.prefix)
    else:
        install_kernel(user=args.user)


if __name__ == '__main__':
    main()
