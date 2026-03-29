# GW-BASIC 2026

A portable C reimplementation of Microsoft GW-BASIC, using the
[original 8088 assembly source](https://github.com/microsoft/GW-BASIC)
(released by Microsoft in 2020) as the authoritative reference.

This is not a transpilation — it reimplements the algorithms in clean C11
with modern data structures while targeting bug-compatible behavior.
Unlike the original assembly (43,771 lines across 43 `.ASM` files), this
version is structured as modular C suitable for new feature development.

## Highlights

- **Authentic full-screen editor** — dynamically sized screen buffer (25×80
  default, full terminal with `--full`), free cursor movement, Enter-on-any-line,
  F1-F10 function keys, Insert/Overwrite toggle
- **Binary and ASCII file formats** — `SAVE` writes tokenized binary by default,
  `LOAD` auto-detects format (just like the real thing)
- **INKEY$ extended keys** — arrow keys, F-keys, and navigation keys return proper
  `CHR$(0)` + scan code two-byte sequences
- **Sixel graphics** — `SCREEN 1`/`SCREEN 2` rendering in compatible terminals
- **Sound** — `SOUND`, `BEEP`, `PLAY` (MML) via PulseAudio, plus continuous tone
  via `OUT` (8253 PIT / PPI speaker emulation)
- **Hardware I/O** — `OUT`, `INP`, `WAIT` port emulation (PIT, PPI, CGA, COM1,
  game port) for classic programs that drive hardware directly
- **Full file I/O** — sequential, random-access, SAVE/LOAD/MERGE/CHAIN/COMMON
- **Printer output** — `LPRINT`/`LLIST` to file or real hardware via `--lpt`
- **Classic programs** — Hamurabi, Lunar Lander, Gunner, and Diamond from
  David Ahl's *BASIC Computer Games* (1978) run out of the box
- **Ahead-of-time compiler** — `gwbasic-compile prog.bas -c` produces native
  executables via C codegen + GCC (Phase 1: core statements + expressions)
- **Jupyter kernel** — inline Sixel graphics, INPUT support, Pygments syntax
  highlighting; `pip install -e . && gwbasickernel-install --user`
- **72 test programs** with golden-file regression testing and DOSBox-X
  compatibility testing against real GWBASIC.EXE
- **MIT License**

```{toctree}
:maxdepth: 2

getting-started
language-reference
architecture
development
roadmap
```
