"""Jupyter kernel for GW-BASIC 2026.

Persistent process model: a long-running gwbasic subprocess reads BASIC
commands from stdin and writes output to stdout.  The kernel sends each
notebook cell as one or more lines of BASIC, followed by a sentinel
PRINT that emits a known delimiter.  Output is captured until the
delimiter appears, then forwarded to the notebook.

GW-BASIC's piped mode suppresses the banner and "Ok" prompt automatically,
and stdout is unbuffered, making it ideal for subprocess communication.

Features beyond basic execution:
  - Inline Sixel graphics: ESC P ... ESC \\ sequences are extracted from
    the output stream, decoded into PNG images, and displayed inline.
  - INPUT statement support: when gwbasic prints "? " (the INPUT prompt),
    the kernel requests input from the notebook front-end via the Jupyter
    stdin protocol.
  - Pygments lexer: GW-BASIC syntax highlighting in notebooks.
  - Tab completion for all GW-BASIC keywords.
"""

import base64
import io
import os
import re
import select
import shutil
import signal
import struct
import subprocess
import zlib

from ipykernel.kernelbase import Kernel

from . import __version__

_SENTINEL = '\x01GWDONE\x01'
_SENTINEL_CMD = 'PRINT CHR$(1)+"GWDONE"+CHR$(1)\n'

_ERROR_RE = re.compile(
    r'(?:Syntax error|Illegal function call|Type mismatch|Out of |'
    r'Division by zero|Overflow|RETURN without GOSUB|'
    r'NEXT without FOR|Undefined |Subscript out of range|'
    r'Redo from start|String too long|'
    r'WHILE without WEND|WEND without WHILE|'
    r'File not found|File already open|Bad file |'
    r'Direct statement in file|Missing operand|'
    r'Duplicate Definition|Formula too complex|'
    r'Internal error|Unprintable error)',
    re.IGNORECASE,
)

# DCS (ESC P) starts Sixel, ST (ESC \) ends it — both as bytes
_DCS = b'\x1bP'
_ST = b'\x1b\\'

GW_KEYWORDS = [
    'AUTO', 'BEEP', 'BLOAD', 'BSAVE', 'CALL', 'CHAIN', 'CHDIR',
    'CIRCLE', 'CLEAR', 'CLOSE', 'CLS', 'COLOR', 'COM', 'COMMON',
    'CONT', 'DATA', 'DATE$', 'DEF', 'DEFDBL', 'DEFINT', 'DEFSNG',
    'DEFSTR', 'DELETE', 'DIM', 'DRAW', 'EDIT', 'ELSE', 'END',
    'ENVIRON', 'ENVIRON$', 'ERASE', 'ERDEV', 'ERDEV$', 'ERR', 'ERL',
    'ERROR', 'FIELD', 'FILES', 'FN', 'FOR', 'GET', 'GOSUB', 'GOTO',
    'IF', 'INPUT', 'IOCTL', 'IOCTL$', 'KEY', 'KILL', 'LCOPY',
    'LET', 'LINE', 'LIST', 'LLIST', 'LOAD', 'LOCATE', 'LPRINT',
    'LSET', 'MERGE', 'MKDIR', 'MOTOR', 'NAME', 'NEW', 'NEXT',
    'NOISE', 'ON', 'OPEN', 'OPTION', 'OUT', 'PAINT', 'PALETTE',
    'PLAY', 'POKE', 'PRESET', 'PRINT', 'PSET', 'PUT', 'RANDOMIZE',
    'READ', 'REM', 'RENUM', 'RESET', 'RESTORE', 'RESUME', 'RETURN',
    'RMDIR', 'RSET', 'RUN', 'SAVE', 'SCREEN', 'SHELL', 'SOUND',
    'STOP', 'SWAP', 'SYSTEM', 'THEN', 'TIME$', 'TIMER', 'TO',
    'TRON', 'TROFF', 'VIEW', 'WAIT', 'WEND', 'WHILE', 'WIDTH',
    'WINDOW', 'WRITE',
    'ABS', 'ASC', 'ATN', 'CDBL', 'CHR$', 'CINT', 'COS', 'CSNG',
    'CSRLIN', 'CVD', 'CVI', 'CVS', 'EOF', 'EXP', 'FIX', 'FRE',
    'HEX$', 'INKEY$', 'INP', 'INPUT$', 'INSTR', 'INT', 'LEFT$',
    'LEN', 'LOC', 'LOF', 'LOG', 'LPOS', 'MID$', 'MKD$', 'MKI$',
    'MKS$', 'OCT$', 'PEEK', 'PEN', 'PMAP', 'POINT', 'POS',
    'RIGHT$', 'RND', 'SGN', 'SIN', 'SPACE$', 'SPC', 'SQR',
    'STICK', 'STR$', 'STRIG', 'STRING$', 'TAB', 'TAN', 'USR',
    'VAL', 'VARPTR', 'VARPTR$',
    'AND', 'EQV', 'IMP', 'MOD', 'NOT', 'OR', 'XOR', 'STEP',
]


# ---------- Sixel decoder (pure Python, no PIL dependency) ----------

def _parse_sixel_palette(data):
    """Parse #n;2;r;g;b palette definitions from Sixel data."""
    palette = {}
    for m in re.finditer(rb'#(\d+);2;(\d+);(\d+);(\d+)', data):
        idx = int(m.group(1))
        r = int(m.group(2)) * 255 // 100
        g = int(m.group(3)) * 255 // 100
        b = int(m.group(4)) * 255 // 100
        palette[idx] = (r, g, b)
    return palette


def _decode_sixel(data):
    """Decode Sixel data into an RGBA image. Returns (width, height, rgba_bytes)."""
    # Strip DCS/ST wrappers if present
    if data.startswith(_DCS):
        data = data[len(_DCS):]
    if data.endswith(_ST):
        data = data[:-len(_ST)]

    # Skip mode char (usually 'q')
    if data and data[0:1] == b'q':
        data = data[1:]

    palette = _parse_sixel_palette(data)
    if not palette:
        palette = {0: (0, 0, 0)}

    # First pass: determine image dimensions
    x = 0
    y = 0
    max_x = 0
    max_y = 0
    color = 0
    i = 0
    while i < len(data):
        ch = data[i]
        if ch == ord('#'):
            # Color selector or palette def — skip palette defs
            j = i + 1
            num = b''
            while j < len(data) and (data[j] in b'0123456789'):
                num += bytes([data[j]])
                j += 1
            if j < len(data) and data[j] == ord(';'):
                # Palette definition — skip to end
                while j < len(data) and data[j] not in b'#$-\x1b' and not (63 <= data[j] <= 126):
                    j += 1
                i = j
                continue
            color = int(num) if num else 0
            i = j
        elif ch == ord('$'):
            x = 0
            i += 1
        elif ch == ord('-'):
            x = 0
            y += 6
            i += 1
        elif ch == ord('!'):
            # RLE: !count<char>
            j = i + 1
            num = b''
            while j < len(data) and data[j] in b'0123456789':
                num += bytes([data[j]])
                j += 1
            count = int(num) if num else 1
            if j < len(data) and 63 <= data[j] <= 126:
                x += count
                j += 1
            max_x = max(max_x, x)
            max_y = max(max_y, y + 6)
            i = j
        elif 63 <= ch <= 126:
            x += 1
            max_x = max(max_x, x)
            max_y = max(max_y, y + 6)
            i += 1
        else:
            i += 1

    if max_x == 0 or max_y == 0:
        return 0, 0, b''

    width, height = max_x, max_y
    # RGBA buffer (transparent background)
    pixels = bytearray(width * height * 4)

    # Second pass: render pixels
    x = 0
    y = 0
    color = 0
    i = 0
    while i < len(data):
        ch = data[i]
        if ch == ord('#'):
            j = i + 1
            num = b''
            while j < len(data) and data[j] in b'0123456789':
                num += bytes([data[j]])
                j += 1
            if j < len(data) and data[j] == ord(';'):
                while j < len(data) and data[j] not in b'#$-\x1b' and not (63 <= data[j] <= 126):
                    j += 1
                i = j
                continue
            color = int(num) if num else 0
            i = j
        elif ch == ord('$'):
            x = 0
            i += 1
        elif ch == ord('-'):
            x = 0
            y += 6
            i += 1
        elif ch == ord('!'):
            j = i + 1
            num = b''
            while j < len(data) and data[j] in b'0123456789':
                num += bytes([data[j]])
                j += 1
            count = int(num) if num else 1
            if j < len(data) and 63 <= data[j] <= 126:
                sixel = data[j] - 63
                j += 1
                r, g, b = palette.get(color, (0, 0, 0))
                for dx in range(count):
                    cx = x + dx
                    if cx >= width:
                        break
                    for bit in range(6):
                        if sixel & (1 << bit):
                            cy = y + bit
                            if cy < height:
                                off = (cy * width + cx) * 4
                                pixels[off] = r
                                pixels[off + 1] = g
                                pixels[off + 2] = b
                                pixels[off + 3] = 255
                x += count
            i = j
        elif 63 <= ch <= 126:
            sixel = ch - 63
            r, g, b = palette.get(color, (0, 0, 0))
            if x < width:
                for bit in range(6):
                    if sixel & (1 << bit):
                        cy = y + bit
                        if cy < height:
                            off = (cy * width + x) * 4
                            pixels[off] = r
                            pixels[off + 1] = g
                            pixels[off + 2] = b
                            pixels[off + 3] = 255
            x += 1
            i += 1
        else:
            i += 1

    return width, height, bytes(pixels)


def _rgba_to_png(width, height, rgba):
    """Encode raw RGBA pixels as PNG (pure Python, no dependencies)."""
    def _chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    raw = b''
    stride = width * 4
    for y in range(height):
        raw += b'\x00' + rgba[y * stride:(y + 1) * stride]
    idat = zlib.compress(raw)

    out = b'\x89PNG\r\n\x1a\n'
    out += _chunk(b'IHDR', ihdr)
    out += _chunk(b'IDAT', idat)
    out += _chunk(b'IEND', b'')
    return out


# ---------- Kernel class ----------

class GWBasicKernel(Kernel):
    implementation = 'gwbasickernel'
    implementation_version = __version__
    language = 'basic'
    language_version = '2026'
    language_info = {
        'name': 'basic',
        'mimetype': 'text/x-basic',
        'file_extension': '.bas',
        'codemirror_mode': 'vb',
        'pygments_lexer': 'gwbasickernel.basic_lexer.GWBasicLexer',
    }
    banner = 'GW-BASIC 2026 Jupyter Kernel'

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._proc = None
        self._timeout = int(os.environ.get('GWBASIC_TIMEOUT', '30'))

    def _find_binary(self):
        env = os.environ.get('GWBASIC')
        if env and os.path.isfile(env) and os.access(env, os.X_OK):
            return env
        for candidate in ['gwbasic', './build/gwbasic', './gwbasic']:
            found = shutil.which(candidate)
            if found:
                return found
        return None

    def _start_process(self):
        self._kill_process()
        binary = self._find_binary()
        if not binary:
            return False
        self._proc = subprocess.Popen(
            [binary],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        return True

    def _kill_process(self):
        if self._proc is not None:
            try:
                self._proc.kill()
                self._proc.wait(timeout=5)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                pass
            self._proc = None

    def _is_alive(self):
        return self._proc is not None and self._proc.poll() is None

    def _send_interrupt_children(self):
        if self._is_alive():
            self._proc.send_signal(signal.SIGINT)
        else:
            super()._send_interrupt_children()

    def _read_until_sentinel(self, allow_stdin=False):
        """Read raw bytes from stdout until sentinel or timeout.

        When allow_stdin is True and the output ends with "? " (INPUT prompt),
        the kernel requests input from the notebook front-end and writes the
        response to the subprocess's stdin.

        Returns (raw_bytes, status).
        """
        fd = self._proc.stdout.fileno()
        buf = b''
        sentinel_bytes = _SENTINEL.encode()

        while True:
            try:
                ready, _, _ = select.select([fd], [], [], self._timeout)
            except (KeyboardInterrupt, InterruptedError):
                if self._is_alive():
                    self._proc.send_signal(signal.SIGINT)
                return buf, 'interrupted'

            if not ready:
                # Check for INPUT prompt before declaring timeout
                if allow_stdin and buf.endswith(b'? '):
                    reply = self.raw_input('? ')
                    self._proc.stdin.write((reply + '\n').encode())
                    self._proc.stdin.flush()
                    continue
                return buf, 'timeout'

            chunk = os.read(fd, 8192)
            if not chunk:
                return buf, 'died'

            buf += chunk

            # Check for sentinel in the buffer
            if sentinel_bytes in buf:
                idx = buf.index(sentinel_bytes)
                # Keep everything before the sentinel, stripping the
                # PRINT output that contains it (newline + sentinel + newline)
                before = buf[:idx]
                # Remove trailing newline left by PRINT before sentinel
                if before.endswith(b'\n'):
                    before = before[:-1]
                return before, 'done'

            # Check for INPUT prompt: "? " at end of buffer (no newline after)
            if allow_stdin and buf.endswith(b'? '):
                last_nl = buf.rfind(b'\n')
                tail = buf[last_nl + 1:] if last_nl >= 0 else buf
                if b'\n' not in tail[:-2]:  # no embedded newline before "? "
                    prompt_line = tail.decode('utf-8', errors='replace')
                    reply = self.raw_input(prompt_line)
                    self._proc.stdin.write((reply + '\n').encode())
                    self._proc.stdin.flush()

    def _split_sixel(self, raw):
        """Split raw output into (text_parts, sixel_blobs) interleaved."""
        parts = []
        pos = 0
        while True:
            dcs_idx = raw.find(_DCS, pos)
            if dcs_idx == -1:
                parts.append(('text', raw[pos:]))
                break
            if dcs_idx > pos:
                parts.append(('text', raw[pos:dcs_idx]))
            st_idx = raw.find(_ST, dcs_idx)
            if st_idx == -1:
                parts.append(('text', raw[pos:]))
                break
            end = st_idx + len(_ST)
            parts.append(('sixel', raw[dcs_idx:end]))
            pos = end
        return parts

    def _has_error(self, text):
        return bool(_ERROR_RE.search(text))

    def _handle_magic(self, code):
        stripped = code.strip()
        if stripped == '%reset':
            self._kill_process()
            return True, 'Session reset.'
        if stripped.startswith('%timeout'):
            parts = stripped.split()
            if len(parts) == 2:
                try:
                    self._timeout = int(parts[1])
                    return True, f'Timeout set to {self._timeout}s.'
                except ValueError:
                    return True, 'Usage: %timeout <seconds>'
            return True, f'Current timeout: {self._timeout}s. Usage: %timeout <seconds>'
        if stripped == '%new':
            if self._is_alive():
                self._proc.stdin.write(b'NEW\n')
                self._proc.stdin.write(_SENTINEL_CMD.encode())
                self._proc.stdin.flush()
                self._read_until_sentinel()
            return True, 'Program cleared.'
        return False, None

    def do_execute(self, code, silent, store_history=True,
                   user_expressions=None, allow_stdin=False):
        handled, response = self._handle_magic(code)
        if handled:
            if not silent and response:
                self.send_response(self.iopub_socket, 'stream',
                                   {'name': 'stdout', 'text': response + '\n'})
            return {
                'status': 'ok', 'execution_count': self.execution_count,
                'payload': [], 'user_expressions': {},
            }

        if not code.strip():
            return {
                'status': 'ok', 'execution_count': self.execution_count,
                'payload': [], 'user_expressions': {},
            }

        if not self._is_alive():
            if not self._start_process():
                if not silent:
                    self.send_response(self.iopub_socket, 'stream', {
                        'name': 'stderr',
                        'text': 'gwbasic binary not found.\n'
                                'Set GWBASIC env var or install gwbasic on PATH.\n',
                    })
                return {
                    'status': 'error', 'execution_count': self.execution_count,
                    'ename': 'FileNotFoundError', 'evalue': 'gwbasic not found',
                    'traceback': [],
                }

        for line in code.split('\n'):
            line = line.rstrip()
            if line:
                self._proc.stdin.write((line + '\n').encode())

        self._proc.stdin.write(_SENTINEL_CMD.encode())
        self._proc.stdin.flush()

        raw, status = self._read_until_sentinel(allow_stdin=allow_stdin)

        if status == 'interrupted':
            if not silent:
                self.send_response(self.iopub_socket, 'stream', {
                    'name': 'stderr', 'text': 'Execution interrupted.\n',
                })
            return {'status': 'abort', 'execution_count': self.execution_count}

        if status == 'timeout':
            self._kill_process()
            if not silent:
                self.send_response(self.iopub_socket, 'stream', {
                    'name': 'stderr',
                    'text': f'Execution timed out after {self._timeout}s. '
                            f'Use %timeout to increase.\n',
                })
            return {
                'status': 'error', 'execution_count': self.execution_count,
                'ename': 'TimeoutError',
                'evalue': f'Timed out after {self._timeout}s',
                'traceback': [],
            }

        if status == 'died':
            self._proc = None
            text = raw.decode('utf-8', errors='replace').strip()
            if not silent and text:
                self.send_response(self.iopub_socket, 'stream', {
                    'name': 'stderr',
                    'text': f'GW-BASIC process exited.\n{text}\n',
                })
            return {
                'status': 'error', 'execution_count': self.execution_count,
                'ename': 'RuntimeError', 'evalue': 'Process exited',
                'traceback': [],
            }

        # Split output into text and Sixel graphics
        parts = self._split_sixel(raw)

        is_error = False
        for kind, data in parts:
            if kind == 'text':
                text = data.decode('utf-8', errors='replace').strip()
                if not text:
                    continue
                if self._has_error(text):
                    is_error = True
                if not silent:
                    self.send_response(self.iopub_socket, 'stream', {
                        'name': 'stderr' if is_error else 'stdout',
                        'text': text + '\n',
                    })
            elif kind == 'sixel' and not silent:
                w, h, rgba = _decode_sixel(data)
                if w > 0 and h > 0:
                    png = _rgba_to_png(w, h, rgba)
                    b64 = base64.b64encode(png).decode('ascii')
                    self.send_response(self.iopub_socket, 'display_data', {
                        'data': {'image/png': b64},
                        'metadata': {'image/png': {'width': w, 'height': h}},
                    })

        if is_error:
            err_text = raw.decode('utf-8', errors='replace').strip()
            return {
                'status': 'error', 'execution_count': self.execution_count,
                'ename': 'BASICError', 'evalue': err_text.split('\n')[0],
                'traceback': err_text.split('\n'),
            }

        return {
            'status': 'ok', 'execution_count': self.execution_count,
            'payload': [], 'user_expressions': {},
        }

    def do_complete(self, code, cursor_pos):
        text = code[:cursor_pos]
        match = re.search(r'([A-Za-z_]\w*\$?)$', text)
        if not match:
            return {'matches': [], 'cursor_start': cursor_pos,
                    'cursor_end': cursor_pos, 'status': 'ok'}

        token = match.group(1).upper()
        start = cursor_pos - len(token)
        matches = [kw for kw in GW_KEYWORDS if kw.startswith(token)]
        return {
            'matches': matches, 'cursor_start': start,
            'cursor_end': cursor_pos, 'status': 'ok',
        }

    def do_is_complete(self, code):
        if not code.strip():
            return {'status': 'incomplete', 'indent': ''}
        return {'status': 'complete'}

    def do_shutdown(self, restart):
        self._kill_process()
        return {'status': 'ok', 'restart': restart}
