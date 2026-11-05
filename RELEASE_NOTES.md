# GW-BASIC 2026 v0.18.1

This bugfix release corrects terminal input, compiler control flow, and test
failure reporting. It also adds reproducible compatibility checks and release
archives for Linux and DOS.

- Numbered edits no longer print an extra `Ok`. INPUT excludes its prompt;
  AUTO and input editing retain the typed answer. Loading ASCII source preserves
  the active command buffer. Lines whose expanded tokens exceed the buffer are
  rejected before execution. CLEAR rejects truncated binary token payloads.
- The terminal keeps pending input and commands when resized. A viewport follows
  the cursor when the window is smaller than the BASIC screen; the logical
  screen retains its startup dimensions. CLS preserves the visible F-key bar.
  Shortening a scrolled answer on a very small screen keeps the cursor within
  the screen buffer, including an empty answer.
- The 16-bit DOS string pool defaults to 8 KiB, leaving room for program lines
  and GC scratch space in the 64 KiB near-data segment. Other builds retain
  32 KiB. `CLEAR` can request a different size within available memory.
- Ctrl-C interrupts DOS loops even when the program does not read the keyboard.
- LOCATE uses BASIC's one-based coordinates consistently in both execution
  paths. The DOS hardware cursor aligns with the editor's logical position.
- Compiled FOR/NEXT loops track the active loop, including alternate NEXT paths,
  shared NEXT statements, and loop re-entry. GOSUB can return to call sites later
  in the source. CLEAR resets both compiled control stacks. Initially empty FOR
  loops skip their bodies in both interpreter and compiler.
- Compiler fixes cover grouped strings, shared array storage, INPUT variable
  preservation, DEF FN tokens, and backward TAB. Regression failures and
  timeouts now fail CI.

The corpus checks 13 scripted cases against Microsoft GW-BASIC transcripts in
the interpreter and six compiler modes. Awari and Checkers now compile. The full
98-listing build audit reports 97 successes and one syntax rejection: Super Star
Trek contains an unspaced `TO` expression also rejected by Microsoft GW-BASIC.
Build success is not complete gameplay coverage. The original listings and
license remain unchanged.

## Archives

- `gwbasic-0.18.1-linux-x86_64.tar.gz`: interpreter, AOT compiler, runtime library,
  headers, and a small example. CI builds it on Ubuntu 22.04. `BUILD.json` records
  the source commit, binary hashes, CI run, and build host's libc version.
  CI builds all binaries from that source commit; local packaging records
  supplied binaries and checks their versions. PulseAudio runtime libraries are
  required; compiling BASIC also requires GCC and the PulseAudio development
  libraries. See the included `QUICKSTART.txt`.
- `gwbasic-0.18.1.zip`: FreeDOS package with the 16-bit real-mode interpreter,
  metadata, documentation, and source. No DOS extender is required.
- `gwbasic-0.18.1-dos32.zip`: 32-bit DOS/4GW interpreter. Supply `DOS4GW.EXE`
  separately; it is not included.
- `gwbasic-0.18.1-source.tar.gz`: tracked source at the release commit.
- `SHA256SUMS`: checksums for the archives and accompanying metadata.

Verify downloads with `sha256sum --check SHA256SUMS` from their directory.
CI prepares these archives after the Linux and DOS checks pass. Packaging does
not create a tag or publish a release.

## Validation scope and remaining limits

Validation includes the interpreter/compiler suites, compiler mode comparisons,
PTY input and resize checks, curated corpus, and sanitizer runs. Local DOS QA
uses a disposable FreeDOS/QEMU boot disk and checks the actual BIOS-rendered
editor. Audio QA measures only the application's PulseAudio stream on an
isolated sink. Physical speaker fidelity is not established; the development
VM's default audio backend also distorted a separate known-good PCM control.

Strict GW-BASIC error behavior for nonempty FOR statements and conditional
DEFTYPE changes still needs work. Most corpus games lack scripted runtime
coverage. Compiled event handlers and several physical-device interfaces remain
unsupported; see the language reference and roadmap. The implementation is
MIT-licensed; vendored BASIC Computer Games listings use the upstream Unlicense.
