# GW-BASIC 2026 v0.18.3

This release corrects the default Sixel colours in SCREEN 1 and SCREEN 2,
expands the original-program reference tests, and adds playable Awari and
Checkers ports. Downloads cover Linux, DOS and both macOS architectures.

## Changes since v0.18.2

- SCREEN 1 starts with black, light cyan, light magenta and high-intensity white.
  SCREEN 2 starts with black and high-intensity white. Bare `PALETTE` restores
  the defaults for the current mode. Interpreter and native tests cover resets,
  mode changes and overrides; `POINT` continues to return attribute values.
- The unchanged-original corpus grows from 53 cases across 26 listings to
  76 cases across 36 listings: 72 completed sessions, two input checkpoints
  and two expected errors in the original programs. Linux tests compare the
  interpreter and six compiler modes with independent reference transcripts.
- Separately labelled Awari and Checkers ports provide complete games where
  the unchanged listings encounter dialect errors. Ten reference cases cover
  60 finished games and two explicit quit paths. All 98 vendored originals
  remain unchanged.
- macOS archives, Homebrew installation, an Arch package recipe and the
  separately distributed `gwbasickernel` Python package were added between
  releases. The project documentation and compiler/Jupyter article are at
  <https://retroprogramming.org/gwbasic/> and <https://retroprogramming.org/articles/gw-basic-2026/>.
  The article retains its recorded v0.18.2 examples.

The reference interpreter is a pinned build of Microsoft's GW-BASIC assembly
source made with JWasm, not retail 3.23. Redirected transcripts check text and
calculations; they do not establish terminal, graphics, sound or hardware
fidelity. The other 62 vendored listings have no scripted runtime coverage.

## Downloads

- `gwbasic-0.18.3-linux-x86_64.tar.gz`: interpreter, BASIC-to-C compiler, runtime
  library, headers and an example. Native compilation needs GCC and PulseAudio
  development libraries because the supplied runtime uses PulseAudio.
- `gwbasic-0.18.3-macos-arm64.tar.gz` and
  `gwbasic-0.18.3-macos-x86_64.tar.gz`: separate Apple Silicon and Intel builds
  for macOS 15 or later. Audio is disabled; only system libraries are required.
  Native compilation needs Apple's Command Line Tools. The binaries are not
  Developer ID signed or notarised.
- `gwbasic-0.18.3.zip`: 16-bit DOS interpreter, package metadata, documentation
  and source. Rename the download to `GWBASIC.ZIP` for DOS short filenames.
- `gwbasic-0.18.3-dos32.zip`: 32-bit DOS interpreter for DOS/4GW. Rename the
  download to `GWBDOS32.ZIP` for DOS short filenames.
- `gwbasic-0.18.3-source.tar.gz`: tracked source at the release commit.
- `SHA256SUMS` and `SHA256SUMS-macos-*`: checksums for the corresponding
  archives and build records. `BUILD.json` and the macOS build records identify
  their source commit, binaries and build environment.

The 16-bit ZIP uses 8.3 names throughout its outer directory tree. Complete
tracked source is in `SOURCE/GWBASIC/SOURCE.TGZ`; extract that inner archive on
a host with long-filename support. Packaging checks outer names and compares
all archived source files with the checkout.

## Validation and limits

Linux CI runs the full original-listing and port matrices. macOS CI runs both
suites through the interpreter and O2 native mode on each architecture, and
checks the extracted packages. DOS runtime checks use FreeDOS 1.4 in QEMU;
the full gameplay matrices have not been repeated on DOS. Physical speaker
fidelity remains unverified.

Floating-point arithmetic uses IEEE 754. MBF file conversion does not reproduce
all intermediate arithmetic of the original interpreter. Hardware interfaces
include emulations and stubs, and compiled timer/key events remain unsupported.
The palette correction does not complete original CGA `COLOR`/intensity
behaviour or drawing-attribute range validation.

The VS Code extension remains a separate VSIX CI artifact with highlighting,
snippets, Run and Compile. Completion and debugging are planned.

The implementation uses the MIT License; the vendored BASIC Computer Games
listings retain their upstream Unlicense. See CHANGES.TXT for the full history.
