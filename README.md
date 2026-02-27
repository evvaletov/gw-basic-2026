# GW-BASIC 2026

A portable C reimplementation of Microsoft GW-BASIC, using the
[original 8088 assembly source](https://github.com/microsoft/GW-BASIC)
(released by Microsoft in 2020) as the authoritative reference.

This is not a transpilation — it reimplements the algorithms in clean C11
with modern data structures while targeting bug-compatible behavior.

## Building

```bash
mkdir -p build && cd build
cmake .. && make
```

Requires a C11 compiler and CMake 3.10+. PulseAudio (`libpulse-simple`)
is optional — detected at build time for `SOUND`/`BEEP`/`PLAY` support.

## Usage

Interactive mode launches the authentic GW-BASIC full-screen editor:

```
$ ./gwbasic
GW-BASIC 2026 0.9.0
(C) Eremey Valetov 2026. MIT License.
Based on Microsoft GW-BASIC assembly source.
Ok
PRINT 2+2
 4
Ok
FOR I=1 TO 5:PRINT I;:NEXT
 1  2  3  4  5
Ok
```

Run a program file:

```bash
./gwbasic tests/programs/prime_sieve.bas
```

Pipe input:

```bash
echo '10 FOR I=1 TO 10:PRINT I*I;:NEXT' | ./gwbasic
```

## What Works

**Data types:** INTEGER (%), SINGLE (!), DOUBLE (#), STRING ($)

**Operators:** `+ - * / ^ \ MOD AND OR XOR EQV IMP NOT < = > <= >= <>`

**Numeric functions:** SGN, INT, ABS, SQR, SIN, COS, TAN, ATN, LOG, EXP,
RND, FIX, CINT, CSNG, CDBL

**String functions:** LEN, ASC, CHR$, VAL, STR$, LEFT$, RIGHT$, MID$,
SPACE$, STRING$, HEX$, OCT$, INSTR, INPUT$

**Statements:**

| Category | Statements |
|----------|------------|
| Output | PRINT, LPRINT, LLIST, PRINT USING, WRITE, CLS |
| Variables | LET, DIM, ERASE, SWAP, DEFINT/SNG/DBL/STR |
| Control flow | GOTO, GOSUB/RETURN, FOR/NEXT, IF/THEN/ELSE, WHILE/WEND, ON...GOTO/GOSUB |
| Input | INPUT, LINE INPUT, DATA/READ/RESTORE, INKEY$ |
| Program | RUN, RUN "file", CONT, STOP, END, NEW, LIST, CLEAR, AUTO, RENUM, DELETE, EDIT |
| Sequential I/O | OPEN, CLOSE, PRINT#, WRITE#, INPUT#, LINE INPUT# |
| Random-access I/O | FIELD, LSET, RSET, PUT, GET, CVI/CVS/CVD, MKI$/MKS$/MKD$ |
| Program I/O | SAVE, LOAD, MERGE, CHAIN, COMMON |
| Event trapping | ON TIMER(n) GOSUB, TIMER ON/OFF/STOP, ON KEY(n) GOSUB, KEY(n) ON/OFF/STOP |
| Error handling | ON ERROR GOTO, RESUME, ERROR, ERR, ERL |
| User functions | DEF FN, RANDOMIZE |
| File management | KILL, NAME, FILES, MKDIR, RMDIR, CHDIR, SHELL |
| Date/time | DATE$, TIME$, TIMER |
| Screen | LOCATE, COLOR, WIDTH, SCREEN, KEY ON/OFF/LIST |
| Graphics | PSET, PRESET, LINE, CIRCLE, DRAW, PAINT |
| Sound | SOUND, BEEP, PLAY (MML) |

### Full-Screen Editor (TUI)

When running interactively, GW-BASIC 2026 presents the authentic full-screen
editor that people remember from the 1980s:

- 25×80 screen buffer with free cursor movement (arrow keys)
- **Enter on any screen line** re-enters it as BASIC input
- Insert/Overwrite toggle (Insert key, cursor shape changes)
- Function key bar on line 25 (`KEY ON`/`KEY OFF`/`KEY LIST`)
- Default F1-F10 bindings (F1=LIST, F2=RUN, F3=LOAD", etc.)
- Ctrl+C interrupts running programs
- Piped input bypasses the TUI entirely — scripts and test harnesses work unchanged
- `--full` flag adapts to the full terminal size instead of the classic 25×80

### Printer Output (LPRINT/LLIST)

`LPRINT` and `LLIST` send output to a printer device or file:

- **Modern systems (default):** output is appended to `LPT1.TXT` in the current directory
- **Real hardware:** use `--lpt /dev/lp0` (Linux) or `--lpt LPT1` (FreeDOS) to send output to a physical parallel port printer

```bash
./gwbasic --lpt /dev/lp0 myprogram.bas   # print to hardware
./gwbasic --lpt report.txt myprogram.bas  # print to file
./gwbasic myprogram.bas                   # default: LPT1.TXT
```

### Graphics

Graphics mode is activated with `SCREEN 1` (320×200, 4 colors) or
`SCREEN 2` (640×200, monochrome). Drawing commands render to a virtual
framebuffer and output via [Sixel graphics](https://en.wikipedia.org/wiki/Sixel),
which works in terminals like xterm, mlterm, foot, and WezTerm.

```
SCREEN 1
LINE (0,0)-(319,199), 1
CIRCLE (160,100), 80, 2
PAINT (160,100), 3, 2
```

## Architecture

The interpreter follows the original GW-BASIC's internal structure:

```
Source text → Tokenizer (CRUNCH) → Token stream
                                      ↓
                              Expression evaluator (FRMEVL)
                                      ↓
                              Statement dispatcher (NEWSTT)
                                      ↓
                              TUI screen buffer (interactive)
                                      ↓
                              HAL (platform I/O)
```

| Module | File | Original |
|--------|------|----------|
| Tokenizer | tokenizer.c | GWMAIN.ASM |
| Evaluator | eval.c | GWEVAL.ASM |
| Interpreter | interp.c | BINTRP.ASM |
| TUI editor | tui.c | — |
| Graphics | graphics.c | — |
| Tokens | tokens.c | IBMRES.ASM |
| Errors | error.c | GWDATA.ASM |
| Math | math_int.c, math_float.c, math_transcend.c | MATH1/2.ASM |
| Strings | strings.c | BISTRS.ASM |
| File I/O | fileio.c | BIPTRG.ASM |
| PRINT USING | print_using.c | BIPRTU.ASM |
| Sound | sound.c | — |
| Platform | hal_posix.c | OEM*.ASM |

Key design differences from the original:
- IEEE 754 floating point (MBF conversion only for CVI/CVS/CVD compatibility)
- Dynamic memory allocation instead of 64KB segment layout
- malloc'd strings instead of compacting garbage collector
- setjmp/longjmp for error recovery

## Tests

56 test programs in `tests/programs/`, with CI via GitHub Actions:

```bash
bash tests/run_tests.sh
```

Compatibility testing against real GWBASIC.EXE under DOSBox-X:

```bash
bash tests/run_compat.sh --generate   # generate .expected from GWBASIC.EXE
bash tests/run_compat.sh              # compare gwbasic output against .expected
```

## License

MIT License. See [LICENSE](LICENSE).
