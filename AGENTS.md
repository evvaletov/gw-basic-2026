# GW-BASIC project coordination

For the owner's requested debug run mode and FORTRAN storage interoperability,
read [docs/fortran-debug-media-handoff.md](docs/fortran-debug-media-handoff.md)
before planning related work. It records the task-brainstorm decision, native
runtime boundaries, TUI/graphical requirements and acceptance criteria.

Preserve pre-existing uncommitted work, including the roadmap and learning
arcade changes. The scripted monitor, POSIX terminal controller and FORTRAN text
exchange are implemented. Read `docs/debugger.md` and the shared
`docs/debug-contract-v1.md`; graphical frontends and next/finish remain planned.
Do not add debug hooks to the compiled
runtime without explicit AOT instrumentation/source-map work.
