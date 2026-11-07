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

`fixtures.json` selects 32 cases from seventeen listings. Inputs are fixed.
Random games use the original default RND sequence in a fresh process; no case
uses the current date or RANDOMIZE. Twenty-eight cases finish naturally with
`END`, two stop at documented input checkpoints, and two reproduce errors in
the original listings. The vendored listing bytes remain unchanged.

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
| Tic-Tac-Toe 2 | loss, draw, invalid moves | Board arrays, nested branches, complete games |
| Towers | two/three disks, invalid-move recovery | FOR/GOSUB scope, complete solutions, TAB after INPUT |
| Chomp | normal game, rules and invalid inputs | Nested arrays, player rotation, input validation |
| One Check | jumps and rejection, replay | Board updates, replay state, input validation |
| Hi-Lo | two wins with replay, six-guess loss | Default RND sequence, high/low feedback, accumulated winnings |
| Bagels | retries and two wins, twenty-guess loss | Distinct random digits, PICO/FERMI feedback, answer reveal |
| Craps | loss followed by replay win | RND(0), dice sequence, running wager balance |
| Awari | three invalid inputs; first legal move | Input checkpoint; original error 26 at line 230 |
| Checkers | opening with invalid input; computer capture | Input checkpoint; original error 26 at line 1340 |

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
Checkpoint fixtures deliberately terminate the owned child only after the
recorded prompt occurrence; the report labels that status `checkpoint`. Stdin
stays open, so EOF cannot silently advance a game. Early exit, a missing prompt,
a timeout and wrong output all fail. Output queued through termination is also
compared. Checkpoints assert this bounded transcript, not a blocked read syscall
or behavior beyond the final prompt. Their scripts are capped at 4 KiB so writing
stdin cannot block before the execution deadline. Expected-error fixtures add only an ON ERROR
diagnostic wrapper to both execution paths; they compare the reference ERR/ERL
and preceding transcript. An error fixture is not a completed game.
Each run uses its own temporary working directory. Set `CC` to choose the native
C compiler. Python 3 and a POSIX host are required.

## Reference capture

`expected/` contains captures from 2026-11-06 and 2026-11-07 using a local
Microsoft GW-BASIC source build (JWasm, 2023-10-03) under DOSBox-X. The pinned
executable is not retail 3.23; these captures do not establish retail identity. `reference.json` records executable, adapter, source, input, transformed
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
exits. After INPUT, the adapter reopens the capture file in append mode to reset
its output column, matching the console's new column after Enter without
capturing terminal echo. An error handler records GW-BASIC errors; an expected
error must match the manifest's exact ERR/ERL pair. For checkpoint captures, an
I/O-only helper counts INPUT calls and exits at the next prompt after the fixed
input budget. It uses reserved counter C9 and rejects conflicting listings.
The adapter preserves
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
identical floating-point arithmetic, or compatibility of the other 81 listings.

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
| Awari | First legal move raises FOR without NEXT at line 230 in the original reference. Its forward FOR has only a backward NEXT. Invalid-move checkpoint and failure transcript are checked. |
| Checkers | A playable opening is checked; the first computer capture reaches original FOR without NEXT at line 1340. Conditional FOR/alternate NEXT paths need a separately labeled dialect port for full play. |
| Super Star Trek | Compiler rejects unspaced `TO` at line 4060; Microsoft GW-BASIC also rejects this syntax |
| Sinewave | Builds, but Microsoft GW-BASIC reports syntax error at line 40 (`REMARKABLE`); excluded from runtime fixtures |

The FOR/NEXT native compilation failures (`ca3a8b8`) are resolved. Broader
scripted gameplay, random-seed handling, and dialect triage remain under `51679f0`.
FOR entry validation and GOSUB loop scope now have focused original-GW-BASIC
references in `../compatibility/semantics.json`. That suite also checks DEFTYPE
storage changes and integer rounding. Successful builds are not counted as
runtime passes. GitHub CI runs the curated suite and uploads
its JSON matrix; the full build audit is an explicit development command.
