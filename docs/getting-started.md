# Getting Started

## Dependencies

- C11 compiler (GCC or Clang)
- CMake 3.10+
- PulseAudio development library (`libpulse-simple`) — optional, for `SOUND`/`BEEP`/`PLAY`

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
