# gwbasic-c

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

Requires a C11 compiler and CMake 3.10+. No other dependencies.

## Usage

Interactive mode:

```
$ ./gwbasic
GW-BASIC 0.4.0
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
./gwbasic examples/prime_sieve.bas
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
SPACE$, STRING$, HEX$, OCT$, INSTR

**Statements:**

| Category | Statements |
|----------|------------|
| Output | PRINT, LPRINT, PRINT USING, WRITE, CLS |
| Variables | LET, DIM, ERASE, SWAP, DEFINT/SNG/DBL/STR |
| Control flow | GOTO, GOSUB/RETURN, FOR/NEXT, IF/THEN/ELSE, WHILE/WEND, ON...GOTO/GOSUB |
| Input | INPUT, LINE INPUT, DATA/READ/RESTORE |
| Program | RUN, RUN "file", CONT, STOP, END, NEW, LIST, CLEAR |
| Sequential I/O | OPEN, CLOSE, PRINT#, WRITE#, INPUT#, LINE INPUT# |
| Random-access I/O | FIELD, LSET, RSET, PUT, GET, CVI/CVS/CVD, MKI$/MKS$/MKD$ |
| Program I/O | SAVE, LOAD, MERGE, CHAIN |
| Error handling | ON ERROR GOTO, RESUME, ERROR |
| User functions | DEF FN, RANDOMIZE |
| File management | KILL, NAME |
| Screen | LOCATE, COLOR, WIDTH |

Graphics statements (SCREEN, PSET, LINE, CIRCLE, DRAW, PAINT, etc.) are
parsed but produce no output — programs that use them won't crash.

## Architecture

The interpreter follows the original GW-BASIC's internal structure:

```
Source text → Tokenizer (CRUNCH) → Token stream
                                      ↓
                              Expression evaluator (FRMEVL)
                                      ↓
                              Statement dispatcher (NEWSTT)
                                      ↓
                              HAL (platform I/O)
```

| Module | File | Original |
|--------|------|----------|
| Tokenizer | tokenizer.c | GWMAIN.ASM |
| Evaluator | eval.c | GWEVAL.ASM |
| Interpreter | interp.c | BINTRP.ASM |
| Tokens | tokens.c | IBMRES.ASM |
| Errors | error.c | GWDATA.ASM |
| Math | math_int.c, math_float.c, math_transcend.c | MATH1/2.ASM |
| Strings | strings.c | BISTRS.ASM |
| File I/O | fileio.c | BIPTRG.ASM |
| PRINT USING | print_using.c | BIPRTU.ASM |
| Platform | hal_posix.c | OEM*.ASM |

Key design differences from the original:
- IEEE 754 floating point (MBF conversion only for CVI/CVS/CVD compatibility)
- Dynamic memory allocation instead of 64KB segment layout
- malloc'd strings instead of compacting garbage collector
- setjmp/longjmp for error recovery

## Tests

27 test programs in `tests/programs/`:

```bash
for f in tests/programs/*.bas; do
    echo "$(basename $f):"
    timeout 5 ./build/gwbasic "$f"
done
```

## License

MIT License. See [LICENSE](LICENSE).
