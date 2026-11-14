# GW-BASIC 2026 v0.18.2

This release improves compatibility with original BASIC programs and fixes
long-filename problems in the DOS packages. It also covers the
VS Code Run/Compile integration and compiler path-handling fixes developed since
v0.18.1.

## Compatibility changes

- Preserve full variable names, including periods, up to 40 characters.
- Match the original RND sequence and RANDOMIZE seed handling, including the
  prompt when RANDOMIZE has no argument.
- Compile INPUT$ natively. Apply DEFTYPE declarations in execution order,
  including in programs that call C through `$EXTERN`.
- Preserve FOR/GOSUB loop scope and native error-recovery continuation points.
- Accept ASCII line labels up to 65529; expression integer limits are unchanged.
- Improve terminal INPUT/LINE INPUT editing, resize handling, CP437 display and
  terminal restoration after interruption.
- Invoke GCC without a shell, so compiler paths containing spaces, quotes and
  shell metacharacters are passed literally.

The compatibility corpus now checks 53 scripted cases from 26 unchanged BASIC Computer Games
listings. Fourteen new sessions cover Reverse, Combat, Bombardment, Acey Ducey,
Hangman and Bullfight. Across the full corpus, 49 sessions finish normally, two
stop at recorded input checkpoints and two reproduce errors in the originals.
Each case runs in the interpreter and six compiler modes. Every run is compared
with the Microsoft reference output.

The reference is a pinned Microsoft GW-BASIC assembly-source build made with
JWasm, not retail 3.23. File redirection captures calculations and text output;
it does not establish equivalent terminal wrapping, graphics, sound or hardware
behaviour. The other 72 vendored listings have no scripted runtime coverage.

## Downloads

- `gwbasic-0.18.2-linux-x86_64.tar.gz`: interpreter, BASIC-to-C compiler, runtime
  library, headers and a small example. `BUILD.json` identifies the source commit,
  supplied binaries, packaging platform and libc version. Native compilation needs GCC
  and PulseAudio development libraries because the supplied runtime uses PulseAudio.
- `gwbasic-0.18.2.zip`: 16-bit DOS interpreter with package metadata, documentation
  and source. No DOS extender is needed. Copy or rename the downloaded archive
  to `GWBASIC.ZIP` for use with DOS short filenames.
- `gwbasic-0.18.2-dos32.zip`: 32-bit interpreter; provide `DOS4GW.EXE` separately.
  Copy or rename this archive to `GWBDOS32.ZIP` for use on DOS.
- `gwbasic-0.18.2-source.tar.gz`: source at the recorded commit.
- `SHA256SUMS`: archive and metadata checksums.

The 16-bit ZIP has 8.3 names throughout its outer directory tree. The complete
tracked source is stored in `SOURCE/GWBASIC/SOURCE.TGZ`; extract that inner
archive on a host with long-filename support. Packaging verifies the outer names
and compares every archived source file with the checkout.

The VS Code extension is distributed separately as a VSIX CI artifact. It
supports highlighting, snippets, Run and Compile. Completion and debugging are
still planned.

## Limits

Floating-point arithmetic uses IEEE 754. MBF file conversion does not reproduce
all intermediate arithmetic of the original interpreter. Several hardware
interfaces are emulated or stubbed, and compiled timer/key events are unsupported.
The interpreter and editor are tested on Linux; DOS runtime tests use FreeDOS/QEMU.
The corpus comparisons run on POSIX and have not been repeated on DOS. Physical speaker fidelity remains unverified.

The implementation uses the MIT License. The vendored BASIC Computer Games
listings retain their upstream Unlicense. See CHANGES.TXT for the full history.
