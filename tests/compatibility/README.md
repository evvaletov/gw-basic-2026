# Focused GW-BASIC semantics

`semantics.json` contains 97 programs and output captured from Microsoft
GW-BASIC source build (JWasm, 2023-10-03) on 2026-11-06 and 2026-11-07.
This is the pinned source build, not a claim of retail 3.23 identity. It covers FOR-entry evaluation and errors, lexical NEXT
matching, GOSUB loop scope, executed DEFTYPE changes, typed arrays, and integer
rounding, RESUME, WHILE/WEND error recovery and loop scope, RND argument
behavior, and invalid variable-name whitespace. The executable, adapter, sources, inputs, adapted programs, and output
hashes record capture provenance. The proprietary executable is not distributed.

The ordinary regression command runs these cases together with the existing
compiler tests, in the interpreter and 16 compiler option combinations:

```bash
python3 tests/test_compiler_regressions.py
```

To reproduce the independent references in a fresh directory:

```bash
python3 tests/corpus_reference.py --suite semantics \
  --reference-exe /path/to/GWBASIC.EXE --output /tmp/gw-semantics-new
```

The adapter redirects I/O and adds completion/error markers. It leaves control
flow and expressions intact. See [the corpus capture notes](../corpus/README.md)
for its I/O assumptions and limits. Expected output must come from the reference
executable, not from the implementation under test.

Correction, 2026-11-06: four earlier tests accepted FOR programs whose lexical
NEXT was missing or mismatched. The original executable rejects the affected
FOR before entering its loop body. The updated `shared_next`,
`conditional_for_positive`, `next_discards_nested`, and `bare_next_reentry`
cases preserve that evidence. `conditional_for_negative` remains a successful
control: its loop prints 3, 2, and 1. Earlier notes that counted all five as
rejected paths were incorrect.

A case-level adapter hash overrides the manifest default when a later capture
used a revised capture script. The 20 cases marked `native` exercise RESUME
continuations, retries, RESUME 0, handler state, and IF/ELSE boundaries. The
regression runner verifies that these use native generated code in every
compiler mode. Earlier RESUME cases with late DEFTYPE declarations still
exercise the interpreter execution path.

`input-console.json` contains 16 independent FreeDOS/QEMU captures using real
Microsoft GW-BASIC console input. It records source, typed answers, editing
keys, cursor positions before/after TAB and comma output, saved answer strings,
and relevant screen rows. These cover ordinary and leading-semicolon INPUT
and LINE INPUT, prompts, empty/numeric answers, cursor editing, wrapping, and
scrolling. Console I/O is unchanged; the programs save their own measurements
to files, and the screen rows come from BIOS text memory. Run the same sources
and keys under the supplied reference executable to reproduce them.

`python3 tests/test_interactive.py` compares the real PTY editor against these
console references. `python3 tests/test_terminal_input.py` runs all sixteen in
the file-launched interpreter and native O0/safe executables on an echoing PTY,
and checks termios restoration, Ctrl-C/EOF, typeahead, fragmented escape keys,
capacity limits, and unchanged pipe/file input. File-I/O adaptation is unsuitable
for cursor behavior.

The twenty-three November 7 semantic cases also require native generated code in
all sixteen compiler modes. They distinguish initial WHILE failures, WEND
re-evaluation, body errors, bare RESUME, RESUME NEXT and explicit reentry, nested
frames, FOR cleanup and GOSUB boundaries; RND cases check startup, zero and
negative arguments, explicit RANDOMIZE (including repeated seeds, signed
endpoints and integer rounding), trailing WEND comments and POS argument
side effects, malformed WEND tails (true/false/recovery), division evaluation
order and negative RND underflow. WEND removes its loop frame before evaluating the WHILE
condition. An error reports WHILE's line, but retrying that WEND raises error 30.

`random-games.json` records the independent captures used to update the existing
Monte Carlo, binary-search guessing game and interactive issue-6 answer tests
after correcting explicit RANDOMIZE. Their previous expectations used the old
generator. END is appended only to close the two no-END reference captures.
