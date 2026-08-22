# Terminal workload measurement

`measure-terminal-workload.ps1` runs the C6 terminal workload checks through the
real `tests1.exe` target. It is deliberately bounded: every child has a
timeout, standard output/error are captured to the run directory, and a timed
out child is terminated as an exact process tree before the next workload
starts.

Run it after building the requested configuration:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/measure-terminal-workload.ps1 `
  -Configuration Debug -Samples 1 -TimeoutSeconds 120 `
  -OutputDirectory C:\Users\developer\tmp\sakura-terminal-workload-c6
```

The harness executes the one-million-line model/scrollback and session
backpressure tests, then the worker-retirement and UI-handoff tests. It writes
`terminal-workload-summary.json` and a matching Markdown table, including
elapsed time, exit code, timeout state, finite queue/scrollback gates, static
checks for the removed output frame timer and production detach, and any
repository build/test process survivors. A nonzero exit means at least one
workload, static check, or survivor check failed.

The fixed 16 ms resize coalescing interval in `TerminalSession.cpp` is a
worker-side condition-variable debounce for backend resize requests; it is not
an output-presentation timer and does not make the UI wait. The output path is
message/event driven. Synchronized-output expiry and protocol-input retry
timers remain separate protocol mechanisms.
