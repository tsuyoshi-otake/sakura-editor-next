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

## Workspace Transitions and Native IME

An in-place folder transition is a terminal ownership boundary. Compare the
canonical `workspaceIdentityKey`; aliases of the same folder do not cross the
boundary. When the identity changes, close every old tab/session and discard
split and queued-input state. Recreate exactly one session in the new folder
only when the Terminal View was visible before the transition. A hidden
Terminal, or a Panel showing Problems/Output, stays process-free until Terminal
is explicitly revealed. Never let a close or replacement-launch failure roll
back an already committed workspace context.

This is an explicit interim divergence from VS Code's workspace-scoped terminal
persistence. Sakura Editor NEXT does not yet serialize/reconnect terminal
processes, history, tabs, or splits per stable workspace/window identity. Do not
retain a live session from the previous workspace or describe the replacement
behavior as restoration; full per-workspace restoration remains open.

The native terminal owns Unicode IME commit delivery. Handle
`WM_IME_COMPOSITION`/`GCS_RESULTSTR` explicitly and encode the complete UTF-16
result once as UTF-8. Keep `WM_IME_CHAR` as the fallback for IMEs that use it,
and do not depend on `DefWindowProc` synthesizing `WM_CHAR`: that behavior varies
between IMEs and native full-screen TUI states. Session rebinding must cancel
composition and discard partial surrogate/backpressured input so old text can
never enter a replacement session.

## DirectWrite Damage Rendering

Terminal render-plan rectangles use absolute client coordinates. Bind the
`ID2D1DCRenderTarget` to the stable full client/back-buffer rectangle even when
Win32 reports a one-row `PAINTSTRUCT::rcPaint`; keep the render plan, HDC clip,
and final `BitBlt` limited to the dirty rectangle. Binding DirectWrite only to a
non-zero dirty row changes the target geometry and can clip shaped fallback
glyphs (notably Japanese) until a later click, move, or full repaint. Preserve
the pixel regression that draws Japanese below row zero and immediately copies
only that damaged row.

## Clipboard and Selection Interaction

On Windows, the native terminal follows the VS Code/Windows Terminal host
selection convention: with terminal mouse reporting disabled, right-click copies
and clears a non-empty selection, while right-click with no selection pastes
Unicode clipboard text through the existing bracketed-paste encoder. When a TUI
has enabled mouse reporting, preserve the application's right-click event;
`Shift` explicitly opts into host selection/clipboard handling. Selection ranges
are stored as half-open cell intervals, but mouse endpoints must be normalized to
include both drag endpoints and the complete continuation cells of wide graphemes
before painting or extracting clipboard text.
