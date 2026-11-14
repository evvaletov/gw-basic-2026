# Reproducing the demos

The README uses a FreeDOS screen capture, a native compiler example and an executed Jupyter notebook. The BASIC listings are in `examples/squares.bas` and `examples/circles.bas`.

## Build on POSIX

Run from the repository root with the dependencies in [Getting started](getting-started.md):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Compile the squares program

GCC is required for native compilation. Redirect standard input to run the interpreter in batch mode, then keep the compiler's generated files in `build/`:

```sh
build/gwbasic examples/squares.bas < /dev/null
cp examples/squares.bas build/squares.bas
build/gwbasic-compile build/squares.bas -c --keep-c --runtime .
build/squares
```

Both executions print the integers 1 to 8 and their squares. The demo was checked by comparing their standard output byte for byte. The compiler writes `build/squares.c` and `build/squares`.

## Run the FreeDOS package

Download `gwbasic-0.18.2.zip` from the [v0.18.2 release](https://github.com/evvaletov/gw-basic-2026/releases/tag/v0.18.2) and check it against the release's `SHA256SUMS`. Copy it to the DOS working directory as `GWBASIC.ZIP`. This archive contains the 16-bit real-mode interpreter, which needs no extender. The 32-bit package requires `DOS4GW.EXE`, which is not included.

Copy `examples/squares.bas` from the source checkout to the DOS working directory as `SQUARES.BAS`. On an existing FreeDOS installation with FDINST configured, run:

```text
FDINST install GWBASIC.ZIP
GWBASIC
```

At the BASIC prompt, enter:

```text
LOAD "SQUARES.BAS"
LIST
RUN
```

Use `SYSTEM` to return to DOS. `FDINST remove GWBASIC` removes the package.

The screenshot was captured from QEMU's VGA display with FreeDOS 1.4, an emulated 486 and 32 MB RAM. The local package test used FDINST 0.99.7 with `installsources 1` in `FDNPKG.CFG`. It checked the installed executable, metadata and source archive against the ZIP, ran the DOS smoke program and squares example, then removed the package. Source is stored in `SOURCE/GWBASIC/SOURCE.TGZ` to preserve the repository's filenames. This is a local installation test; it does not establish acceptance into a FreeDOS repository.

## Execute the graphics notebook

Create and activate a Python environment, then install the kernel and JupyterLab:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e . jupyterlab
gwbasickernel-install --sys-prefix
GWBASIC="$PWD/build/gwbasic" jupyter lab examples/graphics.ipynb
```

Select the GW-BASIC 2026 kernel and run the code cell. It contains `circles.bas` followed by `RUN`. The saved notebook retains the output from a real execution, including intermediate drawing frames. The README preview is a screenshot of an HTML export showing only the final frame.

The notebook demo was executed with Python 3.12, the local Release build of the interpreter and the repository's kernel. A notebook needs no Sixel-capable terminal: the kernel decodes Sixel into PNG. Running `circles.bas` directly in a POSIX terminal does require Sixel support to display the graphics.

## Scope of the examples

These small programs demonstrate editing, compilation and graphics. They do not establish compatibility with every GW-BASIC program. See [Development](development.md) for the wider test coverage and [Language reference](language-reference.md) for emulated or unsupported hardware features.
