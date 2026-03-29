"""Smoke test for the GW-BASIC Jupyter kernel.

Spawns the kernel class directly and exercises the do_execute path
via the subprocess protocol. No running Jupyter instance required.
"""

import os
import sys

# Ensure gwbasic binary is found
os.environ['GWBASIC'] = os.path.join(os.path.dirname(__file__), '..', 'build', 'gwbasic')


def test_kernel():
    from .kernel import GWBasicKernel

    # Minimal mock — we only need the subprocess communication, not ZMQ
    class MockKernel(GWBasicKernel):
        execution_count = 1
        iopub_socket = None
        _captured = []

        def __init__(self):
            # Skip Kernel.__init__ (needs ZMQ); just init our state
            self._proc = None
            self._timeout = 10

        def send_response(self, socket, msg_type, content):
            self._captured.append((msg_type, content))

    k = MockKernel()

    def run(code):
        k._captured = []
        result = k.do_execute(code, silent=False)
        output = ''
        for msg_type, content in k._captured:
            if msg_type == 'stream':
                output += content.get('text', '')
        return result, output.strip()

    # Test 1: Simple PRINT
    r, out = run('PRINT "Hello Jupyter"')
    assert r['status'] == 'ok', f'Expected ok, got {r}'
    assert 'Hello Jupyter' in out, f'Expected Hello Jupyter, got: {out}'
    print(f'  PASS  PRINT: {out}')

    # Test 2: Arithmetic
    r, out = run('PRINT 6*7')
    assert r['status'] == 'ok'
    assert '42' in out, f'Expected 42, got: {out}'
    print(f'  PASS  Arithmetic: {out}')

    # Test 3: State persistence across cells
    r, _ = run('X = 42')
    assert r['status'] == 'ok'
    r, out = run('PRINT X')
    assert '42' in out, f'Expected 42, got: {out}'
    print(f'  PASS  State persistence: {out}')

    # Test 4: Error handling
    r, out = run('PRINT 1/0')
    assert r['status'] == 'error', f'Expected error, got {r}'
    assert 'Division by zero' in out, f'Expected Division by zero, got: {out}'
    print(f'  PASS  Error: {out}')

    # Test 5: Recovery after error
    r, out = run('PRINT "OK after error"')
    assert r['status'] == 'ok'
    assert 'OK after error' in out
    print(f'  PASS  Recovery: {out}')

    # Test 6: Program RUN
    r, _ = run('10 PRINT "Alpha"\n20 PRINT "Beta"')
    assert r['status'] == 'ok'
    r, out = run('RUN')
    assert 'Alpha' in out and 'Beta' in out, f'Expected Alpha+Beta, got: {out}'
    print(f'  PASS  RUN: {out}')

    # Test 7: Magic %reset
    r, out = run('%reset')
    assert r['status'] == 'ok'
    assert 'reset' in out.lower()
    print(f'  PASS  %reset: {out}')

    # Test 8: String functions
    r, out = run('PRINT LEFT$("HELLO", 3)')
    assert 'HEL' in out, f'Expected HEL, got: {out}'
    print(f'  PASS  String functions: {out}')

    # Test 9: Multi-statement line
    r, out = run('A=10:B=20:PRINT A+B')
    assert '30' in out, f'Expected 30, got: {out}'
    print(f'  PASS  Multi-statement: {out}')

    # Test 10: FRE returns real value
    r, out = run('PRINT FRE(0)')
    assert r['status'] == 'ok'
    # Should be a number > 0
    print(f'  PASS  FRE: {out}')

    k.do_shutdown(False)
    print(f'\n10 tests passed.')


if __name__ == '__main__':
    test_kernel()
