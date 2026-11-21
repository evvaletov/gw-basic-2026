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

### VS Code extension

The separately packaged VSIX provides syntax highlighting, ten snippets, F5 Run,
and native Compile tasks. Commands await saves, use the source directory for
file I/O, and resolve settings in the file's workspace folder. The compiler now
launches GCC without a shell, preserving paths containing spaces and metacharacters.
CI tests the installed VSIX with the real binaries and uploads the package.
See [installation and settings](vscode.md). Debugging and JetBrains remain follow-ups.

### Documentation validation

Pull requests now build Sphinx with warnings as errors and run ten documented
interpreter/compiler/FFI workflows from the Markdown itself. Failure checks
cover paths, missing RUN, ABI mismatches, markers, and timeouts. Pages deployment
remains limited to successful `main` builds. See [local checks](development.md).

### Error recovery and input continuation

Native error recovery now retains source-statement retry and continuation
points, including same-line `RESUME NEXT` and original IF/ELSE behavior.
Original-reference programs exercise native recovery, including WHILE conditions
re-evaluated by WEND: ERL identifies WHILE, while RESUME retries WEND after its
frame has been removed. Sixteen original-console input cases verify the editing
cursor and subsequent PRINT positioning in the editor, file-launched interpreter,
and native POSIX executables. Ordinary terminal runs temporarily manage echo and
restore the caller's terminal settings after input.

## Near-Term Priorities

DOS16 and DOS32 now execute in the CI job using a pinned FreeDOS boot fixture
and disposable QEMU disks. Saved output, input/cursor behavior and Ctrl-C
recovery are required checks (`b3de88c`). ASCII line labels through 65529 work
across the editor, loader and compiler, including high targets and SAVE/RENUM
round trips; expression INTEGER limits remain unchanged (`58ea19c`).

The harness now fails on output mismatches and process errors, with separate
limits for audio, timer, and compilation tests (git-bug `54f3aa6`).

The curated compatibility suite checks 76 scripted cases from thirty-six original
listings against Microsoft GW-BASIC output and across six compiler modes.
Hi-Lo, Bagels, Craps, Rock–Scissors–Paper and Dice include complete random
games or simulations, retries and replay. Nim covers normal/misere endings,
and Tic-Tac-Toe includes both X and O play. Reverse, Combat, Bombardment,
Acey Ducey, Hangman and Bullfight add fourteen complete sessions, covering
solutions, losses, input retries and replay. Another twenty-three cases cover
Chief, Cube, Hammurabi, Depth Charge, Gunner, Train, Bombs Away, Chemist, Boxing
and Even Wins. These include a ten-year Hammurabi term, two five-target Gunner
campaigns, complete Boxing matches and both Even Wins starting positions.
Seventy-two cases end naturally; all 98 vendored listings remain unchanged.
Awari and
Checkers have input checkpoints and expected original-listing failures: FOR
without NEXT at lines 230 and 1340, respectively. These are documented dialect
limitations, not successful complete games. Separately labelled
[Awari and Checkers ports](https://github.com/evvaletov/gw-basic-2026/tree/main/examples/ports)
support full play, input validation, replay and quitting. Their ten separate
reference cases include wins, losses, a draw, multiple jumps, promotion and
a 52-game Awari history session; they do not change the original-corpus counts.
The interpreter and compiler now
share the original RND initial state, sequence, argument behavior and explicit
RANDOMIZE seeding, including the no-argument seed prompt. Full variable names
(up to 40 characters, including periods) and native INPUT$ have independent
reference coverage. The 98-listing build audit has
97 successes and one syntax rejection: Super Star Trek's unspaced `TO`, also
rejected by Microsoft GW-BASIC. Broader gameplay coverage stays open (`51679f0`).

The v0.18.1 fixes cover terminal resize visibility, the 16-bit DOS string pool,
and compiler control flow. Numeric and nested IF/ELSE regressions include
missing-line errors and have Microsoft GW-BASIC reference checks (`b2bb5ca`).
Release archives are prepared by CI with source metadata and checksums.
The macOS workflow builds separate Apple Silicon and Intel archives, checks
their architecture and system-library dependencies, and runs the extracted
interpreter and compiler on macOS 15. Tagged-source builds can add downloads to
an existing release. These archives disable audio and are not notarised.
Audio measurements validate the application stream; physical speaker fidelity
remains unverified.

Version 0.18.2 adds FOR-entry validation, GOSUB-scoped loop
stacks and native execution-order DEFTYPE storage. Conditional and later type
declarations use tagged native expressions and shared variable/array storage;
`$EXTERN` calls work on this path, including inside shared statement handlers.
Ordinary POSIX input handles resize and tiny viewports, displays high bytes as
CP437 glyphs, and restores terminal settings on default termination signals
during input. The documentation runner enforces strict fences, bounds live logs,
and cleans up its process groups after successful and failed commands.

The technical article [Compiling GW-BASIC and drawing in Jupyter](https://valetov.com/articles/gw-basic-2026/)
is public on valetov.com. Its examples use v0.18.2 and gwbasickernel 0.1.0;
downloadable outputs, a notebook and source references accompany it.

The Sixel palette defaults now follow the original SCREEN tables: black, light
cyan, light magenta and high-intensity white in mode 1, and black and
high-intensity white in mode 2.
Interpreter and native regressions cover explicit palette overrides, resets,
mode changes and stored pixel attributes. The correction ships in v0.18.3;
the article retains its v0.18.2 output (git-bug `415598e`).

A focused announcement is posted on SDF's RETRO bboard under `evv`, dated
21 November 2026 (git-bug `682425c`).

Version 0.18.3 provides refreshed Linux, DOS16, DOS32, Apple Silicon and Intel
macOS downloads from the same release commit. Homebrew and the Arch recipe use
the checksum-verified source archive. The original-program corpus remains at
76 cases from 36 listings, with the playable ports tested separately.

Website publication and SDF synchronisation are automated through a persistent
fifteen-minute service. Website updates require a successful Docs build at the
current GitHub main commit; SDF receives main, release tags and git-bug refs.
The service reports failures locally and resumes when its publishing host is
online. See the [publication operations guide](https://github.com/evvaletov/gw-basic-2026/tree/main/publication)
for deployment checks and recovery.

Next priorities:

1. Finish integration and validation of the scripted debugger, then add
   interactive TUI controls for stepping, breakpoints and inspection
   (git-bug `32a637c`).
2. Extend scripted gameplay coverage beyond the 36 original listings already
   covered, keeping adapted ports separate (git-bug `51679f0`).
3. Complete original CGA `COLOR` background, palette/intensity selection and
   redraw behaviour (git-bug `1c9b047`).
4. Enforce the drawing-attribute ranges for SCREEN 1 and SCREEN 2, with
   interpreter/native reference checks (git-bug `4d4aed0`).
5. Complete the FreeDOS repository handoff through the existing maintainer
   thread, using the validated FDNPKG route and any further requested checks
   (git-bug `ab8188c`).

AUR publication remains pending account registration. Homebrew and
`gwbasickernel` on PyPI are live; directory requests await maintainers. HN stays
deferred, and Lobsters needs an invitation and an eligible account. Broader
gameplay coverage remains open for 62 listings. Later implementation work
includes VS Code completion/debugging (`32a637c`), C-style `PRINTF` / `FPRINTF`
(`cd8750c`) and FORTRAN-style formatted `WRITE` (`a6e99af`).

## Longer-Term Plans

Development proposals are tracked in git-bug. Run `git-bug bug show <id>` in the
repository for design notes and current status.

| Theme | Item | git-bug | Priority |
|-------|------|---------|----------|
| testing | Expand scripted gameplay and reference coverage beyond the curated corpus | `51679f0` | P2 |
| compiler | Export individual BASIC routines as C-callable functions | `1b7d59c` | P2 |
| language | FORTRAN-style `WRITE` formatted I/O | `a6e99af` | P2 |
| language | C-style `PRINTF` / `FPRINTF` | `cd8750c` | P2 |
| ide | Debugger/completion and JetBrains support after the packaged VS Code milestone | `32a637c` | P2 |
| stdlib | Numerical/data library with arrays, data frames, and plotting | `55a9d14` | P2 |

The VS Code extension is distributed as a CI artifact; Marketplace publication
is separate follow-up work. Runtime execution is tested on Linux.
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
- Runtime DEFTYPE programs use tagged native operations and shared array lookup;
  fixed-type expression folding and `--inline-arrays` caching do not apply.
  `--fast-math` does not remove checks from tagged arithmetic.
- POSIX terminal display assumes UTF-8 with single-cell CP437 glyphs. BASIC
  strings and editing count bytes; Unicode character/grapheme editing is absent.
  Termination cleanup covers default handlers during input. A host that installs
  its own terminating handler owns cleanup if it bypasses normal process exit.
- Most corpus games lack runtime fixtures; build success is separate evidence.
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
