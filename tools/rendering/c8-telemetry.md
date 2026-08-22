# C8 rendering telemetry and fault gate

The production rendering boundary exposes a fixed-capacity telemetry snapshot
through `FrameCoordinatorRuntime::RuntimeTelemetry()`. The snapshot contains
request-to-publication latency, UI-handler and mutex-acquisition histograms,
bounded mailbox depth/saturation counters, skipped/backpressured presentations,
and independent progress for each admitted surface. Histogram bins are fixed;
the owner never invokes a callback, allocates a sample buffer, or waits to
record an observation.

`FrameCadence` computes the interval from the active display/compositor refresh
rate. It is pure math and does not create a timer. A compositor wake or explicit
input event calls the runtime's `Tick()`; no application-wide 16 ms retry loop
is valid evidence.

`FrameCadenceSource` is the production event-boundary adapter. On the owning
window thread it associates the HWND with `MonitorFromWindow`/
`GetMonitorInfoW`, reads the active target rate through
`GetDisplayConfigBufferSizes`/`QueryDisplayConfig`, and separately samples the
DWM QPC refresh period with `DwmGetCompositionTimingInfo`. A topology or
non-zero-rate change advances `displayEpoch`; a transient query failure keeps
the last known rate and a valid epoch. It does not create a timer, wait for a
vblank, or use a synthetic refresh-rate table. Mixed-rate trials must preserve
the raw `displayRefreshRateHz`, `compositorRefreshRateHz`, and epoch from this
source in the evidence.

`FrameBackpressureController` is the deterministic fault-harness model for
bounded admission. General workers have a fixed capacity, Editor has a reserved
lane, and a `Present` backpressure result leaves only the latest request pending
for a later explicit tick. `FrameFaultModel` covers device-loss injection,
hardware/WARP/software terminal selection, epoch advancement, and close.

## Harness input

Capture a JSON object from a performance/fault trial with these fields:

```json
{
  "controlQueueDepth": 2,
  "maxControlQueueDepth": 64,
  "uiHandlerP99Milliseconds": 1.4,
  "uiHandlerMaximumMilliseconds": 4.2,
  "lockP99Microseconds": 42,
  "lockMaximumMicroseconds": 180,
  "inputToVisibleP95Intervals": 1,
  "inputToVisibleP99Intervals": 2
}
```

Run the bounded validator from the repository root:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/rendering/validate-c8-telemetry.ps1 `
  -InputPath build/results/rendering/c8.json -FailOnThresholds
```

The validator also understands optional `cpuWorkQueueDepth`/
`maxCpuWorkQueueDepth` and `publicationDepth`/`maxPublicationDepth` pairs. It
returns exit code 2 only when `-FailOnThresholds` is supplied and a queue or C8
latency limit fails. It never sleeps, retries, launches an editor, or deletes a
profile; the caller owns capture-process cleanup.

To run the real production runtime clean-idle trial with a bounded process
lifetime, source-derived cadence evidence, and automatic threshold validation:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/rendering/run-c8-production-gate.ps1 `
  -Configuration Debug -RequireExternalClock
```

The command launches the built `tests1.exe` production runtime test, waits ten
seconds with no input/cadence/publication event, and writes stdout, stderr,
raw telemetry, validation, and a summary under
`build/results/rendering/c8-production-<utc>/`. It proves that the exact test
PID exited and fails if a survivor remains. `-RequireExternalClock` rejects a
trial that acquired neither a display nor compositor rate; omit it only when
the platform cannot expose either source and record that limitation. The
default software idle trial intentionally does not claim physical visibility
or native `Present1` content projection. Use `-RequireNativePresentation` only
with a window-integrated native target trial; a zero native-attempt count is
evidence that that path was not exercised, not a pass.

## Required fault cases

The focused `FrameCoordinatorC8TelemetryTest` covers the deterministic core
cases: a stalled surface cannot block a ready sibling, saturated general work
still admits the reserved Editor lane, present backpressure skips without a
busy retry, refresh math for 60/120/144 Hz and mixed refresh, bounded telemetry
percentiles/progress, and device-loss recovery through software and close.

For a native trial, record the same fields before and after each case and keep
the following evidence together: queue capacities, skipped/backpressure count,
last request and publication IDs per surface, device epoch/state, UI handler
p99/max, lock p99/max, and input-to-visible p95/p99 in refresh intervals.

The current clean-idle JSON names the latency field
`inputToVisible*Intervals` for compatibility with the gate schema, but its
software fallback sample is request-to-publication. It must not be presented
as proof that pixels reached a physical display until the native target,
readback, and visible-window capture are connected.
