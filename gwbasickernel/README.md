# GW-BASIC Jupyter kernel

Run GW-BASIC programs in JupyterLab or Notebook. Variables and program lines
persist between cells. The kernel converts the interpreter's Sixel output to
inline PNG images, so the notebook browser does not need Sixel support.

Requires Python 3.10 or later and the native
[GW-BASIC 2026 interpreter](https://github.com/evvaletov/gw-basic-2026)
on Linux or macOS. The Python package does not include that interpreter.
The DOS executables cannot be used as the notebook backend.

## Install the interpreter

With Homebrew on Linux or macOS:

```sh
brew tap evvaletov/gwbasic https://github.com/evvaletov/gw-basic-2026.git
brew install evvaletov/gwbasic/gwbasic
```

The Homebrew formula builds from a pinned source revision. It includes the
interpreter and native compiler, with PulseAudio disabled.

Alternatively, build from source with a C11 compiler and CMake:

```sh
git clone https://github.com/evvaletov/gw-basic-2026.git
cd gw-basic-2026
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
export GWBASIC="$PWD/build/gwbasic"
```

## Install the kernel

Create a Python environment for the kernel and JupyterLab:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install gwbasickernel jupyterlab
gwbasickernel-install --sys-prefix
jupyter lab
```

Choose **GW-BASIC 2026** when creating a notebook. The kernel looks for the
interpreter in `GWBASIC`, then on `PATH`. To save its location in the kernelspec,
use this command instead of the installer command above:

```sh
gwbasickernel-install --sys-prefix --gwbasic /absolute/path/to/gwbasic
```

If Jupyter runs in another Python environment, use `--user` to register the
kernel for your account. Keep the kernel's Python environment in place:
the kernelspec records its Python executable.

## Try it

```basic
10 FOR N=1 TO 8
20 PRINT N;N*N
30 NEXT N
RUN
```

For graphics, clear the previous program first:

```basic
NEW
10 SCREEN 1
20 CIRCLE (160,100),80,2
RUN
```

See the [executed graphics notebook](https://github.com/evvaletov/gw-basic-2026/blob/main/examples/graphics.ipynb)
and [demo guide](https://github.com/evvaletov/gw-basic-2026/blob/main/docs/demos.md).

`%new` clears the BASIC program; `%reset` starts a fresh interpreter process.
`%timeout 60` changes the output wait timeout from its default of 30 seconds.
