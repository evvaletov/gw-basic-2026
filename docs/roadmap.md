# Roadmap

## Current Features

### Interpreter and Platform Support

The interpreter includes a full-screen editor, binary and ASCII SAVE/LOAD,
sequential and random-access file I/O, graphics, and sound. Hardware access is
emulated: speaker and CGA ports have implementations, while joystick and serial
interfaces return fixed values. The [language reference](language-reference.md)
describes those behaviors and the remaining device stubs.

The POSIX build uses an ANSI terminal and optional PulseAudio output. OpenWatcom
builds are available for 16-bit real-mode DOS and 32-bit DOS/4GW. The real-mode
build uses the MEDIUM memory model and a far-heap screen buffer; the 32-bit build
requires `DOS4GW.EXE`. See [DOS setup](getting-started.md).

### Ahead-of-Time Compiler

`gwbasic-compile` reads ASCII BASIC, tokenizes it, and emits C source. With `-c`,
GCC builds an executable linked against `libgwrt.a`. Numbered and unnumbered
source files are accepted. Complex statements can delegate to the interpreter
runtime, sharing its file I/O, graphics, and string handling.

Compiler options include:

- `-O0` through `-O3` to select code-generation transformations and GCC optimization
- `--warn` for static diagnostics
- `--safe` for checked integer operations and detailed array/GOSUB diagnostics
- `--safe=sanitize` to instrument generated C with GCC's address and undefined
  behavior sanitizers
- `--inline-arrays` to cache shared runtime storage for eligible numeric arrays
- `--no-gc-check` and `--fast-math` to remove specific checks, changing error and
  runtime behavior
- `--max-output-size` to cap generated C output

Parenthesized string expressions are supported in output, concatenation,
comparison, and `INSTR`. Regression tests cover compiler option combinations;
the test suite is not a claim of complete GW-BASIC compatibility.

### Cross-Language Linking

`--emit-obj` produces an object file, and `--main-name` gives its entry point a
name that a C or Fortran host can call. BASIC calls to C use the `'$EXTERN`
pragma, with INTEGER/SINGLE/DOUBLE/STRING type declarations and optional C-symbol
aliases. String-returning functions can be used in string expressions. The
[linking examples](getting-started.md)
show the declarations and link commands.

Exporting individual BASIC routines as C-callable functions remains deferred.

### Jupyter Kernel

The Python kernel keeps an interpreter subprocess alive between cells. It
supports `INPUT` through Jupyter's stdin protocol, Sixel-to-PNG conversion for
inline graphics, keyword completion, Pygments highlighting, and the `%reset`,
`%timeout`, and `%new` commands.

Install it from the repository root:

```bash
pip install -e .
gwbasickernel-install --user
```

## Near-Term Priorities

1. Make every regression failure fail the harness, including expected-output
   mismatches, and set timeouts that allow audio tests to finish (git-bug
   `54f3aa6`).
2. Build a curated compatibility corpus with fixed input and expected output.
   Compare interpreter and compiled results before expanding the collection.
3. Validate terminal editing, audible output, and execution under DOS, then
   prepare a bugfix release. The 16-bit and 32-bit DOS targets cross-compile;
   that check does not establish runtime behavior (git-bug `e4b2824`).
4. Finish and package the VS Code extension, including installation instructions
   and tests of its editor integration (git-bug `32a637c`).
5. After release validation, prepare Show HN and Lobsters posts and a FreeDOS
   package submission.

## Longer-Term Plans

Development proposals are tracked in git-bug. Run `git-bug bug show <id>` in the
repository for design notes and current status.

| Theme | Item | git-bug | Priority |
|-------|------|---------|----------|
| compiler | Export individual BASIC routines as C-callable functions | `1b7d59c` | P2 |
| language | FORTRAN-style `WRITE` formatted I/O | `a6e99af` | P2 |
| language | C-style `PRINTF` / `FPRINTF` | `cd8750c` | P2 |
| ide | JetBrains support after the VS Code extension | `32a637c` | P2 |
| stdlib | Numerical/data library with arrays, data frames, and plotting | `55a9d14` | P2 |

The VS Code extension and larger *BASIC Computer Games* corpus are development
work outside the supported build and regression suites.

## Known Limitations

- Token handling includes device stubs. `CALL`/`CALLS` machine-code execution
  raises "Illegal function call"; `MOTOR`, `COM`, `IOCTL`, and `LCOPY` do not
  control physical devices.
- Floating-point arithmetic uses IEEE 754. MBF conversion supports binary program
  and file formats, but does not make intermediate arithmetic identical to the
  original implementation.
- Compiled programs do not support timer or key event handlers; `TRON` and
  `TROFF` are ignored by the compiler.
- The compiler reads ASCII source. Save tokenized files with `SAVE "file.bas",A`
  before compiling them.
- `DATE$` and `TIME$` assignments change a process-local clock offset and leave
  the operating system clock unchanged.
- The runtime has one global interpreter state. Generated entry points cannot
  run concurrently or recursively in a host process.
- POSIX and 32-bit DOS builds cap interpreter storage at 1024 variables and 256
  arrays, with FOR/GOSUB/WHILE nesting limits of 64/128/64. The 16-bit DOS limits
  are 256 variables, 64 arrays, and nesting depths of 16/24/16 to fit its 64KB
  data segment.
