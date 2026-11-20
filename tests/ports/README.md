# Ported-game tests

This suite exercises the separately labelled [playable ports](../../examples/ports/README.md).
The unchanged-original corpus remains at 76 cases from 36 listings. Every run
of this suite also verifies the hashes of all 98 original listings.

```sh
python3 tests/run_corpus.py --suite ports --report ports-matrix.json
```

The default run compares the interpreter and all six compiler modes against
the reference transcript. Linux CI runs this matrix; macOS CI runs the
interpreter and O2 native mode on Apple Silicon and Intel.

| Case | Coverage |
| --- | --- |
| `awari-win` | Complete player win, captures and bonus moves |
| `awari-loss` | Complete computer win, captures and bonus moves |
| `awari-draw` | Complete drawn game |
| `awari-retry` | Invalid, fractional and empty pits; invalid replay choice; complete win |
| `awari-history` | 52 complete games, filling, rotating and reusing the 50-entry learned history |
| `checkers-win` | Complete player win, promotion on both sides and multiple jumps |
| `checkers-loss` | Complete computer win, computer multiple jump and blocked human player |
| `checkers-retry-replay` | Complete win and loss with replay; invalid coordinates, backward moves, friendly/empty jumps, invalid continuation and piece reselection |
| `awari-quit` | Explicit quit; not a completed game |
| `checkers-quit` | Piece reselection and explicit quit; not a completed game |

The eight complete-game cases contain 60 finished games, including the long
history session. The two quit cases are marked in the table. These are
tests of the documented historical variants, including optional captures in
Checkers, rather than a claim of tournament-rule compliance.

## Reference captures

The transcripts were captured on 20 November 2026 from the same pinned Microsoft
assembly-source/JWasm build used by the original corpus, under DOSBox-X. This
executable is not retail GW-BASIC 3.23 and is not distributed here.
`reference.json` records executable, adapter, port source, input, transformed
source and transcript hashes. Expected output is never generated from the
implementation under test.

To capture again into a fresh directory:

```sh
python3 tests/corpus_reference.py --suite ports \
  --reference-exe /path/to/GWBASIC.EXE --output /tmp/gwbasic-ports-reference
```

The existing I/O adapter redirects prompts, input and printed output and
requires each program to reach `END`. The game calculations and control flow
come from the port files. File capture does not establish terminal wrapping,
echo, graphics or sound fidelity. Review a new capture before replacing a
checked-in transcript or its metadata.
