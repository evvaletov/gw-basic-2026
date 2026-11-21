# Debug command/event contract v1

Implemented by the FORTRAN AST and BASIC interpreter line monitors. This file
is kept identical in the two repositories; change it together with both tests.

Start with `--debug-script COMMANDS --debug-output NEWFILE`. Both paths are
required. Commands come from a dedicated text file (or explicitly supplied FIFO);
the output file must not exist. Command read and log write/close failures
end the process with status 2 instead of silently resuming. Program stdin, stdout and display remain theirs.
The first stop is before the first executable statement. A script EOF disables
the monitor and resumes execution, including pending breakpoints.

| Command | Effect |
| --- | --- |
| `capabilities` | Lists supported commands and explicit exclusions |
| `step` | Executes the stopped statement, then pauses at the next dispatch, entering source-level calls |
| `continue` | Runs until a breakpoint or program termination |
| `break LOCATION` | Adds a breakpoint; maximum 16 |
| `clear` | Removes all breakpoints |
| `watch REF` | Adds a variable or constant-index array element and prints it; maximum 16 |
| `unwatch REF` | Removes that watch, ignoring letter case |
| `inspect REF` | Prints one value without registering a watch |
| `checkpoint NAME` | Emits a named, bounded inspection export to the log |
| `snapshot NAME` | Same inspection export as `checkpoint` |

Names use letters, digits, underscores, full stops or hyphens, maximum 64
characters. Commands are lowercase, one per line, maximum 254 characters before
the newline. Blank lines and lines beginning with `#` are ignored. References
have no embedded spaces; names are case-insensitive. Array subscripts are decimal
integer literals, e.g. `A(2,3)` or BASIC `A%(2,3)`. Duplicate watch spellings are
idempotent; BASIC `A` and `A!` remain distinct watch registrations because the
former follows the current DEF type. Unsupported expressions and commands emit
`error ...` and leave execution/state unchanged. No expressions or functions are
evaluated, and absent BASIC variables/arrays are never allocated by inspection.

FORTRAN LOCATION is the physical source line in the input deck, not a statement
label. BASIC LOCATION is `LINE` or `LINE:OFFSET`, where OFFSET is the zero-based
byte offset in the tokenised line, as printed by a stop event. It is not a source
character column. A BASIC line breakpoint stops at each dispatched statement on
that line, including an inline THEN/ELSE statement. FORTRAN continuation cards do
not create extra steps. Statement functions and SAP instructions have no internal
step points in this version. A source-level CALL steps into a FORTRAN unit or a
BASIC GOSUB; a SAP call completes before the next AST stop.

The text event stream begins `debug-v1 engine=... frontend=line`. Stable record
prefixes are `stop`, `value`, `break`, `checkpoint`, `error`, `resume` and
`capabilities`; the remaining payload is native to the engine. Stop IDs increase
within a process. A checkpoint has `begin scope=...` and matching `end` records
and refers to the current stop ID. Its scope is registered watches plus FORTRAN
flags/media positions or BASIC table/stack counts/open files and bounded buffers.
It is not a full memory image, a hash of all guest state, a resumable execution
checkpoint or a record of external input. Duplicate names are allowed; use the
stop ID to distinguish them. Watch errors remain visible in the export.

`next`, `finish`, reverse stepping and `restore` are unsupported. BASIC AOT and
FORTRAN SAP instruction debugging are unsupported. Capabilities describe the
engine/frontend separately from the guest's SCREEN mode. The first frontend is
a scripted text monitor, not an interactive pane or graphical inspector.
External clocks/devices keep running during a pause; wall-clock-sensitive input,
audio and TIMER/KEY events can therefore differ from an unpaused run. Watches
never call time/RNG functions, consume input or seek guest files.
