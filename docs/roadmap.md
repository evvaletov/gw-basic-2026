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

The harness now fails on output mismatches and process errors, with separate
limits for audio, timer, and compilation tests (git-bug `54f3aa6`).

The curated compatibility suite checks 23 scripted cases from twelve original
listings against Microsoft GW-BASIC output and across six compiler modes.
Awari and Checkers now compile (`ca3a8b8`). The 98-listing build audit has
97 successes and one syntax rejection: Super Star Trek's unspaced `TO`, also
rejected by Microsoft GW-BASIC. Broader gameplay coverage stays open (`51679f0`).

The v0.18.1 fixes cover terminal resize visibility, the 16-bit DOS string pool,
and compiler control flow. Numeric and nested IF/ELSE regressions include
missing-line errors and have Microsoft GW-BASIC reference checks (`b2bb5ca`).
Release archives are prepared by CI with source metadata and checksums.
Audio measurements validate the application stream; physical speaker fidelity
remains unverified.

Development since v0.18.1 adds FOR-entry validation, GOSUB-scoped loop
stacks and execution-order DEFTYPE storage. Programs with conditional or later
type declarations use interpreter execution inside the compiled executable;
entry-prefix declarations retain native compilation. Gameplay fixtures now
include complete Tic-Tac-Toe and Towers games, Chomp, and One Check sessions.

Next priorities:

1. Finish and package the VS Code extension, including installation instructions
   and tests of its editor integration (git-bug `32a637c`).
2. Prepare Show HN and Lobsters posts and a FreeDOS package submission using the
   published release and clearly identified development changes
   (git-bug `dc24012`, `ab8188c`).
3. Check Sphinx warnings and executable documentation examples on pull requests,
   before the existing documentation deployment on `main` (git-bug `f3a24bc`).
4. Automate FreeDOS runtime smoke tests for both DOS builds in CI. The existing
   CI cross-builds do not exercise keyboard, cursor, or saved-output behavior
   (git-bug `b3de88c`).
5. Extend gameplay coverage to random games and triage Awari/Checkers runtime
   paths against original GW-BASIC (git-bug `51679f0`).

## Longer-Term Plans

Development proposals are tracked in git-bug. Run `git-bug bug show <id>` in the
repository for design notes and current status.

| Theme | Item | git-bug | Priority |
|-------|------|---------|----------|
| testing | Expand scripted gameplay and reference coverage beyond the curated corpus | `51679f0` | P2 |
| compiler | Native runtime DEFTYPE storage and FFI support | `990569c` | P2 |
| compiler | Export individual BASIC routines as C-callable functions | `1b7d59c` | P2 |
| language | Accept ASCII interpreter line labels above 32767 | `58ea19c` | P2 |
| language | FORTRAN-style `WRITE` formatted I/O | `a6e99af` | P2 |
| language | C-style `PRINTF` / `FPRINTF` | `cd8750c` | P2 |
| ide | JetBrains support after the VS Code extension | `32a637c` | P2 |
| stdlib | Numerical/data library with arrays, data frames, and plotting | `55a9d14` | P2 |

The VS Code extension remains development work outside the supported build.
The corpus runtime checks cover only the cases listed in its fixture manifest.

## Known Limitations

- Token handling includes device stubs. `CALL`/`CALLS` machine-code execution
  raises "Illegal function call"; `MOTOR`, `COM`, `IOCTL`, and `LCOPY` do not
  control physical devices.
- Floating-point arithmetic uses IEEE 754. MBF conversion supports binary program
  and file formats, but does not make intermediate arithmetic identical to the
  original implementation.
- Compiled programs do not support timer or key event handlers; `TRON` and
  `TROFF` are ignored by the compiler.
- Programs with runtime DEFTYPE changes use interpreter execution in the
  compiled executable, so native expression/array optimizations do not apply.
  Combining this path with `$EXTERN` calls is rejected with a diagnostic.
- Most corpus games lack runtime fixtures; build success is separate evidence.
- The interpreter rejects ASCII line labels above 32767; full GW-BASIC line
  numbering through 65529 is tracked in `58ea19c`.
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
