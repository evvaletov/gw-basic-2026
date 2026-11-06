# BASIC Computer Games compatibility corpus

This directory contains 98 unmodified BASIC listings from
[coding-horror/basic-computer-games](https://github.com/coding-horror/basic-computer-games/tree/a81c331c1d32f9553c6226639a415bfceb2daed6),
pinned at `a81c331c1d32f9553c6226639a415bfceb2daed6`. `sources.json` records each
upstream path, Git blob ID, and SHA-256. All 98 files were checked against that
revision. The upstream [Unlicense](LICENSE) applies to the vendored listings.

## Runtime cases

Run from the repository root after building:

```bash
bash tests/run_corpus.sh --report corpus-matrix.json
bash tests/run_corpus.sh --case plot --mode O2
python3 tests/test_corpus.py
```

`fixtures.json` selects 13 cases from eight listings. Inputs are fixed; no case
uses random output or the current date. These paths finish naturally with `END`.
The listings themselves are not shortened or patched.

| Listing | Cases | Main coverage |
|---------|-------|---------------|
| Name | yes/no answers | String arrays, sorting, negative FOR step |
| Love | short/long text | DATA, string arrays, layout |
| Bunny | complete picture | Auto-DIM, GOSUB, CHR$ |
| Diamond | sizes 5 and 21 | Nested loops, backward TAB, MID$ |
| 3D Plot | complete plot | Compact DEF FN, SQR, EXP, layout |
| Weekday | same date, Friday the 13th, pre-1582 date | Multiple INPUT fields, FN, branches |
| Calendar | complete 1979 calendar | Arrays, ON GOTO, DATA |
| Literature Quiz | four wrong answers | INPUT, zero-score branch |

Every case runs in the interpreter and in six compiled modes: `-O0`, `-O1`,
`-O2`, `-O3`, `-O2 --inline-arrays`, and `-O2 --safe`. Each transcript must match
its independent reference, and compiled output must match interpreter output.
CRLF becomes LF and trailing spaces/tabs are removed. Leading spaces, blank
lines, and the final newline are significant.

Execution is limited to five seconds, transpilation to 20 seconds, and native
compilation to 60 seconds, with two seconds to terminate a stalled process.
Child output files are capped at 8 MiB and generated C at 1 MiB. A timeout is a
failure. Failed transpilation cannot pass by leaving a partial C file. Runtime
stderr, nonzero exit status, missing output, and mismatched hashes also fail.
Each run uses its own temporary working directory. Set `CC` to choose the native
C compiler. Python 3 and a POSIX host are required.

## Reference capture

`expected/` was captured on 2026-11-05 from a local Microsoft `GWBASIC.EXE` under
DOSBox-X. `reference.json` records executable, adapter, source, input, transformed
source, and transcript hashes. The Microsoft executable is not distributed here.

To reproduce into a new directory using your own executable:

```bash
python3 tests/corpus_reference.py \
  --reference-exe /path/to/GWBASIC.EXE \
  --output /tmp/gwbasic-reference-new
```

This requires the `com.dosbox_x.DOSBox-X` Flatpak. Each capture gets an isolated
DOS drive. `PRINT` writes to file #9, `INPUT` reads the scripted file #8 after
writing its prompt, and `END` writes a completion marker, closes files, and
exits. An error handler records unexpected GW-BASIC errors. The adapter preserves
calculations and control flow and supports only these curated listings. It
retains transformed source, raw output, logs, and normalized transcripts for
review. The marker is required and removed; DOS Ctrl-Z sector padding is ignored.
The command never overwrites an existing directory or the checked-in references.

Review the adapted source and capture before copying transcripts and metadata
into this directory. Do not regenerate expected output from the implementation
being tested. Hash checks detect accidental edits; they do not establish oracle
correctness by themselves.

File redirection does not reproduce terminal echo, screen width/wrapping,
graphics, or sound. These cases do not establish complete gameplay coverage,
identical floating-point arithmetic, or compatibility of the other 90 listings.

## Full build audit and remaining gaps

```bash
bash tests/run_corpus.sh --audit --report corpus-audit.json
```

The audit transpiles and compiles every vendored listing at `-O0`; it does not
execute them. On 2026-11-05, 97 of 98 compiled. `build-audit.json` preserves the
baseline with temporary paths removed. The command returns failure for either
transpilation or native compilation errors, so the current audit exits 1.

| Listing | Finding |
|---------|---------|
| Awari and Checkers | Native compilation fixed with runtime FOR/NEXT tracking; gameplay remains unverified |
| Super Star Trek | Compiler rejects unspaced `TO` at line 4060; Microsoft GW-BASIC also rejects this syntax |
| Sinewave | Builds, but Microsoft GW-BASIC reports syntax error at line 40 (`REMARKABLE`); excluded from runtime fixtures |

The FOR/NEXT native compilation failures (`ca3a8b8`) are resolved. Broader
scripted gameplay, random-seed handling, and dialect triage remain under `51679f0`.
Nonempty FOR entry validation and conditional DEFTYPE behavior still differ
from Microsoft GW-BASIC in some cases. Successful builds
are not counted as runtime passes. GitHub CI runs the curated suite and uploads
its JSON matrix; the full build audit is an explicit development command.
