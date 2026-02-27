# Language Reference

## Data Types

| Type | Suffix | Description |
|------|--------|-------------|
| INTEGER | `%` | 16-bit signed |
| SINGLE | `!` | 32-bit float |
| DOUBLE | `#` | 64-bit float |
| STRING | `$` | Up to 255 bytes |

## Operators

`+`, `-`, `*`, `/`, `^`, `\` (integer div), `MOD`, `AND`, `OR`, `XOR`, `EQV`,
`IMP`, `NOT`, `<`, `=`, `>`, `<=`, `>=`, `<>`

## Numeric Functions

`SGN`, `INT`, `ABS`, `SQR`, `SIN`, `COS`, `TAN`, `ATN`, `LOG`, `EXP`, `RND`,
`FIX`, `CINT`, `CSNG`, `CDBL`

`RND` can be called with or without parentheses: `RND` is equivalent to `RND(1)`.

## String Functions

`LEN`, `ASC`, `CHR$`, `VAL`, `STR$`, `LEFT$`, `RIGHT$`, `MID$`, `SPACE$`,
`STRING$`, `HEX$`, `OCT$`, `INSTR`, `INPUT$`

## File Functions

`EOF`, `LOC`, `LOF`

## Pseudo-variables

`ERL`, `ERR`, `CSRLIN`, `INKEY$`, `DATE$`, `TIME$`, `TIMER`, `POS(0)`

## Literals

Decimal, `&H` hex, `&O` octal, `D` exponent (double), `E` exponent (single),
type suffixes (`%`, `!`, `#`)

## Statements

| Category | Statements |
|----------|------------|
| Output | `PRINT`, `LPRINT`, `LLIST`, `PRINT USING`, `WRITE`, `CLS` |
| Variables | `LET`, `DIM`, `ERASE`, `SWAP`, `DEFINT`, `DEFSNG`, `DEFDBL`, `DEFSTR` |
| Control flow | `GOTO`, `GOSUB`/`RETURN`, `FOR`/`NEXT`, `IF`/`THEN`/`ELSE`, `WHILE`/`WEND`, `ON...GOTO`, `ON...GOSUB` |
| Input | `INPUT`, `LINE INPUT`, `DATA`/`READ`/`RESTORE`, `INKEY$` |
| Program control | `RUN`, `RUN "file"`, `CONT`, `STOP`, `END`, `NEW`, `LIST`, `CLEAR`, `AUTO`, `RENUM`, `DELETE`, `EDIT` |
| Sequential I/O | `OPEN`, `CLOSE`, `PRINT#`, `WRITE#`, `INPUT#`, `LINE INPUT#` |
| Random-access I/O | `FIELD`, `LSET`, `RSET`, `PUT`, `GET`, `CVI`/`CVS`/`CVD`, `MKI$`/`MKS$`/`MKD$` |
| Program I/O | `SAVE`, `LOAD`, `MERGE`, `CHAIN`, `COMMON` |
| Event trapping | `ON TIMER(n) GOSUB`, `TIMER ON`/`OFF`/`STOP`, `ON KEY(n) GOSUB`, `KEY(n) ON`/`OFF`/`STOP` |
| Error handling | `ON ERROR GOTO`, `RESUME`, `RESUME NEXT`, `RESUME n`, `ERROR`, `ERR`, `ERL` |
| User functions | `DEF FN`, `RANDOMIZE` |
| File management | `KILL`, `NAME`, `FILES`, `MKDIR`, `RMDIR`, `CHDIR`, `SHELL` |
| Date/time | `DATE$`, `TIME$`, `TIMER` |
| Screen | `LOCATE`, `COLOR`, `WIDTH`, `SCREEN`, `KEY ON`/`OFF`/`LIST`, `KEY n,"string"` |
| Graphics | `PSET`, `PRESET`, `LINE`, `CIRCLE`, `DRAW`, `PAINT` |
| Sound | `SOUND`, `BEEP`, `PLAY` (MML parser, PulseAudio backend) |
| Misc | `POKE`, `KEY`, `TRON`/`TROFF`, `OPTION BASE`, `MID$` assignment |
| System | `SYSTEM` |

## Printer Output (LPRINT / LLIST)

`LPRINT` works identically to `PRINT` but sends output to the printer:

- **Default:** output is appended to `LPT1.TXT` in the current directory
- **`--lpt /dev/lp0`** (Linux) or **`--lpt LPT1`** (FreeDOS): send to real hardware
- **`--lpt report.txt`**: redirect to a custom file

`LLIST` lists the program to the printer, with optional line number ranges
(`LLIST`, `LLIST 10-50`, `LLIST -100`).

Both support `PRINT USING`, semicolons, commas, `TAB()`, and `SPC()`.

## Graphics

Graphics mode is activated with `SCREEN 1` (320x200, 4 colors) or
`SCREEN 2` (640x200, monochrome). Drawing commands render to an internal
framebuffer and output via [Sixel graphics](https://en.wikipedia.org/wiki/Sixel),
which works in terminals like xterm, mlterm, foot, and WezTerm.

### Drawing Commands

- `PSET (x,y), color` / `PRESET (x,y)` — set/reset individual pixels
- `LINE (x1,y1)-(x2,y2), color [,B|BF]` — lines, boxes, filled boxes
- `CIRCLE (cx,cy), r [,color [,start, end [,aspect]]]` — circles and arcs
- `PAINT (x,y), fill, border` — flood fill
- `DRAW string` — turtle graphics mini-language (U/D/L/R/E/F/G/H, M, C, S, B, N)
- `POINT (x,y)` — read pixel color
- `COLOR fg, bg` — set foreground/background colors

### Example

```
SCREEN 1
LINE (0,0)-(319,199), 1
CIRCLE (160,100), 80, 2
PAINT (160,100), 3, 2
```

## Sound

- `SOUND frequency, duration` — play a tone (frequency in Hz, duration in clock ticks)
- `BEEP` — play the default beep
- `PLAY string` — Music Macro Language (MML) string for melodies

Sound output uses PulseAudio when available; commands are silently ignored otherwise.

## Full-Screen Editor (TUI)

When running interactively, GW-BASIC 2026 presents the authentic full-screen
editor:

- 25×80 screen buffer by default, or full terminal size with `--full`
- Press Enter on any screen line to re-enter it as BASIC input
- Insert/Overwrite toggle (Insert key)
- Home/End/Delete/Backspace/Escape for line editing
- Ctrl+C interrupts running programs
- Uses the ANSI alternate screen buffer for clean terminal restore on exit

### Function Keys

Default F1-F10 bindings match the original GW-BASIC:

| Key | Default | Key | Default |
|-----|---------|-----|---------|
| F1 | `LIST ` | F6 | `,"LPT1:"` + Enter |
| F2 | `RUN` + Enter | F7 | `TRON` + Enter |
| F3 | `LOAD"` | F8 | `TROFF` + Enter |
| F4 | `SAVE"` | F9 | `KEY ` |
| F5 | `CONT` + Enter | F10 | `SCREEN 0,0,0` + Enter |

- `KEY ON` — show the function key bar on line 25
- `KEY OFF` — hide the bar
- `KEY LIST` — display all definitions
- `KEY n, "string"` — redefine a function key

### Piped Mode

When stdin is not a terminal (piped input), the TUI is not activated.
The interpreter reads lines from stdin and writes output directly to stdout,
suitable for scripting and test harnesses.

### Program Editing

- `EDIT [linenum]` — display a program line for editing in the TUI; press Enter to re-store it
- `AUTO [start][,increment]` — automatic line numbering mode
- `RENUM [new][,[old][,increment]]` — renumber program lines (patches all GOTO/GOSUB references)
- `DELETE range` — delete program lines (`DELETE 10-50`, `DELETE -100`, `DELETE 200-`)

## Event Trapping

GW-BASIC supports event-driven programming through trap handlers that fire
between statements during program execution.

### Timer Events

```
ON TIMER(n) GOSUB line    ' register handler (fires every n seconds)
TIMER ON                  ' enable timer trapping
TIMER STOP                ' suspend trapping (events are queued)
TIMER OFF                 ' disable trapping (events are discarded)
```

### Function Key Events

```
ON KEY(n) GOSUB line      ' register handler for F-key n (1-10)
KEY(n) ON                 ' enable trapping for key n
KEY(n) STOP               ' suspend trapping (events are queued)
KEY(n) OFF                ' disable trapping
```

Event handlers execute as implicit GOSUBs. The `RETURN` statement returns
to the interrupted code and clears the handler's in-progress flag. Events do
not fire inside their own handler (re-entrant protection).

`TIMER STOP` / `KEY(n) STOP` queue events while stopped; switching to
`TIMER ON` / `KEY(n) ON` fires the pending event immediately.
