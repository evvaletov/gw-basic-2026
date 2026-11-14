#!/usr/bin/env python3
"""Exercise an installed wheel outside the checkout through Jupyter's protocol."""

import argparse
import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

from jupyter_client import KernelManager
from jupyter_client.kernelspec import KernelSpecManager


def execute(client, code):
    """Collect one execution's messages, failing on errors or a missing idle reply."""
    message_id = client.execute(code)
    messages = []
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        message = client.get_iopub_msg(timeout=max(0.1, deadline - time.monotonic()))
        if message['parent_header'].get('msg_id') != message_id:
            continue
        content = message['content']
        assert message['msg_type'] != 'error', content
        messages.append(message)
        if message['msg_type'] == 'status' and content['execution_state'] == 'idle':
            return messages
    raise AssertionError('Kernel did not become idle')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix='gwbasic-wheel-') as td:
        root = Path(td)
        prefix = root / 'kernel prefix'
        command = [sys.executable, '-m', 'gwbasickernel.install', '--prefix', str(prefix)]
        env = dict(os.environ)
        env.pop('PYTHONPATH', None)
        env.pop('GWBASIC', None)
        missing = subprocess.run(command + ['--gwbasic', str(root / 'missing')],
                                 cwd=root, env=env, capture_output=True, text=True)
        assert missing.returncode == 2, missing
        assert not (prefix / 'share/jupyter/kernels/gwbasic').exists()
        subprocess.run(command + ['--gwbasic', str(binary)], cwd=root, env=env, check=True)
        kernel_dirs = [str(prefix / 'share/jupyter/kernels')]
        ksm = KernelSpecManager(kernel_dirs=kernel_dirs)
        spec = ksm.get_kernel_spec('gwbasic')
        assert spec.argv[0] == sys.executable, spec.argv
        assert spec.env['GWBASIC'] == str(binary), spec.env
        km = KernelManager(kernel_name='gwbasic', kernel_spec_manager=ksm)
        km.start_kernel(cwd=root, env=env)
        client = km.blocking_client()
        client.start_channels()
        try:
            client.wait_for_ready(timeout=30)
            messages = execute(client, '10 FOR N=1 TO 3\n20 PRINT N*N\n30 NEXT N\nRUN')
            text = ''.join(m['content']['text'] for m in messages if m['msg_type'] == 'stream')
            assert text.split() == ['1', '4', '9'], text
            messages = execute(client, 'NEW\n10 SCREEN 1\n20 CIRCLE (160,100),80,2\nRUN')
            images = [m['content']['data']['image/png'] for m in messages
                      if m['msg_type'] == 'display_data' and 'image/png' in m['content']['data']]
            assert images, 'No inline graphics'
            png = base64.b64decode(images[-1], validate=True)
            assert png.startswith(b'\x89PNG\r\n\x1a\n'), 'Invalid PNG signature'
            width, height = struct.unpack('>II', png[16:24])
            assert width == 320 and 200 <= height <= 204, (width, height)
        finally:
            client.stop_channels()
            km.shutdown_kernel(now=True)
        print(json.dumps({'status': 'passed', 'python': sys.version.split()[0],
                          'platform': sys.platform, 'text_output': text.split(),
                          'inline_frames': len(images), 'image_size': [width, height]}))


if __name__ == '__main__':
    main()
