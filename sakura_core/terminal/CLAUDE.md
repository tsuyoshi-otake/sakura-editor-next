# P3 Terminal and Task Backend Guidance

## Ownership

The terminal session service owns PTY/process creation, environment, input,
output, resize, exit, cancellation, and restore. A panel is a view of a session;
hiding or destroying the panel must not accidentally kill or orphan it.

Each launch reaches exactly one terminal outcome, including spawn failure,
normal exit, signal/forced exit, cancellation, host loss, and shutdown timeout.
Bound output queues and background work, and never block the UI thread on PTY
I/O. Tasks compose terminal sessions and problem matchers through stable
services, with deduplicated launch and bounded retry/cancellation.

## Verified Terminal Boundary (2026-07-31)

`CTerminalSession` already isolates the ConPTY backend behind
`ITerminalBackend`, bounds input/output queues and drain work, and exposes
explicit Idle/Starting/Running/Closing/Exited/Failed states. Keep ConPTY handles
private to the backend.

The Task configuration catalog is owned under
`workbench/tasks/CLAUDE.md`. A catalog entry is not an execution session.
Execution must inject its terminal/session factory, preserve argument
boundaries, assign stable run IDs, bound concurrently active runs, and own
cancel/Stop cleanup independently of panel visibility. Process tasks and shell
tasks require different launch-policy paths; do not concatenate a shell command
inside the catalog.

`CTerminalSession` now supplies the lifecycle shape required by
`ITaskExecutionSession`: `BeginClose()` is nonblocking and idempotent, and
`WaitForClose(absoluteDeadline)` returns `Closed` or `DeadlineExceeded` only
after the backend and all workers are quiescent. A callback/worker self-wait
returns `InProgress` and leaves finalization with an external owner. Start is
fenced by a concurrent close request, and callback-origin destruction retains
the shared implementation until the close worker completes. Never reintroduce
a live-worker detach or reinterpret a reporting deadline as permission to
release active backend work.

`ITerminalBackend::WaitForExit` now returns a typed `Exited`, `TimedOut`, or
`Failed` result. ConPTY caches the root process exit code and reports `Exited`
only after both the root and its job-owned descendants have terminated; output
EOF alone is never treated as process exit. `TerminalSessionCallbacks::completed`
fires exactly once after backend close and reader/writer quiescence and
distinguishes natural exit, requested close, and failure. Keep the real exit
code at this post-quiescence boundary.

`CTaskTerminalSessionFactory` is the production adapter. Process tasks preserve
the executable and argument vector. Shell tasks pass through the injected
PowerShell launch policy, which is the only owner allowed to serialize argument
tokens. Cancellation and close remain distinct, and an explicit service-owned
close does not race a second session-exit publication.

The remaining Terminal compatibility seam is presentation ownership. Production
Task sessions currently drain their bounded output queue, but the normal-process
composition supplies no view sink, so Task output is not yet attached to the
existing Terminal tab/model authority. Do not route raw ANSI/UTF-8 chunks into
an unrelated view-local buffer or claim native Task Terminal compatibility
until a runtime-owned terminal presentation service is shared by tasks and the
panel. Variable resolution, dependency/background scheduling, problem matching,
terminal restoration, and full native task UI are also open.
