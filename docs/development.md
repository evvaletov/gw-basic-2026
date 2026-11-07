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
python3 tests/test_compiler_paths.py        # literal GCC paths and launch failures
python3 tests/test_line_numbers.py          # labels, loading, editing and SAVE/RENUM
python3 tests/test_dos_runtime.py           # DOS harness failure handling
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

### DOS runtime checks

The DOS cross-build job executes both binaries under FreeDOS in QEMU before
uploading them for release packaging. Run the same checks locally after building
both targets:

```bash
./build_dos.sh 16
./build_dos.sh 32
python3 tests/run_dos_runtime.py --dos16 gwbasic16.exe --dos32 gwbasic.exe \
  --dos4gw "$HOME/openwatcom-v2/binw/dos4gw.exe" --output build/dos-runtime
```

Install Python 3, `qemu-system-x86` (with PC floppy support), and `mtools`.
`--qemu PATH` selects a different QEMU executable. The runner uses TCG software
emulation, 32 MiB guest RAM, a private boot floppy and a disposable 64 MiB FAT
disk for each target. It needs neither root, kernel disk devices nor KVM.
The default deadline is 120 seconds per target; CI bounds the whole DOS build
job to 15 minutes. Temporary guest disks are removed and QEMU is stopped on
success or failure.

The first run downloads the 23 MiB
[FreeDOS 1.4 Floppy Edition](https://www.freedos.org/download/) from the project's
ibiblio mirror. Every run verifies the archive against the pinned SHA256 from
[upstream verify.txt](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/verify.txt)
and verifies the boot image member separately. `--cache DIR` selects the download
cache; a corrupt cache fails rather than being accepted. FreeDOS sources and
licenses are available through the upstream download page; no FreeDOS or
Microsoft binaries are committed here. `DOS4GW.EXE` comes from the same
OpenWatcom installation used to cross-build the interpreter.

Checks cover arithmetic, strings, loops, file I/O, high line labels, nested
IF/error recovery, editor input against an original-console reference, and
Ctrl-C recovery from a compute loop. Saved output and cursor positions must
match expectations. The high-line/error fixture also matches an unmodified
run in a pinned Microsoft GW-BASIC source build (JWasm, 2023-10-03);
`tests/compatibility/dos-runtime-reference.json`
records its source and capture hashes. Missing tools or binaries return status 2 (unavailable);
guest failures, timeouts, corrupt fixtures and output mismatches return status 1.
Neither is a successful skip. `report.json`, text-screen captures, saved output,
and QEMU logs are retained in the output directory and uploaded even on CI
failure. The shell entry point `tests/run_freedos_qemu_auto.sh` forwards to this
runner; it no longer attaches local disk images or uses sudo.

This smoke suite does not establish physical graphics/audio fidelity or complete
DOS/gameplay compatibility. The manual `tests/run_freedos_qemu.sh` checklist and
the larger local semantic matrices remain useful release checks.

### Compatibility Testing

The curated corpus has 39 scripted cases from twenty original *BASIC
Computer Games* listings. Each interpreter run and each of six compiler modes
must match a checked-in Microsoft GW-BASIC transcript. Compiled output must also
match the interpreter. Normalization converts CRLF to LF and removes trailing
spaces and tabs; leading spaces and blank lines remain significant.

```bash
# Run one case, or leave off --case to run all 39.
bash tests/run_corpus.sh --case plot --report corpus-matrix.json

# Transpile and compile all 98 listings without executing them.
# Currently exits 1: Super Star Trek has unsupported unspaced TO syntax.
bash tests/run_corpus.sh --audit --report corpus-audit.json
```

The corpus runner uses separate working directories, a five-second execution
limit, 20 seconds for transpilation, and 60 seconds for C compilation. A timeout,
nonzero status, missing build output, runtime stderr, or transcript mismatch fails
the case. It checks source, input, and expected-output hashes before execution.
Two fixtures end at explicit input checkpoints, keeping stdin open until the
recorded prompt; they are marked as checkpoints, not completed games. Two
others compare original-listing ERR/ERL through a diagnostic wrapper. Expected
errors and checkpoint definitions are bound to the reference metadata. A
timeout never counts as reaching a checkpoint.
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


The `vscode-extension` job separately builds the runtime, tests the editor
commands and TextMate grammar, packages a VSIX, then installs that package into
an isolated VS Code profile for integration tests. The successful package is
uploaded as the `vscode-extension-stable` artifact. See [extension development](vscode.md)
for local commands. Native compiler launch tests cover literal filenames, custom
runtime/C-output paths, object output, sanitizer arguments, and GCC failures.

### Documentation checks

The Docs workflow runs on every pull request to `main`, every push to `main`,
and manual dispatch. This includes README-only changes, edits to the included
VS Code README, and runtime changes that could break an example. Sphinx runs
with `-W --keep-going -n`, so warnings and unresolved references fail the job.

The build job has read-only repository permissions and does not retain checkout
credentials. Pages upload and deployment run only on `main` push/manual events,
after all documentation checks pass. Only the deployment job has Pages/OIDC
write permissions. Each pull request has its own concurrency group.

After building the runtime, run the same checks locally:

```bash
python3 -m pip install -r docs/requirements.txt
python3 -m sphinx -W --keep-going -n -b html docs docs/_build/html
python3 tests/test_doc_examples.py
python3 tests/run_doc_examples.py --report docs/_build/examples.json
```

The full example suite requires Bash, GCC, GFortran, and PulseAudio development
libraries for the documented link commands. CI installs these dependencies.
Use `--build /path/to/build` for an alternate runtime build or `--case guide-pipe`
to select a workflow. The failure-detection tests use the repository's `build/`.

The runner reads 18 marked code blocks directly from `README.md` and
`docs/getting-started.md`. Ten workflows exercise program-file and piped input,
C emission, native and safety-mode compilation, BASIC called from C/Fortran,
and C functions called from BASIC. Each workflow gets a fresh temporary copy of
the binaries, headers, and fixtures. Output values/text are compared after
whitespace splitting; terminal layout is tested by the separate PTY suite.
The documented C drivers/providers are also compiled alongside generated
prototypes, catching ABI mismatches that separate-object linking can miss.

A marker such as `<!-- docs-test: guide-pipe -->` immediately precedes the
fenced example. IDs, languages, and expected outputs are registered in
`tests/run_doc_examples.py`. To add coverage, add a marker and an example entry
there. Missing, duplicate, malformed, or unused IDs fail the check. The runner
executes the marked Bash blocks exactly, with strict error/pipeline handling;
installation commands and interactive transcripts are outside this smoke suite.
These are ordinary commands in a temporary directory, not a security sandbox.

The JSON report records snippet locations/hashes and per-workflow results,
including failures, and CI uploads it as `documentation-example-results`.
Commands have timeouts and timeout cleanup kills their process groups.
`test_doc_examples.py` deliberately breaks paths, RUN, pipelines, C ABIs,
Fortran symbols, and markers, and checks timeout cleanup.

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
Review release notes and checksum verification before publication. CI now runs
the DOS smoke suite described above. Broader DOS compatibility matrices and
isolated audio measurements remain local release evidence.
