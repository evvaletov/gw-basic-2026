# GW-BASIC 2026

A portable C reimplementation of Microsoft GW-BASIC, using the
[original 8088 assembly source](https://github.com/microsoft/GW-BASIC)
(released by Microsoft in 2020) as the authoritative reference.

The interpreter implements the language in C11 and aims to reproduce the
original behavior. Hardware access uses emulation or device stubs; see the
[language reference](docs/language-reference.md) for their limits.

## Building

```bash
cmake -B build && cmake --build build
```

The POSIX build requires a C11 compiler, CMake 3.12+, and `pkg-config`.
PulseAudio (`libpulse-simple`) is optional and detected at build time for
`SOUND`/`BEEP`/`PLAY` audio output.

Builds three targets:

- `gwbasic` -- the interpreter
- `gwbasic-compile` -- the ahead-of-time compiler
- `libgwrt.a` -- runtime library for compiled programs

## Usage

Run these commands from the repository root. Interactive mode launches a
GW-BASIC-style full-screen editor:

```
$ build/gwbasic
GW-BASIC 2026 0.18.0
(C) Eremey Valetov 2026. MIT License.
Based on Microsoft GW-BASIC assembly source.
Ok
PRINT 2+2
 4
Ok
```

Run a program file (ASCII or binary tokenized):

```bash
build/gwbasic tests/programs/prime_sieve.bas
```

Compile to a native executable:

```bash
build/gwbasic-compile tests/programs/fibonacci.bas -c --runtime .
```

## Ahead-of-Time Compiler

`gwbasic-compile` translates ASCII BASIC programs to C source. With `-c`,
it invokes GCC to produce native executables linked against `libgwrt.a`.
GCC is required for this path even if the interpreter was built with Clang.

Pipeline: `.bas` → tokenizer → analysis → C codegen → `gcc` → native binary.

The compiler test harness compares eligible programs with the expected-output
files. It skips interactive, timing-dependent, and audio tests, plus helper
programs loaded by other tests. Passing that suite does not establish full
GW-BASIC compatibility; compiled timer and key event handlers are unsupported.

```bash
# Generate C source
build/gwbasic-compile myprog.bas -o myprog.c

# Compile to executable (automatic)
build/gwbasic-compile myprog.bas -c --runtime .
```

## Jupyter Kernel

A Jupyter notebook kernel for GW-BASIC with inline Sixel graphics
rendering, INPUT support, and Pygments syntax highlighting.

```bash
pip install -e .
gwbasickernel-install --user
jupyter notebook  # select "GW-BASIC 2026" kernel
```

## What Works

The interpreter covers the language features below. Device-specific operations
include emulated ports, fixed return values, and unsupported machine-code calls.

Data types: INTEGER (%), SINGLE (!), DOUBLE (#), STRING ($)

Operators: `+ - * / ^ \ MOD AND OR XOR EQV IMP NOT < = > <= >= <>`

Statements:

| Category | Statements |
|----------|------------|
| Output | PRINT, LPRINT, LLIST, PRINT USING, WRITE, CLS |
| Variables | LET, DIM, ERASE, SWAP, DEFINT/SNG/DBL/STR |
| Control flow | GOTO, GOSUB/RETURN, FOR/NEXT, IF/THEN/ELSE, WHILE/WEND, ON...GOTO/GOSUB |
| Input | INPUT, LINE INPUT, DATA/READ/RESTORE, INKEY$ |
| Program | RUN, CONT, STOP, END, NEW, LIST, CLEAR, AUTO, RENUM, DELETE, EDIT |
| Sequential I/O | OPEN, CLOSE, PRINT#, WRITE#, INPUT#, LINE INPUT# |
| Random-access I/O | FIELD, LSET, RSET, PUT, GET, CVI/CVS/CVD, MKI$/MKS$/MKD$ |
| Program I/O | SAVE (binary/ASCII), LOAD (auto-detects), MERGE, CHAIN, COMMON |
| Event trapping | ON TIMER(n) GOSUB, TIMER ON/OFF/STOP, ON KEY(n) GOSUB |
| Error handling | ON ERROR GOTO, RESUME, ERROR, ERR, ERL |
| User functions | DEF FN, RANDOMIZE |
| File management | KILL, NAME, FILES, MKDIR, RMDIR, CHDIR, SHELL, ENVIRON |
| Date/time | DATE$, TIME$, TIMER |
| Screen | LOCATE, COLOR, WIDTH, SCREEN, KEY ON/OFF/LIST |
| Graphics | PSET, PRESET, LINE, CIRCLE, DRAW, PAINT, GET/PUT (sprites), VIEW, WINDOW, PALETTE, PMAP |
| Sound | SOUND, BEEP, PLAY (MML) |
| Memory | DEF SEG, PEEK, POKE, BSAVE, BLOAD |
| Hardware I/O | OUT, INP, WAIT, MOTOR, STICK, STRIG |

## Tests

Run the regression suites from the repository root:

```bash
python3 tests/test_harnesses.py          # harness failure and timeout checks
bash tests/run_tests.sh                 # interpreter
bash tests/run_compiler_tests.sh        # compiled programs
bash tests/run_corpus.sh --report corpus-matrix.json  # curated compatibility cases
python3 -m gwbasickernel.test_kernel     # Jupyter kernel
```

For an interpreter run without audio playback:

```bash
PULSE_SERVER=unix:/tmp/gwbasic-no-audio-server bash tests/run_tests.sh
```

This points PulseAudio at an unavailable socket, as the interpreter CI step does.
Audio fixtures have a 30-second limit so they can also finish with a live server.
Output mismatches, execution failures, and timeouts fail the suites. See
[Development](docs/development.md) for timeout settings and test coverage.

The [compatibility corpus](tests/corpus/README.md) contains 98 original BASIC
Computer Games listings. Its automated runtime suite checks 13 fixed-input cases
from eight listings against Microsoft GW-BASIC transcripts and compares six
compiler modes with the interpreter. The other listings have build-audit
coverage only; Awari and Checkers currently fail native compilation.

## DOS / FreeDOS

Cross-compiles to DOS with OpenWatcom V2:

```bash
wmake -f Makefile.dos16   # 16-bit real mode, no extender
wmake -f Makefile.dos     # 32-bit DOS/4GW, requires DOS4GW.EXE
```

The 16-bit build runs on FreeDOS, MS-DOS, and compatible systems without
a DOS extender.  See [Getting Started](docs/getting-started.md) for details.

## Documentation

Full Sphinx documentation in `docs/`:

```bash
cd docs && pip install -r requirements.txt && make html
```

## License

The implementation uses the [MIT License](LICENSE). The vendored BASIC Computer
Games listings in `tests/corpus/` use the upstream [Unlicense](tests/corpus/LICENSE).
