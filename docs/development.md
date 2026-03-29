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
| 0.15.0 | | Hardware I/O simulator, gap-fill (100% token coverage), string pool + compacting GC, Jupyter kernel |

## Tests

72 test programs in `tests/programs/`, plus 4 classic interactive programs in
`tests/classic/` (Hamurabi, Lunar Lander, Gunner, Diamond from David Ahl's
*BASIC Computer Games*).

Run the full automated suite:

```bash
bash tests/run_tests.sh
```

Each test has a 5-second timeout. 67 tests have `.expected` golden files
for output regression detection. Tests without golden comparison: datetime,
on_timer, timer_stop (timing-dependent), color_test (visual), play_music
(audio duration).

### Compatibility Testing

Compare output against real GWBASIC.EXE running under DOSBox-X:

```bash
# Generate .expected files from GWBASIC.EXE (requires DOSBox-X Flatpak)
bash tests/run_compat.sh --generate

# Compare gwbasic output against .expected
bash tests/run_compat.sh
```

## CI

GitHub Actions runs on every push to `main` and on pull requests. The workflow
builds the project with PulseAudio support and runs all 66 test programs.

See [`.github/workflows/ci.yml`](https://github.com/evvaletov/gw-basic-2026/blob/main/.github/workflows/ci.yml).
