"""Install the GW-BASIC kernel into Jupyter."""

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path


def install_kernel(user=True, prefix=None, gwbasic=None):
    from jupyter_client.kernelspec import KernelSpecManager

    spec = json.loads((Path(__file__).parent / 'kernel.json').read_text())
    spec['argv'][0] = sys.executable
    if gwbasic is not None:
        binary = shutil.which(str(gwbasic))
        if binary is None:
            raise ValueError(f'GW-BASIC executable not found: {gwbasic}')
        spec['env'] = {'GWBASIC': str(Path(binary).absolute())}

    with tempfile.TemporaryDirectory() as td:
        dest = Path(td) / 'gwbasic'
        dest.mkdir()
        (dest / 'kernel.json').write_text(json.dumps(spec, indent=2) + '\n')

        ksm = KernelSpecManager()
        ksm.install_kernel_spec(
            str(dest), kernel_name='gwbasic', user=user, prefix=prefix,
        )

    print('GW-BASIC kernel installed successfully.')
    print('Run "jupyter kernelspec list" to verify.')


def main():
    parser = argparse.ArgumentParser(description='Install the GW-BASIC Jupyter kernel')
    location = parser.add_mutually_exclusive_group()
    location.add_argument('--user', action='store_true',
                        help='Install for the current user (default)')
    location.add_argument('--sys-prefix', action='store_true',
                        help='Install into sys.prefix (for virtualenvs)')
    location.add_argument('--prefix', type=str, default=None,
                        help='Install into a specific prefix')
    parser.add_argument('--gwbasic', help='Interpreter executable to save in the kernelspec')
    args = parser.parse_args()

    try:
        if args.sys_prefix:
            install_kernel(user=False, prefix=sys.prefix, gwbasic=args.gwbasic)
        elif args.prefix:
            install_kernel(user=False, prefix=args.prefix, gwbasic=args.gwbasic)
        else:
            install_kernel(gwbasic=args.gwbasic)
    except ValueError as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
