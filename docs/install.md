# Install and download

GW-BASIC 2026 includes an interpreter, an ahead-of-time compiler and a Jupyter
kernel. The [v0.18.2 release](https://github.com/evvaletov/gw-basic-2026/releases/tag/v0.18.2)
provides the following archives, with build records and SHA-256 checksums.
Compare your download with the matching checksum file on the release page.

| Platform | Archive |
| --- | --- |
| Linux x86-64 | [gwbasic-0.18.2-linux-x86_64.tar.gz](https://github.com/evvaletov/gw-basic-2026/releases/download/v0.18.2/gwbasic-0.18.2-linux-x86_64.tar.gz) |
| macOS 15 or later, Apple Silicon | [gwbasic-0.18.2-macos-arm64.tar.gz](https://github.com/evvaletov/gw-basic-2026/releases/download/v0.18.2/gwbasic-0.18.2-macos-arm64.tar.gz) |
| macOS 15 or later, Intel | [gwbasic-0.18.2-macos-x86_64.tar.gz](https://github.com/evvaletov/gw-basic-2026/releases/download/v0.18.2/gwbasic-0.18.2-macos-x86_64.tar.gz) |
| DOS / FreeDOS, 16-bit real mode | [gwbasic-0.18.2.zip](https://github.com/evvaletov/gw-basic-2026/releases/download/v0.18.2/gwbasic-0.18.2.zip) |
| DOS / FreeDOS, 32-bit protected mode | [gwbasic-0.18.2-dos32.zip](https://github.com/evvaletov/gw-basic-2026/releases/download/v0.18.2/gwbasic-0.18.2-dos32.zip) |
| Source | [gwbasic-0.18.2-source.tar.gz](https://github.com/evvaletov/gw-basic-2026/releases/download/v0.18.2/gwbasic-0.18.2-source.tar.gz) |

See the [macOS download instructions](getting-started.md#macos-binary-downloads)
and [DOS setup](getting-started.md#building-for-dos-freedos) for platform details.
The 16-bit DOS build needs no extender; the 32-bit build uses DOS/4GW.
The macOS archives disable audio and are not notarised.
You can also [build from source](getting-started.md#building).

## Homebrew on Linux and macOS

With Homebrew installed:

```sh
brew tap evvaletov/gwbasic https://github.com/evvaletov/gw-basic-2026.git
brew install evvaletov/gwbasic/gwbasic
```

The formula builds a pinned source release with audio disabled. Native BASIC
compilation also needs a C compiler available as `gcc`; Apple's Command Line
Tools provide that command through Clang on macOS.

## Jupyter notebooks

Install the native interpreter first, using Homebrew or a source build. The
Python package does not include it. On Linux or macOS with Python 3.10 or later:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install gwbasickernel==0.1.0 jupyterlab
gwbasickernel-install --sys-prefix
jupyter lab
```

Choose **GW-BASIC 2026** as the notebook kernel. It finds `gwbasic` on `PATH`;
otherwise set the `GWBASIC` environment variable to the interpreter's absolute
path before starting JupyterLab.

The [compiler and Jupyter article](https://valetov.com/articles/gw-basic-2026/)
includes a downloadable notebook and explains how Sixel graphics become inline
images. The [demos](demos.md) give commands for reproducing the screenshots.
