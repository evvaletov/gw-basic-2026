# Development

## Development History

| Version | Commit | Description |
|---------|--------|-------------|
| 0.1.0 | `d8e8375` | Expression calculator with direct mode |
| 0.2.0 | `6162595` | Variables, arrays, program storage, control flow |
| 0.3.0 | `c2d73e9` | File I/O, PRINT USING, SAVE/LOAD, MID$ assignment, graphics stubs |
| 0.4.0 | `df5c308` | CHAIN, RUN "file", random-access I/O (FIELD/PUT/GET), CVI/CVS/CVD/MKI$/MKS$/MKD$ |
| 0.5.0 | `ad21350` | Full-screen TUI editor, KEY statement, Ctrl+Break, Sixel graphics, SOUND/BEEP/PLAY, DOSBox-X compat testing, project rename |
| 0.6.0 | `ece018d` | DATE$/TIME$/TIMER, FILES, SHELL, CHDIR, MKDIR, RMDIR |
| 0.7.0 | `da6b513` | AUTO, RENUM (with GOTO/GOSUB patching), DELETE, COMMON, LIST range fix |
| 0.8.0 | `c68167c` | Dynamic TUI screen buffer, `--full` flag, LPRINT/LLIST with `--lpt` |
| 0.9.0 | `2a8f98b` | EDIT statement, ON TIMER/ON KEY event trapping, F-key escape parser fixes |
| 0.10.0 | | Binary tokenized SAVE/LOAD, INKEY$ extended key sequences, golden-file regression tests, classic BASIC programs |
| 0.11.0 | | DEF SEG / PEEK / POKE virtual memory, GET/PUT graphics sprites, PRINT USING fixes |
| 0.12.0 | | BSAVE/BLOAD, TUI ANSI 16-color rendering, CGA graphics framebuffer PEEK/POKE, keyboard shift flags |
| 0.13.0 | | VIEW/WINDOW/PALETTE graphics, PMAP function, MBF float format for CVS/CVD/MKS$/MKD$ |
| 0.14.0 | | MBF binary file compatibility (IEEE↔MBF at save/load boundary), fix binary loader null-byte truncation, fix DRAW M/S/A bugs, add TA/=var;/X substring |
| 0.15.0 | | Hardware I/O simulator, additional token handlers and device stubs, string pool + compacting GC, Jupyter kernel (Sixel graphics, INPUT, Pygments), ahead-of-time compiler Phase 1 |
| 0.16.0 | | AOT compiler: expanded regression coverage. CHAIN/COMMON/RUN "file" via runtime delegation, token embedding, string comparison, division-by-zero detection, FIELD read-back, graphics, file I/O delegation, RNG matching, dead code elimination |
| 0.17.0 | `981aeab` | Compiler memory safety (`--warn`, `--safe`, `--safe=sanitize`); 16-bit real-mode DOS target (Makefile.dos16, far-heap TUI screen buffer); 32-bit DOS/4GW target (Makefile.dos); FreeDOS 1.4 compatibility |
| 0.18.0 | `89fe0fb` | Cross-language linking: link BASIC objects into a C/Fortran project (`--emit-obj`/`--main-name`) and call C functions from BASIC (`'$EXTERN` FFI pragma); codegen parenthesized-string-comparison fix; `--no-gc-check`/`--fast-math`; larger 32-bit static caps; process-local `DATE$`/`TIME$` assignment |
| 0.18.1 | | Terminal input and resize fixes; active FOR/NEXT and GOSUB control stacks; DOS memory and break-key repairs; checked tokenization; reference corpus and release archive validation |

## Tests

Run the suites from the repository root after building:

```bash
python3 -m pip install -r tests/requirements.txt
python3 tests/test_harnesses.py          # harness failure and timeout checks
python3 tests/test_corpus.py             # corpus harness failure checks
bash tests/run_tests.sh                 # interpreter programs
bash tests/run_compiler_tests.sh        # native executables
python3 tests/test_interactive.py        # terminal input and prompt behavior
python3 tests/test_compiler_regressions.py  # compiler option combinations
bash tests/run_corpus.sh --report corpus-matrix.json  # curated corpus
python3 -m gwbasickernel.test_kernel     # Jupyter kernel
```

The harnesses allow five seconds for ordinary programs, 30 seconds for audio
fixtures, and ten seconds for timer fixtures. Each compiler invocation has a
60-second limit. A process that ignores termination is killed after a further
two seconds. On slower machines, set `GW_TEST_TIMEOUT` for ordinary programs
or `GW_TEST_COMPILE_TIMEOUT` for compilation; both accept positive decimal
seconds. Audio and timer fixtures keep their separate limits.

The interpreter CI step disables audio playback. To run the same check locally,
point PulseAudio at an unavailable socket:

```bash
PULSE_SERVER=unix:/tmp/gwbasic-no-audio-server bash tests/run_tests.sh
```

For a separate check with audio playback, run `bash tests/run_tests.sh` with a
working PulseAudio server and its normal connection settings. The harness checks
execution and text output; sound quality still needs listening.

The interpreter and compiler harnesses compare output with files in
`tests/expected/`, normalizing carriage returns, trailing whitespace, and blank
lines. A mismatch fails the test and prints a unified diff. Execution errors,
timeouts, comparison-tool errors, and suites with no runnable tests also return
a failing status. Tests without an expected-output file are marked `[no expected]`
and check execution only. Inspect timing-dependent and visual behavior separately.
Both harnesses report skipped helper programs; compiler tests also skip interactive,
timing-dependent, and audio programs.

`test_harnesses.py` runs controlled executables in temporary fixture directories.
It checks failure propagation using deliberately wrong output, failed compilation,
timeouts, and a process that ignores termination, as well as normalization and
the documented skips. It does not replace the BASIC regression suites.

The terminal tests run the interpreter under a pseudo-terminal to exercise the
full-screen editor. Compiler regressions compare focused programs across
optimization, safety, and array-access modes. The four interactive games in
`tests/classic/` need a user at the keyboard.

`python3 tests/test_tokenizer.py --sanitize` checks every output-buffer boundary
for representative token widths and rejects oversized lines through the
interpreter, LOAD, and compiler. It builds a small ASan/UBSan test helper.

### Compatibility Testing

The curated corpus has 13 deterministic cases from eight original *BASIC
Computer Games* listings. Each interpreter run and each of six compiler modes
must match a checked-in Microsoft GW-BASIC transcript. Compiled output must also
match the interpreter. Normalization converts CRLF to LF and removes trailing
spaces and tabs; leading spaces and blank lines remain significant.

```bash
# Run one case, or leave off --case to run all 13.
bash tests/run_corpus.sh --case plot --report corpus-matrix.json

# Transpile and compile all 98 listings without executing them.
# Currently exits 1: Super Star Trek has unsupported unspaced TO syntax.
bash tests/run_corpus.sh --audit --report corpus-audit.json
```

The corpus runner uses separate working directories, a five-second execution
limit, 20 seconds for transpilation, and 60 seconds for C compilation. A timeout,
nonzero status, missing build output, runtime stderr, or transcript mismatch fails
the case. It checks source, input, and expected-output hashes before execution.
Its timeout settings are fixed and independent of the shell harness variables.

Reference capture uses a user-supplied Microsoft executable under DOSBox-X.
Only I/O and error diagnostics are adapted; the original vendored files remain
unchanged. The capture does not check terminal echo, screen wrapping, graphics,
or audio. The checked-in transcripts let CI run without either proprietary
software or DOSBox. See the [corpus instructions](https://github.com/evvaletov/gw-basic-2026/tree/main/tests/corpus)
for provenance, capture commands, and the limits of this evidence.


The DOSBox-X harness compares output with Microsoft GWBASIC.EXE:

```bash
# Generate .expected files from GWBASIC.EXE (requires DOSBox-X Flatpak)
bash tests/run_compat.sh --generate

# Compare gwbasic output against .expected
bash tests/run_compat.sh
```

Generating expected files changes the regression references. Review those changes
before using them to assess compatibility.

## CI

GitHub Actions builds the project with PulseAudio support and runs harness,
interpreter, compiler, terminal, compiler-regression, and curated corpus checks
on pushes to `main` and pull requests. The corpus JSON matrix is uploaded as an
artifact, including on failures. See [the workflow](https://github.com/evvaletov/gw-basic-2026/blob/main/.github/workflows/ci.yml)
for the current commands.


## Release preparation

`include/gwbasic.h` sets the runtime version. Keep `CHANGES.TXT`, the README
banner, `pkg/GWBASIC.LSM`, and `RELEASE_NOTES.md` consistent with it. The DOS
builder checks header timestamps as well as source timestamps, so a version
change rebuilds the executable.

After CI passes, its `release-archives` job builds Linux binaries on Ubuntu 22.04
and packages them with the DOS outputs from the same source commit. It tests the
packaged compiler/runtime layout and uploads archives, `BUILD.json`, and
`SHA256SUMS`. The workflow does not tag or publish a release.

To package already-built binaries locally from a clean tracked checkout:

```bash
python3 pkg/build_release.py --dos16 gwbasic16.exe --dos32 gwbasic.exe \
  --output dist/release-local
```

The output directory must be empty or absent. `BUILD.json` records the source
commit, binary hashes, host and libc version. Local packaging checks versions;
it cannot prove that supplied binaries were built from the current source.
CI builds both platforms from the same commit and records its run URL. Use its
Linux archive for the Ubuntu 22.04 baseline.
Review release notes and checksum verification before publication. DOS runtime
checks and isolated audio measurements are local release evidence, separate
from cross-compilation and text-output tests in CI.
