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
- **Sixel graphics** — `SCREEN 1`/`SCREEN 2` rendering in compatible terminals
- **Sound** — `SOUND`, `BEEP`, `PLAY` (MML) via PulseAudio
- **Full file I/O** — sequential, random-access, SAVE/LOAD/MERGE/CHAIN/COMMON
- **Printer output** — `LPRINT`/`LLIST` to file or real hardware via `--lpt`
- **54 test programs** with DOSBox-X compatibility testing against real GWBASIC.EXE
- **MIT License**

```{toctree}
:maxdepth: 2

getting-started
language-reference
architecture
development
roadmap
```
