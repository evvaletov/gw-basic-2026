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
GW-BASIC 2026 0.14.0
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

### Compiler Options

```
Usage: gwbasic-compile [options] input.bas
Options:
  -o FILE        Output C source file (default: stdout)
  -c             Compile to executable (invoke gcc)
  -O LEVEL       GCC optimization level (default: 2)
  --keep-c       Keep generated C file (with -c)
  --runtime DIR  Path to runtime headers/library
  --warn         Static analysis warnings
  --safe         Runtime safety checks (implies --warn)
  --safe=sanitize  Above + address/UB sanitizers (with -c)
```

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
