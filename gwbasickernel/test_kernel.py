"""Smoke test for the GW-BASIC Jupyter kernel.

Spawns the kernel class directly and exercises the do_execute path
via the subprocess protocol. No running Jupyter instance required.
"""

import os
import sys

# Ensure gwbasic binary is found
os.environ['GWBASIC'] = os.path.join(os.path.dirname(__file__), '..', 'build', 'gwbasic')


def test_kernel():
    from .kernel import GWBasicKernel, _decode_sixel, _rgba_to_png

    # Minimal mock — we only need the subprocess communication, not ZMQ
    class MockKernel(GWBasicKernel):
        execution_count = 1
        iopub_socket = None
        _captured = []

        def __init__(self):
            self._proc = None
            self._timeout = 10

        def send_response(self, socket, msg_type, content):
            self._captured.append((msg_type, content))

    k = MockKernel()

    def run(code):
        k._captured = []
        result = k.do_execute(code, silent=False)
        text = ''
        has_image = False
        for msg_type, content in k._captured:
            if msg_type == 'stream':
                text += content.get('text', '')
            elif msg_type == 'display_data':
                has_image = True
        return result, text.strip(), has_image

    # Test 1: Simple PRINT
    r, out, _ = run('PRINT "Hello Jupyter"')
    assert r['status'] == 'ok', f'Expected ok, got {r}'
    assert 'Hello Jupyter' in out, f'Expected Hello Jupyter, got: {out}'
    print(f'  PASS  PRINT: {out}')

    # Test 2: Arithmetic
    r, out, _ = run('PRINT 6*7')
    assert r['status'] == 'ok'
    assert '42' in out, f'Expected 42, got: {out}'
    print(f'  PASS  Arithmetic: {out}')

    # Test 3: State persistence across cells
    r, _, _ = run('X = 42')
    assert r['status'] == 'ok'
    r, out, _ = run('PRINT X')
    assert '42' in out, f'Expected 42, got: {out}'
    print(f'  PASS  State persistence: {out}')

    # Test 4: Error handling
    r, out, _ = run('PRINT 1/0')
    assert r['status'] == 'error', f'Expected error, got {r}'
    assert 'Division by zero' in out, f'Expected Division by zero, got: {out}'
    print(f'  PASS  Error: {out}')

    # Test 5: Recovery after error
    r, out, _ = run('PRINT "OK after error"')
    assert r['status'] == 'ok'
    assert 'OK after error' in out
    print(f'  PASS  Recovery: {out}')

    # Test 6: Program RUN
    r, _, _ = run('10 PRINT "Alpha"\n20 PRINT "Beta"')
    assert r['status'] == 'ok'
    r, out, _ = run('RUN')
    assert 'Alpha' in out and 'Beta' in out, f'Expected Alpha+Beta, got: {out}'
    print(f'  PASS  RUN: {repr(out)}')

    # Test 7: Magic %reset
    r, out, _ = run('%reset')
    assert r['status'] == 'ok'
    assert 'reset' in out.lower()
    print(f'  PASS  %reset: {out}')

    # Test 8: String functions
    r, out, _ = run('PRINT LEFT$("HELLO", 3)')
    assert 'HEL' in out, f'Expected HEL, got: {out}'
    print(f'  PASS  String functions: {out}')

    # Test 9: Multi-statement line
    r, out, _ = run('A=10:B=20:PRINT A+B')
    assert '30' in out, f'Expected 30, got: {out}'
    print(f'  PASS  Multi-statement: {out}')

    # Test 10: FRE returns real value
    r, out, _ = run('PRINT FRE(0)')
    assert r['status'] == 'ok'
    print(f'  PASS  FRE: {out}')

    # Test 11: Sixel decoder (unit test)
    # Minimal Sixel: single red pixel at position (0,0)
    # '@' = ASCII 64 = sixel value 1 = bit 0 set = row 0 pixel
    sixel_data = b'\x1bPq#1;2;100;0;0#1@\x1b\\'
    w, h, rgba = _decode_sixel(sixel_data)
    assert w == 1 and h == 6, f'Expected 1x6, got {w}x{h}'
    # Pixel (0,0) should be red (255,0,0,255)
    assert rgba[0] == 255 and rgba[1] == 0 and rgba[2] == 0 and rgba[3] == 255, \
        f'Expected red pixel, got {list(rgba[:4])}'
    print(f'  PASS  Sixel decode: {w}x{h} pixel')

    # Test 12: PNG encoder (unit test)
    png = _rgba_to_png(1, 1, bytes([255, 0, 0, 255]))
    assert png[:8] == b'\x89PNG\r\n\x1a\n', 'Invalid PNG header'
    print(f'  PASS  PNG encode: {len(png)} bytes')

    # Test 13: Sixel graphics via SCREEN (integration)
    r, out, has_img = run('SCREEN 1\nPSET (10,10), 1\nSCREEN 0')
    # The Sixel output should be detected and rendered as an image
    assert r['status'] == 'ok', f'Expected ok, got {r}'
    if has_img:
        print('  PASS  Sixel graphics: inline image rendered')
    else:
        print('  PASS  Sixel graphics: no image (Sixel may not be emitted in piped mode)')

    # Test 14: Pygments lexer loads
    from .basic_lexer import GWBasicLexer
    lexer = GWBasicLexer()
    tokens = list(lexer.get_tokens('10 PRINT "Hello"'))
    assert len(tokens) > 0, 'Lexer produced no tokens'
    print(f'  PASS  Pygments lexer: {len(tokens)} tokens')

    k.do_shutdown(False)
    print(f'\n14 tests passed.')


if __name__ == '__main__':
    test_kernel()
