# Getting Started

## Dependencies

- C11 compiler (GCC or Clang)
- CMake 3.10+
- PulseAudio development library (`libpulse-simple`) -- optional, for `SOUND`/`BEEP`/`PLAY`

On Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake libpulse-dev
```

On Fedora/RHEL:

```bash
sudo dnf install gcc cmake pulseaudio-libs-devel
```

## Building

```bash
git clone https://github.com/evvaletov/gw-basic-2026.git
cd gw-basic-2026
mkdir -p build && cd build
cmake .. && make
```

The binary is `build/gwbasic`.

## Usage

### Interactive Mode

Running `./gwbasic` with no arguments launches the full-screen editor:

```
$ ./gwbasic
GW-BASIC 2026 0.17.0
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

Use arrow keys to move the cursor freely. Press Enter on any screen line to
re-enter it. F1-F10 insert common commands (F2 runs the program).

### Running a Program File

```bash
./gwbasic tests/programs/prime_sieve.bas
```

### Piped Input

```bash
echo '10 FOR I=1 TO 10:PRINT I*I;:NEXT' | ./gwbasic
```

### Direct Mode Expressions

Type expressions and statements at the `Ok` prompt:

```
PRINT SIN(3.14159/2)
 1
A$="HELLO WORLD":MID$(A$,7,5)="BASIC":PRINT A$
HELLO BASIC
```

### Command-Line Options

```
Usage: gwbasic [options] [file.bas]
Options:
  -f, --full         Use full terminal size (default: 25x80)
  -h, --help         Show this help
  --lpt DEVICE|FILE  Printer output destination (default: LPT1.TXT)
                     Use LPT1 or /dev/lp0 for real hardware
  -v, --version      Show version
```

## Ahead-of-Time Compiler

`gwbasic-compile` translates `.bas` programs to C source, then optionally
invokes GCC to produce native executables linked against `libgwrt.a`.

### Basic Usage

```bash
# Emit C source to stdout
build/gwbasic-compile program.bas

# Compile to native executable
build/gwbasic-compile -c --runtime . program.bas
```

Both numbered (`10 PRINT "HI"`) and unnumbered (`PRINT "HI"`) sources
compile.  Unnumbered lines get auto-assigned numbers (10, 20, 30, ...) so
the analysis pass and codegen can produce labeled statements; explicit
line numbers are preserved.  Direct-mode scratchpad scripts and classic
"just a list of statements" programs compile without manual renumbering.

### Compiler Options

```
Usage: gwbasic-compile [options] input.bas
Options:
  -o FILE          Output C source file (default: stdout)
  -c               Compile to executable (invoke gcc)
  -O LEVEL         GCC optimization level (default: 2)
  --keep-c         Keep generated C file (with -c)
  --runtime DIR    Path to runtime headers/library
  --warn           Static analysis warnings
  --safe           Runtime safety checks (implies --warn)
  --safe=sanitize  Above + address/UB sanitizers (with -c)
  --no-gc-check    Skip per-line gwrt_check_line() (no GC, no Break)
  --fast-math      Skip division-by-zero checks
```

### Performance Flags (`--no-gc-check` / `--fast-math`)

`--no-gc-check` skips the `gwrt_check_line()` call emitted at the start of
every non-REM line.  That call drives the string-pool compacting GC and
the Ctrl+Break trap.  Removing it gives a small per-line speedup for
programs that don't allocate strings or need responsive interruption.
String reassignment can still trigger compaction lazily, but the
guaranteed periodic check is gone.

`--fast-math` removes the explicit divide-by-zero check around the `/`
operator.  The result of `X = 10 / 0` becomes `inf` rather than raising
"Division by zero".  Useful for compute-bound code that already validates
inputs.

### Memory Safety (`--warn` / `--safe`)

The `--warn` flag enables compile-time static analysis warnings:

- **Uninitialized variables** -- variables used before their first assignment
  (via LET, FOR, READ, INPUT)
- **GOTO/GOSUB to nonexistent line** -- jump targets that don't exist in the
  program
- **Unreachable code** -- lines after unconditional GOTO/END/STOP that are not
  jump targets

The `--safe` flag (implies `--warn`) adds runtime safety checks to the
generated C:

- **Integer overflow detection** -- arithmetic on integer (%) variables uses
  checked functions (`gw_int_add`, `gw_int_sub`, `gw_int_mul`) that raise
  "Overflow" instead of silently wrapping, matching real GW-BASIC behavior
- **Enhanced array diagnostics** -- subscript errors report the array name,
  subscript value, line number, and which dimension exceeded its bound
- **GOSUB stack diagnostics** -- stack overflow reports the source line and
  current depth

The `--safe=sanitize` flag (with `-c`) additionally passes
`-fsanitize=address,undefined` to GCC for full memory error detection.

```bash
# Warnings only (zero runtime cost)
build/gwbasic-compile --warn program.bas

# Runtime safety checks
build/gwbasic-compile --safe -c --runtime . program.bas

# Full sanitizer build (debugging)
build/gwbasic-compile --safe=sanitize -c --runtime . program.bas
```

### Cross-Language Linking (`--emit-obj` / `--main-name`)

`--emit-obj` produces a `.o` object file instead of a final executable;
`--main-name NAME` renames the entry point so it doesn't collide with
the host project's `main()`.  Together they let you link BASIC into a
larger C or Fortran build.

```bash
# BASIC source compiled to greet.o with renamed entry point
build/gwbasic-compile --emit-obj --main-name=run_basic_greet \
    --runtime . greet.bas
```

C driver:

```c
extern int run_basic_greet(int argc, char **argv);

int main(void) {
    run_basic_greet(0, NULL);  /* runs the BASIC program */
    return 0;
}
```

Link both together:

```bash
gcc driver.c greet.o -L./build -lgwrt -lm -lpthread -lpulse-simple
```

Fortran driver (modern, with `iso_c_binding`):

```fortran
program main
  use iso_c_binding
  interface
    function run_basic_greet(argc, argv) bind(c, name="run_basic_greet")
      use iso_c_binding
      integer(c_int), value :: argc
      type(c_ptr), value :: argv
      integer(c_int) :: run_basic_greet
    end function
  end interface
  integer(c_int) :: rc
  rc = run_basic_greet(0, c_null_ptr)
end program
```

```bash
gfortran driver.f90 greet.o -L./build -lgwrt -lm -lpthread -lpulse-simple
```

The BASIC code shares the `gw` interpreter state with `libgwrt`, so a
single binary runs at most one BASIC program at a time.  Calling BASIC
from C / Fortran is always safe; calling C / Fortran from BASIC needs
the foreign-function-declaration extension on the roadmap (Level 2).

## Building for DOS / FreeDOS

GW-BASIC 2026 cross-compiles to DOS using OpenWatcom V2 (`wcc` / `wcc386`).
Two targets are available:

### 16-bit real-mode (recommended for FreeDOS)

Produces a standalone 128KB MZ executable -- no DOS extender required.

```bash
wmake -f Makefile.dos16
```

Requires OpenWatcom V2 with 16-bit DOS target.  Uses MEDIUM memory model
(`-mm`): code can exceed 64KB, data must fit in 64KB.

### 32-bit DOS/4GW

Produces a 175KB LE executable requiring `DOS4GW.EXE` (265KB) at runtime.
Also builds the compiler (`GWBASCOM.EXE`) and runtime library (`GWRT.LIB`).

```bash
wmake -f Makefile.dos
```

### Running on FreeDOS

Copy `GWBASIC.EXE` (and `DOS4GW.EXE` for the 32-bit build) to your FreeDOS
system.  Run programs from the command line:

```
C:\> GWBASIC PROGRAM.BAS
```

Running without arguments launches the interactive editor.  The TUI renders
through BIOS INT 10h with the screen buffer in far memory, so the full-screen
editor, F-key bar, cursor positioning, and scrolling all work on bare FreeDOS
without `ANSI.SYS`.

### Verifying the DOS Build

Two automated checks run from a Linux host:

```bash
./build_dos.sh 16            # produces gwbasic16.exe (~128KB)
./build_dos.sh 32            # produces gwbasic.exe (~175KB)
bash tests/run_dos_smoke.sh  # runs gwbasic16.exe under DOSBox-X, diffs golden
```

The smoke harness validates non-interactive features (arithmetic, strings,
control flow, GOSUB, FOR/NEXT, DATA/READ, DEF FN, file I/O via OPEN/PRINT#).
The interactive TUI features below need a manual session under DOSBox-X or
real FreeDOS:

| Check | What to do | Expected |
|-------|-----------|----------|
| TUI startup | Launch `GWBASIC.EXE` with no arguments | `Ok` prompt, F-key bar at row 25 (`1LIST 2RUN ...` in inverse video) |
| Cursor keys | Press up/down/left/right | Cursor moves freely without printing characters |
| Re-enter line | Type `10 PRINT "HI"`, Enter; arrow up to that line, Enter | Line re-tokenized; subsequent `LIST` shows it stored |
| F1 (LIST) | Press F1 then Enter | Inserts `LIST `, runs `LIST` |
| F2 (RUN) | Type a program, press F2 | Runs it (`RUN\r` is appended) |
| Insert toggle | Press Ins; type characters mid-line | Cursor switches between block (insert) and underline (overwrite) shapes; characters insert vs overstrike accordingly |
| Home / End | Press Home, End | Cursor jumps to column 0 / past last printable char on the row |
| Scroll | Fill the screen with output | Bottom row pinned to the F-key bar; new lines push old ones up |
| Ctrl-C | Run `10 GOTO 10` and press Ctrl-C | Program stops with `Break in 10` |
| KEY OFF / KEY ON | `KEY OFF` then `KEY ON` | F-key bar disappears / reappears |
| CLS | `CLS` | Screen clears, cursor at top-left |
| Exit | `SYSTEM` | Returns to DOS prompt cleanly (no leftover escape codes) |
