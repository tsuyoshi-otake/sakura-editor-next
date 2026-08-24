# Frame-coherence verification

`measure-frame-coherence.ps1` turns the repository's
`stale-pixel-verification` protocol into a repeatable local gate. It launches an
isolated Debug editor profile, proves the window is unoccluded, performs a
gesture, and captures the same window through two independent paths:

- `Graphics.CopyFromScreen` reads the pixels currently presented by Windows.
- `PrintWindow(PW_RENDERFULLCONTENT)` asks the editor to render its current
  layout again.

The tool writes both captures, a red difference heat map, and `summary.json`.
Every measurement is corrected by a full-redraw noise-floor capture from the
same process, at the same window geometry and semantic state. A baseline from
the opposite resize width is invalid because `PrintWindow` has stable,
geometry-dependent differences from the composed screen. `PrintWindow` remains a supplementary GDI diagnostic; future
DirectComposition islands also require internal frame/epoch telemetry and GPU
or software readback.

The summary also compares `CopyFromScreen` immediately after the gesture with
`CopyFromScreen` after the full-redraw noise-floor probe. This
`screenStability` measurement directly detects pixels the user actually saw
that changed only after a forced redraw, while keeping `PrintWindow` omissions
reported separately instead of mistaking every native-control limitation for a
presented stale frame.

Use `-PresentedScreenOnly` when a native control's `WM_PRINT` implementation is
known to perturb or omit its own pixels. This mode never calls `PrintWindow` or
forces a redraw: it compares the screen immediately after each User32 gesture
with the naturally settled screen at the same geometry. It is a complementary
presented-frame gate, not a replacement for the independent dual-capture gate.

`-Gesture SideBarResize` finds the real vertical
`SakuraWorkbenchPanelSash`, maps its screen rectangle into the editor client,
and sends the actual `WM_LBUTTONDOWN` / captured `WM_MOUSEMOVE` /
`WM_LBUTTONUP` sequence through User32. With `-PresentedScreenOnly`, the first
capture occurs while the sash is still held and the second after the committed
layout settles, matching the panel-width gesture rather than resizing the
top-level window as a proxy.

When more than one vertical Workbench sash is visible, the probe selects the
one geometrically adjacent to the activated ViewContainer. Before trials it
iteratively normalizes that surface to 320 pixels and rejects the run unless
the measured width converges within one pixel. This prevents the legacy shared
Side Bar extent or an Auxiliary Bar sash from changing the test fixture. The
throwaway editor is foreground/topmost only for the bounded capture interval;
the state is removed during cleanup.

## Repeated resize measurement

Run from the repository root after building `x64 Debug`:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/rendering/measure-frame-coherence.ps1 `
  -DocumentPath .\CLAUDE.md -Trials 20 -FailOnExcess
```

The resize gesture alternates widths in one process. The tool verifies the
requested width before accepting each trial. `-FailOnExcess` exits with code 2
when any trial exceeds the measured noise floor by more than
`-AllowedExcessPercent` (default `0.05`). Do not summarize a run using only its
median; intermittent repaint races are failures too.

Use `-ActivityBarPage Explorer`, `Search`, `SourceControl`, or `Extensions` to activate that
Primary Side Bar ViewContainer through the real Activity Bar child before the
gesture trials. The probe fails when the Activity Bar is missing or the page
activation does not change the visible child layout.

Pass `-WorkspaceFolder <path>` to launch the editor through its real
`-FOLDER=<path>` command-line contract. Use this for Explorer and Source Control
measurements when the populated tree or change list is the subject of the
test; an empty ViewContainer is not an adequate substitute for rows of text.
For Search, combine it with `-SearchQuery <text>`; the probe writes the real
query `EDIT` control and waits for the result surface to settle before resizing.

## Command measurement

Use the Sakura function code rather than a synthesized key chord:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/rendering/measure-frame-coherence.ps1 `
  -Gesture Command -FunctionCode <function-code> -Trials 20 -FailOnExcess
```

The script sends `WM_COMMAND` with a bounded `SendMessageTimeout` and rejects a
trial if the completed handler did not change the visible child-window layout.
Waiting for handler completion is essential: a retained previous frame while a
layout transaction is still executing is not stale output. This also prevents
an unrecognized shortcut or command from producing a false zero-difference
result.

## Safety and cleanup

- Profiles must start with `codex-render-` and are created only below
  `%APPDATA%\sakura\`.
- The exact launched window is closed after the run; only that process tree may
  be terminated after the bounded close timeout.
- Profile deletion uses bounded backoff because the Sakura control process can
  briefly retain `storage-v1.lock` after the editor window closes.
- Any occlusion sample owned by another top-level window invalidates the trial.
- Captures and JSON are written under `build/results/render-coherence/` by
  default and are build artifacts, not source inputs.
