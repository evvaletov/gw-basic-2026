"""Jupyter kernel for GW-BASIC 2026.

Persistent process model: a long-running gwbasic subprocess reads BASIC
commands from stdin and writes output to stdout.  The kernel sends each
notebook cell as one or more lines of BASIC, followed by a sentinel
PRINT that emits a known delimiter.  Output is captured until the
delimiter appears, then forwarded to the notebook.

GW-BASIC's piped mode suppresses the banner and "Ok" prompt automatically,
and stdout is unbuffered, making it ideal for subprocess communication.
"""

import os
import re
import select
import shutil
import signal
import subprocess

from ipykernel.kernelbase import Kernel

from . import __version__

_SENTINEL = '<<<GWDONE>>>'
_SENTINEL_CMD = f'PRINT "{_SENTINEL}"\n'

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

    def _read_until_sentinel(self):
        """Read stdout until the sentinel line or timeout.

        Returns (output_lines, status) where status is one of
        'done', 'timeout', 'died', or 'interrupted'.
        """
        lines = []
        fd = self._proc.stdout.fileno()
        buf = b''

        while True:
            try:
                ready, _, _ = select.select([fd], [], [], self._timeout)
            except (KeyboardInterrupt, InterruptedError):
                if self._is_alive():
                    self._proc.send_signal(signal.SIGINT)
                return lines, 'interrupted'

            if not ready:
                return lines, 'timeout'

            chunk = os.read(fd, 8192)
            if not chunk:
                return lines, 'died'

            buf += chunk
            while b'\n' in buf:
                line_bytes, buf = buf.split(b'\n', 1)
                line = line_bytes.decode('utf-8', errors='replace').rstrip('\r')
                if _SENTINEL in line:
                    return lines, 'done'
                lines.append(line)

    def _has_error(self, lines):
        for line in lines:
            if _ERROR_RE.search(line):
                return True
        return False

    def _handle_magic(self, code):
        """Handle %magic commands. Returns (handled, response)."""
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
        # Magic commands
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

        # Start process if needed
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

        # Send each line of the cell
        for line in code.split('\n'):
            line = line.rstrip()
            if line:
                self._proc.stdin.write((line + '\n').encode())

        # Send sentinel to delimit output
        self._proc.stdin.write(_SENTINEL_CMD.encode())
        self._proc.stdin.flush()

        # Read output
        output_lines, status = self._read_until_sentinel()

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
            output = '\n'.join(output_lines)
            if not silent and output:
                self.send_response(self.iopub_socket, 'stream', {
                    'name': 'stderr',
                    'text': f'GW-BASIC process exited.\n{output}\n',
                })
            return {
                'status': 'error', 'execution_count': self.execution_count,
                'ename': 'RuntimeError', 'evalue': 'Process exited',
                'traceback': [],
            }

        # Filter empty leading/trailing lines
        while output_lines and not output_lines[0].strip():
            output_lines.pop(0)
        while output_lines and not output_lines[-1].strip():
            output_lines.pop()

        is_error = self._has_error(output_lines)
        output = '\n'.join(output_lines)

        if not silent and output:
            self.send_response(self.iopub_socket, 'stream', {
                'name': 'stderr' if is_error else 'stdout',
                'text': output + '\n',
            })

        if is_error:
            return {
                'status': 'error', 'execution_count': self.execution_count,
                'ename': 'BASICError', 'evalue': output.split('\n')[0],
                'traceback': output.split('\n'),
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
