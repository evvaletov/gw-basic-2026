# Focused GW-BASIC semantics

`semantics.json` contains 54 programs and output captured from Microsoft
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
used a revised capture script. RESUME cases with late DEFTYPE declarations
exercise the interpreter execution path; they do not establish native
same-line RESUME NEXT support.
