# Integrated terminal loop diagnostics

Issue: #276

This trace is intended to separate fresh PTY output from a terminal model,
scrollback, viewport, or paint loop. It is opt-in and metadata-only: terminal
bytes are represented by byte counts and SHA-256 digests. Raw terminal content,
launch arguments, and working-directory text are not written.

## Start tracing after the loop appears

Use the PID of the editor process that owns the affected integrated terminal:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\enable-terminal-trace.ps1 `
  -TargetProcessId 25416 `
  -Destination '\\asus\G\terminal-loop-traces'
```

The running process checks the named activation event at most once per second
on terminal activity. The signal enables every integrated terminal session in
that editor process. Existing sessions keep lightweight cumulative counters
before activation, so the trace header still reports session age, total PTY
traffic, total scrollback append/eviction counts, and the first/last observed
eviction times. Detailed event history begins only after activation.

If `-Destination` is omitted, output goes under
`%TEMP%\sakura-editor\terminal-traces`. Each session writes
`terminal-pty-<pid>-<session>.jsonl` plus at most one `.previous.jsonl` file.
Each file is capped at 8 MiB, so one session retains at most about 16 MiB.

## Start tracing before reproduction

Set the output directory before launching the editor:

```powershell
$env:SAKURA_TERMINAL_TRACE_DIR = '\\asus\G\terminal-loop-traces'
.\x64\Debug\sakura.exe
```

This provides the complete event sequence from session construction. Remove the
environment variable for normal runs.

## Interpretation

Correlate events by `mono_us` and compare `sha256` values:

| Observation | Supports |
|---|---|
| Repeated `pty_read` hashes and byte counts | Agent/child process is re-emitting content |
| `protocol` `pty_write` immediately precedes matching output bursts | DSR/DA/window-size feedback |
| Repeated `resize_request`/`resize_apply` precedes output bursts | Resize feedback |
| No new `pty_read`, but `model_publish`/`viewport_publish` repeats | Sakura model/viewport publication loop |
| `scrollback_evicted` grows with stable `scrollback_rows == scrollback_limit` | Long-running bounded-history workload is involved |
| Dropped sequence gaps or nonzero `dropped_before` | Diagnostic writer could not keep up; timing conclusions need caution |

A 1000-line limit is treated as an amplifier, not proof of root cause. It makes
the session enter continuous eviction earlier. The relevant evidence is whether
the failure follows cumulative eviction/anchoring activity, fresh PTY bytes,
protocol replies, or resize events.
