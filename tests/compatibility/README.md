# Focused GW-BASIC semantics

`semantics.json` contains 74 programs and output captured from Microsoft
GW-BASIC on 2026-11-06. It covers FOR-entry evaluation and errors, lexical NEXT
matching, GOSUB loop scope, executed DEFTYPE changes, typed arrays, and integer
rounding, RESUME, and invalid variable-name whitespace. The executable, adapter, sources, inputs, adapted programs, and output
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
console references. File-I/O adaptation is unsuitable for this behavior.
