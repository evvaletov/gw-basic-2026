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
| Program I/O | `SAVE` (binary/ASCII), `LOAD` (auto-detects), `MERGE`, `CHAIN`, `COMMON` |
| Event trapping | `ON TIMER(n) GOSUB`, `TIMER ON`/`OFF`/`STOP`, `ON KEY(n) GOSUB`, `KEY(n) ON`/`OFF`/`STOP` |
| Error handling | `ON ERROR GOTO`, `RESUME`, `RESUME NEXT`, `RESUME n`, `ERROR`, `ERR`, `ERL` |
| User functions | `DEF FN`, `RANDOMIZE` |
| File management | `KILL`, `NAME`, `FILES`, `MKDIR`, `RMDIR`, `CHDIR`, `SHELL`, `RESET` |
| Date/time | `DATE$`, `TIME$`, `TIMER` (read/write; `DATE$`/`TIME$` assignment accepted) |
| Environment | `ENVIRON`, `ENVIRON$` |
| Screen | `LOCATE`, `COLOR`, `WIDTH`, `SCREEN`, `KEY ON`/`OFF`/`LIST`, `KEY n,"string"` |
| Graphics | `PSET`, `PRESET`, `LINE`, `CIRCLE`, `DRAW`, `PAINT`, `GET`, `PUT`, `VIEW`, `WINDOW`, `PALETTE`, `PMAP` |
| Sound | `SOUND`, `BEEP`, `PLAY` (MML parser, PulseAudio backend) |
| Memory | `DEF SEG`, `PEEK`, `POKE`, `BSAVE`, `BLOAD` |
| Hardware I/O | `OUT`, `INP`, `WAIT`, `MOTOR`, `STICK`, `STRIG` |
| Device stubs | `ERDEV`, `ERDEV$`, `IOCTL`, `IOCTL$`, `COM`, `LCOPY` |
| Misc | `KEY`, `TRON`/`TROFF`, `OPTION BASE`, `MID$` assignment |
| System | `SYSTEM` |

## Program I/O (SAVE / LOAD)

`SAVE` writes the current program to a file. The default format is tokenized
binary (compact, fast to load), matching the original GW-BASIC behavior:

```
SAVE "myprog.bas"       ' tokenized binary (default)
SAVE "myprog.bas",A     ' ASCII text (human-readable, editable)
```

`LOAD` reads a program file, auto-detecting the format from the first byte:

```
LOAD "myprog.bas"       ' auto-detects binary or ASCII
LOAD "myprog.bas",R     ' load and run immediately
```

Binary files use the standard GW-BASIC 0xFF header format. Command-line
loading (`./gwbasic file.bas`) also auto-detects format.

`MERGE` loads an ASCII file without clearing the current program, overlaying
lines by number. `CHAIN` loads and runs a new program, optionally preserving
variables listed by `COMMON`.

## INKEY$ Extended Keys

`INKEY$` returns a zero-length string when no key is available, a one-byte
string for regular ASCII keys, or a two-byte string for extended keys:

```
K$ = INKEY$
IF LEN(K$) = 2 THEN scan = ASC(MID$(K$, 2, 1))
```

Extended keys return `CHR$(0)` as the first byte and the IBM PC scan code
as the second. Common scan codes:

| Key | Scan | Key | Scan |
|-----|------|-----|------|
| F1-F10 | 59-68 | Home | 71 |
| Up | 72 | PgUp | 73 |
| Left | 75 | Right | 77 |
| End | 79 | Down | 80 |
| PgDn | 81 | Ins | 82 |
| Del | 83 | | |

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

### Sprite Capture and Blit (GET / PUT)

- `GET (x1,y1)-(x2,y2), array` — capture a screen rectangle into an integer array
- `PUT (x,y), array [, action]` — blit a captured sprite back to the screen

The `action` parameter controls how the sprite combines with the existing screen:

| Action | Effect |
|--------|--------|
| `XOR` (default) | Exclusive OR — drawing twice erases the sprite |
| `PSET` | Overwrite screen with sprite pixels |
| `PRESET` | Overwrite screen with inverted sprite pixels |
| `AND` | Bitwise AND of screen and sprite |
| `OR` | Bitwise OR of screen and sprite |

Sprite data is stored in CGA-compatible packed format: word 0 is the width
in bits, word 1 is the height, and remaining words contain packed pixel data
matching the original GW-BASIC representation.

### VIEW / WINDOW / PALETTE

- `VIEW [[SCREEN] (x1,y1)-(x2,y2) [,[fill][,border]]]` — define a graphics
  viewport.  Without `SCREEN`, drawing coordinates are relative to the
  viewport origin.  With `SCREEN`, coordinates remain absolute.  Without
  arguments, resets to full screen.
- `WINDOW [[SCREEN] (x1,y1)-(x2,y2)]` — map logical coordinates onto the
  viewport.  Without `SCREEN`, Y increases upward (Cartesian); with `SCREEN`,
  Y increases downward.  Without arguments, resets to physical coordinates.
- `PALETTE [attribute, color]` — remap a color attribute to a different
  physical color (0–15).  Without arguments, resets to the default CGA palette.
- `PMAP(coordinate, function)` — convert between logical and physical
  coordinates.  Function 0/1 = logical→physical X/Y; 2/3 = physical→logical X/Y.

### Example

```
SCREEN 1
LINE (0,0)-(319,199), 1
CIRCLE (160,100), 80, 2
PAINT (160,100), 3, 2
```

## DEF SEG / PEEK / POKE

`DEF SEG`, `PEEK`, and `POKE` provide access to a virtual 8086 address space
that emulates the memory layout programs expected on a real IBM PC:

```
DEF SEG = &HB800       ' select CGA video buffer segment
POKE 0, 65             ' write 'A' to top-left screen cell
PRINT PEEK(1)          ' read the color attribute
DEF SEG                ' reset to default segment
```

### Emulated Memory Regions

| Segment | Address Range | Description |
|---------|---------------|-------------|
| `0040` | BIOS data area | Video mode (`0049`), column count (`004A`), cursor position (`0050-0051`), timer ticks (`006C-006F`), screen rows (`0084`), keyboard shift flags (`0017`: bit 7 = insert mode) |
| `B800` | CGA text buffer | Character/attribute pairs in text mode (even byte = char, odd byte = attr, 80 columns × 25 rows = 4000 bytes) |
| `B800` | CGA graphics buffer | In SCREEN 1/2: CGA interlaced layout (even rows at offset 0, odd rows at 0x2000, 80 bytes/row) |

All other segments read as 0 and writes are silently discarded. The timer
tick counter at `0040:006C` tracks real time at the original 18.2 Hz rate.

## BSAVE / BLOAD

`BSAVE` saves a block of virtual memory to a binary file with a 7-byte header.
`BLOAD` loads it back. These operate on whichever segment was last set by
`DEF SEG`.

```
DEF SEG = &HB800
BSAVE "screen.bin", 0, 4000    ' save the CGA text buffer
CLS
BLOAD "screen.bin"              ' restore it
BLOAD "screen.bin", 100         ' load to a different offset
```

The file format is compatible with the original GW-BASIC: byte 0 = `0xFD`,
bytes 1-2 = segment (LE), bytes 3-4 = offset (LE), bytes 5-6 = length (LE),
followed by the raw data bytes.

## Hardware I/O (OUT / INP / WAIT)

`OUT`, `INP`, and `WAIT` provide access to emulated IBM PC I/O ports, enabling
classic programs that drive hardware directly — speaker tones via the 8253 PIT,
CGA palette tricks, and serial port polling.

```
OUT &H43, &HB6              ' PIT channel 2: lobyte/hibyte, mode 3
OUT &H42, &HD3 : OUT &H42, &H04   ' set frequency divisor (1235 → 966 Hz)
OUT &H61, INP(&H61) OR 3    ' speaker on
OUT &H61, INP(&H61) AND &HFC ' speaker off
```

### Emulated Ports

| Port | Device | Behavior |
|------|--------|----------|
| `&H42`–`&H43` | 8253 PIT channel 2 | Speaker frequency divisor (1193180 / divisor Hz) |
| `&H61` | PPI Port B | Bits 0–1 control speaker; both set = tone on, either clear = off |
| `&H3D8` | CGA mode control | Mode register; writes with changed mode bits trigger `SCREEN` changes |
| `&H3D9` | CGA color select | Background color (bits 0–3) and palette select (bit 5) |
| `&H201` | Game port | Returns `&HF0` (no joystick connected) |
| `&H3F8`–`&H3FE` | COM1 serial | Minimal stub; LSR (`&H3FD`) returns `&H60` (transmitter ready) |
| Default | — | Reads return `&HFF` (floating bus), writes silently discarded |

### Related Functions

- `INP(port)` — read a byte from an I/O port
- `STICK(n)` — joystick axis position (returns 128 = center, n = 0–3)
- `STRIG(n)` — joystick button state (returns 0 = not pressed)
- `WAIT port, mask [, xor_mask]` — busy-wait until `(INP(port) XOR xor_mask) AND mask` is nonzero; Ctrl+C breaks out
- `MOTOR [n]` — accepted and silently ignored (cassette motor control)

When the PPI speaker bits are set, the PIT frequency divisor is used to
generate a continuous tone via PulseAudio (same backend as `SOUND` / `PLAY`).

## Environment Variables

- `ENVIRON "var=value"` — set an environment variable (uses the C `putenv()` call)
- `ENVIRON$("var")` — read an environment variable (returns "" if not set)

```
ENVIRON "GREETING=Hello"
PRINT ENVIRON$("GREETING")   ' prints: Hello
PRINT ENVIRON$("PATH")       ' prints the system PATH
```

## Date/Time Assignment

`DATE$` and `TIME$` can be assigned to set the date and time:

```
DATE$ = "01-15-2026"
TIME$ = "14:30:00"
```

These assignments are accepted for compatibility but do not modify the
system clock.  Reading `DATE$` and `TIME$` always returns the current
system date and time.

## Device Stubs

The following device-related statements and functions are accepted for
compatibility with programs that reference them, but return stub values
since there is no real device hardware:

- `ERDEV` — device error code (always 0)
- `ERDEV$` — device error name (always "")
- `IOCTL [#n,] string` — device control string output (accepted, ignored)
- `IOCTL$(n)` — device control string input (always "")
- `COM ON` / `COM OFF` / `COM STOP` — serial port event trapping (accepted, ignored)
- `LCOPY [n]` — screen dump to printer (accepted, ignored)
- `CALL` / `CALLS` — machine code execution (raises Illegal function call)
- `RESET` — close all open files (equivalent to `CLOSE`)

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

## References

- Microsoft Corporation. *Microsoft GW-BASIC: User's Guide and Reference*.
  Microsoft Press, 1989. ISBN 978-1-55615-260-3.
- Inman, Don and Bob Albrecht. *The GW-BASIC Reference*. Osborne McGraw-Hill,
  1990. ISBN 978-0-07-881644-4.
- Ahl, David H. *BASIC Computer Games: Microcomputer Edition*. Workman, 1978.
  ISBN 978-0-89480-052-8.
- Microsoft Corporation. *GW-BASIC User's Manual*. Microsoft, 1987.
  (OEM bundled; no ISBN.)
- Microsoft Corporation.
  [GW-BASIC Source Code](https://github.com/microsoft/GW-BASIC). Released 2020.
