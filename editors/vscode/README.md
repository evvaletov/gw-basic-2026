# GW-BASIC 2026 for VS Code

Edit GW-BASIC source with syntax highlighting and snippets. Run the active file
with **F5**, or use **GW-BASIC: Compile current file (AOT)** in the Command Palette
or editor context menu. Both commands use VS Code tasks in the integrated terminal.

## Install

Download `gwbasic-2026-0.1.0.vsix` from the `vscode-extension-stable` artifact of a
successful [CI run](https://github.com/evvaletov/gw-basic-2026/actions/workflows/ci.yml).
Unzip the artifact, then select **Extensions → … → Install from VSIX…** or run:

```sh
code --install-extension gwbasic-2026-0.1.0.vsix
```

The extension is packaged separately from the runtime and is not published to
the Marketplace. VS Code 1.75 or later is required. `.bas` and `.gwb` files are
associated with GW-BASIC; use **Change Language Mode** if another BASIC extension
owns the file association. Edit ASCII source; convert binary/tokenized programs
with GW-BASIC's `SAVE "program.bas",A` first.

## Set up the runtime

Build [GW-BASIC 2026](https://github.com/evvaletov/gw-basic-2026#building), or extract
its Linux release archive. Native compilation needs GCC, the runtime headers,
and `build/libgwrt.a`; a runtime built with PulseAudio also needs its development
libraries. The VSIX does not contain these binaries.

Run/Compile are tested on Linux. On Windows, use a WSL window and install the
extension and runtime in WSL. The DOS executables cannot run as editor tasks.
Windows/WSL, macOS runtime execution, and other remote hosts have not been validated.

For a checkout opened as your workspace, set:

```json
{
  "gwbasic.interpreterPath": "./build/gwbasic",
  "gwbasic.compilerPath": "./build/gwbasic-compile"
}
```

For programs elsewhere, use absolute executable paths and a runtime root:

```json
{
  "gwbasic.interpreterPath": "/path/to/gwbasic/bin/gwbasic",
  "gwbasic.compilerPath": "/path/to/gwbasic/bin/gwbasic-compile",
  "gwbasic.runtimePath": "/path/to/gwbasic",
  "gwbasic.compileArgs": ["-c", "-O2", "--safe"]
}
```

The compiler path-handling fix is newer than v0.18.1. Build the current checkout
for compilation in paths containing spaces, quotes, or shell metacharacters;
the published v0.18.1 compiler does not handle those paths safely.

## Running and compiling

- Save new documents to disk before running. Commands wait for dirty files to
  save and stop if the save fails or is cancelled.
- Each task starts in the source file's directory, so relative BASIC file I/O
  works beside the program. In a multi-root workspace, settings come from the
  folder containing that file. Files outside a workspace also work.
- Run focuses the terminal for `INPUT` and keyboard input. After `END`, the
  interpreter remains at its `Ok` prompt. Use `SYSTEM` to exit, or terminate
  the task through **Tasks: Terminate Task** before restarting a running file.
  This is an interpreter session; F5 does not start a debugger.
- Compile's default `-c` creates an executable beside the source, dropping its
  extension (`game.bas` → `game`). The compiler overwrites an existing executable
  there. It does not run the result. For extensionless or dotfile source names,
  the current compiler adds `.out` instead of overwriting the source.
- Use `"gwbasic.compileArgs": []` to print generated C in the task terminal, or
  `["-o", "game.c"]` to write C. `--emit-obj` writes an object instead of an
  executable. `-o` names the generated C file, not the executable.
- In Restricted Mode, highlighting and snippets remain available. Run/Compile
  require Workspace Trust. Virtual documents cannot be executed.

Paths and argument entries are passed literally. Do not add shell quotes,
`~`, environment variables, or `${workspaceFolder}` placeholders to settings;
these are not supported. VS Code task-variable syntax (`${...}`) in filenames,
paths, or arguments is rejected before launching a task, because VS Code would
otherwise expand it. A bare executable name uses the extension host's `PATH`.
Missing executables and compiler errors appear in the task terminal.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `gwbasic.interpreterPath` | `gwbasic` | Executable name or path; no arguments. |
| `gwbasic.compilerPath` | `gwbasic-compile` | Compiler name or path; no arguments. |
| `gwbasic.runtimePath` | empty | Built repository or extracted release containing `include/gwrt.h` and `build/libgwrt.a`. |
| `gwbasic.compileArgs` | `["-c"]` | One string per compiler argument. |

Relative executable paths (starting with `./`, for example) and `runtimePath`
resolve from the file's workspace folder, or its directory for a loose file.
With an empty `runtimePath`, native compilation first looks one level above an
explicit compiler path (the `bin/` or `build/` layout), then in that workspace
folder. If neither has the runtime files, the command asks you to set the path.
An explicit `--runtime` in `compileArgs` overrides this detection. Relative paths
inside `compileArgs` resolve from the source directory, the task's working directory.

## Language support

Highlighting covers line labels, statements, control flow, built-in functions,
word operators, strings, `REM` and apostrophe comments, decimal/exponent literals,
and hexadecimal/octal constants. It is lexical highlighting, not a syntax checker.
Unterminated strings end at the source line so they do not color later lines.

Snippets: `for`, `while`, `if`, `deffn`, `gosub`, `data`, `printusing`, `open`,
`input`, `onerror`. Adjust their line-number placeholders for your program.
Completion, breakpoints, variable inspection, and JetBrains support remain
follow-up work (git-bug `32a637c`).

## Build and test the extension

From `editors/vscode`, using Node.js 22 or later:

```sh
npm ci
npm test
npm run package
npm run test:integration
```

The package is written to `dist/gwbasic-2026-0.1.0.vsix`. Only runtime extension
files, language assets, README, changelog, and license are included. There are no
runtime npm dependencies. The packaging dependency's minimatch override selects
a version with its glob amplification fix; the lockfile pins the build tools.

Integration tests require built `gwbasic`/`gwbasic-compile` binaries in the
repository's `build/`. They download VS Code stable, install the actual VSIX into
a temporary profile, and exercise activation, save/run, native compilation,
source-relative I/O, literal paths, snippets, and files outside the workspace.
Use `xvfb-run -a npm run test:integration` on headless Linux. Set
`VSCODE_EXECUTABLE_PATH` to reuse a local VS Code executable, or
`VSCODE_TEST_VERSION` to test a specific downloaded version. A private Wayland
compositor can be used with `GWBASIC_TEST_WAYLAND=1` and `WAYLAND_DISPLAY`.

The tests never install into your normal VS Code profile. Command unit tests
cover cancelled/failed saves, concurrent saves, trust, settings, and invalid
resources; TextMate tests use the same tokenizer library as VS Code.

MIT licensed. Copyright 2026 Eremey Valetov.
