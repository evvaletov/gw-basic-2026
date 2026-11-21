# FORTRAN/GW-BASIC debug and media handoff

Owner-directed handoff, 20 November 2026. Read this when resuming GW-BASIC work.
The first scripted interpreter monitor and FORTRAN text exchange are now
implemented. See [the BASIC debugger guide](debugger.md) and the
[shared v1 contract](debug-contract-v1.md). BASIC still has no native drum or
cassette-image backend.

The owner asked to coordinate useful tape/drum compatibility with FORTRAN 704,
run task-brainstorm on organisation, and provide both projects with a debug run
mode: variable watches, stepping/checkpoints, and appropriate media views, richer
in graphical mode and simpler in a TUI.

The coordinated design and acceptance criteria are in
the sibling file `../../fortran-704/docs/debug-media-plan.md`.
Grok 4.7, Fable 5.2 and Gemini Argon completed both independent panel passes.
All recommended native engines with common commands/specifications/fixtures;
shared C runtime extraction is deferred until proven useful in both projects.

FORTRAN's `word-machine` milestone now supplies physical 32K word addresses,
descending arrays, persistent ascending Hollerith literals and a bounded
SAP/BSS execution path. Its scripted inspector and persistent tape/drum
bundles now use this foundation. See
the sibling file `../../fortran-704/docs/word-machine-validation.md`.

## Implemented first milestone

The interpreter now accepts `--debug-script FILE --debug-output NEWFILE` on
POSIX, DOS16 and DOS32. Step/continue, line/token-offset breaks, pure typed
watches, named inspection checkpoints and native file/FIELD views are available.
The FORTRAN peer saves and loads typed tape/drum bundles and exports printable
ASCII records; `examples/debug-media/read.bas` reads its exchange fixture.
The contract and text fixture are kept in both repositories. Full execution
restore, AOT/SAP instruction stepping, next/finish and rich interactive/graphical
frontends are still follow-ups. This supersedes the plan-only status above.

FIELD mappings describe native copies, not a new pointer-alias implementation.
Inspection preserves those copy semantics and does not synchronise buffers.

## Original requested scope and remaining work

1. Begin with an **interpreter debug mode**, using safe points before executable
   statements. Identify the BASIC line and colon-separated statement offset.
   Add step/continue, breakpoints, pure variable/constant-index watches and
   named inspection checkpoints. Add step-over/finish with explicit call-depth
   semantics when supported. `TRON` is existing tracing, not sufficient debugger
   state/control; the current compiler ignores `TRON`/`TROFF`.
2. Expose typed values, arrays, strings, file modes/positions and random-file
   FIELD buffer slices. Inspection must not create variables, change buffers,
   consume input, seek, call functions or advance random/timer state. Preserve
   BASIC's actual initialisation semantics and show aliases as shared storage.
3. Build the line/TUI controls directly over these hooks. Keep program input and
   output separate from debug commands. Respect 16/32-bit DOS memory limits and
   preserve the running program's text/graphics display. Host graphical views
   can use the same inspection events; don't require a network server or JSON
   library in a 16-bit DOS runtime.
4. Advertise engine and frontend capabilities separately from BASIC `SCREEN`.
   Graphical inspectors can show source, watches and file/FIELD byte maps.
   AOT source stepping needs instrumentation/source maps; keep it unavailable
   until implemented. Don't offer invented punch-card/drum/cassette controls.
5. A checkpoint is initially a named inspection barrier; a snapshot is an
   immutable inspection export. Restoring execution is a separate later feature
   requiring complete frames, variable bindings, loops/events, random state,
   files/buffers/positions and external-effect handling. Reject unsupported
   restore; never replay host writes or consume console input twice silently.

Acceptance: debug off/on runs agree in results, output and file effects;
repeated watches are pure; loops, GOSUB/RETURN, colon statements, arrays,
strings and FIELD aliases are correct; scripted stops/checkpoints repeat;
debugging preserves the DOS screen; unsupported AOT/restore operations fail
clearly. See the canonical plan for common proposed verbs and event contents.

## Storage interoperability work

The inspected tree has sequential/random byte files and `FIELD`/`GET`/`PUT`;
`MOTOR` is a silent cassette stub. No implemented tape-image or drum backend
was found. The original Microsoft GW-BASIC source repository includes cassette
I/O, but this handoff does not establish its image/waveform format.

FORTRAN owns its 36-bit, 6-bit BCD, record/mark tapes and word-addressed drums.
Keep BASIC's byte-file semantics. Useful first interoperability is a text
fixture exported from 704 BCD records and read by BASIC `LINE INPUT#`, with an
explicit character mapping, record boundaries and treatment of tape marks.
Numeric conversion needs specified ranges/precision and byte-exact BASIC
`MKI$`/`MKS$`/`MKD$` and `CVI`/`CVS`/`CVD` fixtures across interpreter/AOT and
supported platforms. No bitwise equivalence to 704 floating point is assumed.

Research cassette layout and `MOTOR`/device behaviour from primary Microsoft
sources before selecting a cassette image format. Preserve that evidence in
this project. Existing emulator formats should be evaluated before inventing
a shared container. No new BASIC drum language commands are implied.

## Worktree coordination

At handoff creation, `docs/roadmap.md` was already modified, and `-` plus
`docs/learning-arcade.md` were untracked. This handoff and the new AGENTS pointer
do not modify or commit those files. Preserve the other session's work and
reconcile its current roadmap before choosing implementation order. FORTRAN's
numerical/library/physical-address work continues independently.

No remote publication, new paid API use or historical-source vendoring is
authorised by this handoff.
