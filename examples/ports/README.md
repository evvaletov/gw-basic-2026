# Playable GW-BASIC ports

These are adaptations of the BASIC Computer Games Awari and Checkers listings.
They have their own banners, controls and gameplay tests. The unchanged
originals and their compatibility failures remain in
[`tests/corpus`](../../tests/corpus/README.md).

From the repository root:

```sh
build/gwbasic examples/ports/awari.bas
build/gwbasic examples/ports/checkers.bas
```

Both can also be compiled. For example:

```sh
build/gwbasic-compile examples/ports/checkers.bas -c --runtime .
examples/ports/checkers
```

## Awari

Choose a pit from 1 to 6, counting left to right along the bottom row. Enter
`0` to quit. After a game, enter `1` to play again or `0` to finish.

This keeps the listing's rules: seeds pass through both stores; landing in an
empty non-store pit captures the opposite pit's seeds when it is occupied.
Each turn allows at most one bonus move. A game ends when either row is empty;
only seeds already in the stores determine the score. Remaining seeds are not
swept into a store. The computer uses the original move heuristic and remembers
up to 50 games it did not win, discarding the oldest record when that fills.

The port replaces the backward `NEXT` path with a forward loop ending, rejects
fractional and out-of-range pit numbers, and adds explicit replay and quit
controls. The bounded history prevents an array overrun during repeated play.

## Checkers

You play `O`, moving towards row 7; the computer plays `X` and moves first.
Coordinates are `column,row`, with `(0,0)` at the bottom left. The board shows
both axes. Kings are marked `O*` and `X*` and can move in either direction.

| Prompt | Input |
| --- | --- |
| `FROM` | Your piece's coordinates; `-1,-1` quits |
| `TO` | A diagonal step or jump; `-1,-1` selects another piece |
| `+TO` | Another jump by the same piece; `-1,-1` finishes the turn |
| `PLAY AGAIN` | `1` starts a new game; `0` finishes |

As in the listing, captures and further jumps are optional. A man moves and
captures forwards; a king can also move and capture backwards. Crowning ends
the turn. A player loses when no pieces or legal moves remain. There is no
automatic repetition or move-limit draw rule; use the quit control to leave
a drawn position.

The port separates the computer's man and king capture loops, removes the
source and captured pieces before crowning, and checks human coordinates before
indexing the board. It rejects backward man moves and jumps over empty or
friendly squares. It also detects a blocked human player and resets the board
when replaying. The computer's original scoring heuristic is retained.

## Source and validation

Both files derive from the
[pinned BASIC Computer Games revision](https://github.com/coding-horror/basic-computer-games/tree/a81c331c1d32f9553c6226639a415bfceb2daed6).
Their originals are `04_Awari/awari.bas` and `23_Checkers/checkers.bas`; the
vendored copies and hashes are recorded in
[`sources.json`](../../tests/corpus/sources.json). These adaptations retain the
[Unlicense](LICENSE).

[`tests/ports`](../../tests/ports/README.md) holds separate Microsoft GW-BASIC
reference captures for the adapted programs, including complete games, replay,
input rejection and quitting. Those results do not turn the original listings'
known failures into compatibility passes.
