GW-BASIC 2026 for Arch Linux

The package builds the interpreter and native compiler from the v0.18.2 release source. It installs the runtime library and headers needed by gwbasic-compile. GCC is a runtime dependency. This package disables the optional PulseAudio sound backend. The Jupyter kernel is available separately on PyPI as gwbasickernel.

To build from this directory on Arch Linux:

    sudo pacman -S --needed base-devel
    makepkg -si

Then run:

    gwbasic /usr/share/gwbasic-2026/examples/squares.bas < /dev/null

To compile a program in a writable directory:

    cp /usr/share/gwbasic-2026/examples/squares.bas .
    gwbasic-compile squares.bas -c
    ./squares

BASIC's END returns to the editor when standard input is a terminal. Redirecting standard input from /dev/null lets a batch run exit.
