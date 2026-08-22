# Markdown preview live-resize verification

`measure-markdown-preview-resize.ps1` is the repeatable User32/GDI probe for
Markdown preview resize, repaint, scrollbar ownership, cancellation, and native
window cleanup. It launches an isolated profile, drives the product through
window messages, and never relies on the application's internal layout state as
proof of what was painted.

## Run

Build the executable first, then run from the repository root:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/measure-markdown-preview-resize.ps1 `
  -ExePath x64/Debug/sakura.exe `
  -DocumentPath README.md `
  -OutputDirectory "$env:TEMP/sakura-markdown-resize" `
  -SampleCount 60
```

The target rectangle must be fully visible. Use `-WindowX`, `-WindowY`,
`-WindowWidth`, and `-WindowHeight` when another top-level window or the taskbar
would cover the default 1280 by 800 rectangle. An occluded trial fails instead
of turning another application's pixels into a false repaint defect.

`WM_MOUSEMOVE` always keeps the default five-second message timeout. Use
`-CommitTimeoutMilliseconds` when a deliberately large document needs a longer
bounded mouse-up observation; `summary.json` records that one committed reflow
separately as `commitMilliseconds`.

To exercise a pathological single-block document without maintaining a large
fixture in the repository, pass `-GeneratedGiantParagraphCharacters <count>`.
The probe writes the generated Markdown into its isolated output directory and
records the requested character count in `summary.json`.

Run probes for one executable serially. The script enforces this with a named
mutex because its before/after PID ownership check cannot distinguish two
simultaneous runs of the same binary; allowing that would make one probe eligible
to capture or terminate the other's window.

## What it proves

For every drag sample the probe:

- sends the native preview command and mouse messages with a bounded
  `SendMessageTimeout`;
- proves that the `SakuraMarkdownPreview` rectangle actually changed;
- records the message duration and reports median, p95, and maximum;
- checks that the preview has no `WS_VSCROLL` and has exactly one aligned
  `SakuraWorkbenchOverlayScrollbar` sibling;
- compares `Graphics.CopyFromScreen` with
  `PrintWindow(PW_RENDERFULLCONTENT)` for both the full frame and the preview
  child, using a forced-redraw baseline as the noise floor;
- writes screen, fresh-render, and heat-map PNGs for diagnostic frames;
- starts a second drag, sends `WM_CANCELMODE`, and proves that the committed
  width is restored;
- hides and reopens the preview, then directly destroys its HWND and proves that
  its sibling overlay is destroyed too.

The preview-child comparison is authoritative for preview stale pixels. On some
Windows configurations a top-level `PrintWindow` does not reproduce child
surfaces, so the full-frame comparison can contain stable non-client or child
composition noise even when the preview comparison is exactly zero.

The probe deliberately does not call `DwmFlush`. It is not evidence of physical
scanout and has previously blocked for about 23 seconds on a large or inactive
window. The gesture handler's synchronous `RedrawWindow(...RDW_UPDATENOW)` and
the independent screen/fresh-render captures are the relevant observations.

The script always records `summary.json`, removes only its generated
`%APPDATA%\sakura\codex-md-resize-*` profile, terminates only processes started
from the supplied executable during that run (parents first), and reports any
survivors. A timeout, occlusion, missing gesture, failed cancellation, orphaned
overlay, surviving process, or profile-cleanup failure is a failed run.

## Acceptance

For a performance-sensitive change, run at least 60 alternating samples in one
process and compare against the same binary/input/window geometry. Expect:

- `terminalState` is `completed` and all requested samples completed;
- `previewGeometryChanged` is true;
- `previewFramesAboveNoiseFloor` and `leakedNativeScrollbarFrames` are zero;
- `messageMilliseconds.p95` and `commitMilliseconds` meet the thresholds chosen
  before the change;
- every sample has `previewOverlayCount == 1`;
- `cancelProbe.transientGeometryChanged` and
  `cancelProbe.committedGeometryRestored` are true;
- `scrollbarLifecycle.destroyedPreviewOverlaySurvivors == 0`;
- `survivingRunOwnedProcesses` is empty and
  `profileDirectoryExistsAfterCleanup` is false;
- the performance threshold chosen before the change is met.

Keep the JSON and heat maps under `.codex/goal-loop/<task>/` when the run is part
of a correction loop. Report the distribution, not one especially good frame.
