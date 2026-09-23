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

Pass `-SideBarCapturePhase AfterRelease` to capture immediately after the sash
button-up instead. Run both phases when diagnosing a resize: the default
`DuringDrag` phase finds transient pixels during movement, and `AfterRelease`
isolates stale pixels left after the gesture commits. The two runs have
different capture boundaries and must not be compared as if they measured the
same frame.

For a held-drag survey, use `-SideBarCapturePhase DragSequence` without
`-PresentedScreenOnly`. Each trial moves the real sash through four 18-pixel
steps, captures the presented frame immediately and after a 300 ms quiet
interval at each width, and records sash, Part host, and page rectangles with
timestamps. The `immediateTreeViews` and `stableTreeViews` records capture each
native TreeView's visibility, `SysSetRedraw` suppression, item count, and bounds
beside those images. They also record `GetUpdateRect` and the HWND returned by
`WindowFromPoint` at the tree center. This separates a delayed paint from a
control that became invisible, lost its model, or was covered by a sibling. It
rejects a step if the host width does
not move in the requested direction, changes while held, or the physical cursor
leaves the requested position. `heldSideBarMeasurement` crops the Part host to
locate a difference;
its pixel count is compared with the same absolute full-frame budget, so a
smaller crop does not silently tighten the gate. At the final held width the
tool also compares `CopyFromScreen` with `PrintWindow` before and after a full
redraw, and reports the screen change after button-up separately. A run that
fails the input or occlusion checks has no accepted result: `incomplete.json`
records the completed steps, error, and process exit status, while
`summary.json` is absent. Repeat it when the desktop is idle.
Each complete step also writes `trial-NNN-step-NN-metadata.json` immediately,
so a later interruption does not discard its control-state evidence.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/rendering/measure-frame-coherence.ps1 `
  -DocumentPath .\CLAUDE.md -WorkspaceFolder . -ActivityBarPage Explorer `
  -Gesture SideBarResize -SideBarCapturePhase DragSequence -Trials 20 -FailOnExcess
```

Odd trials widen and even trials narrow the Primary Side Bar, giving ten
trials per direction. Inspect `heldSteps`, `failedSideBarStepCount`,
`failedRedrawTrialCount`, and `failedDualTrialCount` in `summary.json`, then
inspect the corresponding heat maps. A changed editor caret or title outside
the Side Bar is recorded in `heldScreenMeasurement` but is not evidence of
Side Bar flicker.
If a long run ends when the throwaway editor exits, use two complete ten-trial
runs with fresh processes. Do not count images from an incomplete trial as an
accepted run; they remain diagnostic evidence only.

In the 2026-09-23 Issue #311 survey, a same-geometry held step captured native
Outline rows in `CopyFromScreen` immediately after the move, then a blank
Outline 300 ms later. The native TreeView remained visible with 11 items and
`SysSetRedraw` unset at both captures. `PrintWindow` still rendered the rows,
and a full synchronous frame redraw restored them without changing geometry.
Treat this as a presented-pixel failure even though the model and HWND state
are correct. Removing the second queued whole-frame invalidation from the
mouse-move handler did not eliminate the failure in a complete 10-trial run;
that experiment was reverted. The capture establishes the symptom and rejects
model loss and redraw suppression, but it does not yet identify which native
paint or composition step clears the pixels.

A separate six-trial Explorer drag on 2026-09-23 reproduced one blank step with
both TreeViews visible, populated, uncovered at their centers, and holding a
pending full-client update region. Calling `UpdateWindow` on those two native
TreeViews at that same held width restored every row, changing 4.037% of the
Side Bar crop without moving any window. Use `-ProbePendingTreePaint` only for
this diagnostic: it performs the targeted update after the stable capture on
steps 1-3 when the Side Bar changed by more than 0.5%. Leave it off for an
unperturbed acceptance run. A pending update region also appears on some
correct frames, so `GetUpdateRect` alone is not a failure detector.

The Explorer page wrapper has no background paint and clips the host-owned
header with a window region. Removing its `WS_EX_TRANSPARENT` extended style
is a candidate correction for the delayed native child presentation. In a
controlled lower-monitor comparison, the unchanged wrapper had two broad
Outline blanks in 40 held steps (4.309% and 4.037% of the Side Bar crop).
The wrapper without that style had no broad Outline blank in 40 steps, but
the strict Side Bar gate still failed twice: one Explorer row appeared or
disappeared between captures (0.344% and 0.294%). TreeView item counts were
unchanged. Another ten-trial run without the style had zero strict Side Bar
failures; this does not erase the two row-level failures in the controlled run.
Inspect heat maps and before/after images instead of treating equal failure
counts as equal symptoms. The style change is a partial improvement, not a
verified complete fix.

The panel host also sent `WM_SETFONT` with redraw enabled to every descendant
on every sash layout, even when a native control already held that font. The
guard in `ApplyChromeFont` avoids that redundant repaint; its repeated-layout
and DPI-change contract is covered by `WorkbenchPanelHost` tests. With both
the style change and font guard, two complete ten-trial lower-monitor drags
each had zero strict Side Bar failures and zero changed Side Bar pixels in
40 held steps. Both TreeViews remained visible with 212/11 items throughout.
The intervening 20-trial attempt stopped on trial 16 because the physical
cursor differed from the requested position by three pixels; its
`incomplete.json` and step captures are diagnostic only, and are not included
in those 80 accepted steps. The whole-frame gate still reported changes
outside the Side Bar (9 and 7 steps respectively), so use the Side Bar crop
for this Issue. This establishes repeatability on the tested monitor and
fixture; it does not claim that every desktop configuration is flicker-free.

The broader Issue #311 survey used the same lower monitor before the final
font guard. With the synchronous Outline-collapse redraw, ten Outline toggles
had no presented-screen or Side Bar failures; a separate ten-trial dual-capture
run also had zero failures. Ten Explorer/Search Activity Bar switches had one
full-frame difference outside the Side Bar and no Side Bar difference. Five
Explorer resizes and five Search resizes had respectively two and one
full-frame differences, with no Side Bar difference. Five Source Control
resizes had no failures under the configured absolute-pixel budget. The
Projects fixture did not start successfully, so that page is unmeasured. These
results check the other requested gestures but do not substitute for a repeat
of each gesture on the final font-guard build.

## Scrollbar and title follow-up for Issue #311

The shared overlay scrollbar uses VS Code's 10-DIP list track and full-width
slider, with a 20-DIP minimum slider length. Clicking the empty track centers
the slider at the pointer and starts a drag. Verify the behavior with a
populated Explorer tree: rotate the wheel over the tree and over the overlay,
click near the track bottom, drag the slider, then inspect immediate and
300-ms-later captures of the title, tree, and entire 10-pixel bar. Both wheel
locations must move the first visible row; clicking the track must jump toward
the pointer; dragging must move toward the opposite end. The bar must keep its
rectangle, stay above the tree in `WindowFromPoint`, and show only the track
and slider, never tree text. A pressed middle button plus pointer movement is
a separate gesture from rotating the wheel; do not infer one from the other.

The first 11-gesture lower-monitor run exposed tree text in the bar after
scrolling. The Explorer TreeView was created without `WS_CLIPSIBLINGS`, so it
could paint over its later overlay sibling. Adding that style gave zero
immediate-to-settled pixel differences in the title, tree, and bar over the
same 11 gestures. Every bar rectangle remained fixed and `WindowFromPoint`
returned the overlay. The same sibling-clipping rule was applied to Search,
SCM, and SENP tree controls that share the overlay; their native tests and
solution build pass, but they were not each measured on screen with populated
scrollable lists at that point in the survey. The populated follow-up below
separates scroll behavior from resize paint.

On 2026-09-23, the lower-monitor 11-gesture checks used 102 Search rows
(22 visible) and 44 SCM rows (10 visible). In both pages, wheel input over
the list and over the overlay moved the first row from 0 to 3; a track click
and thumb drag moved it to the last viewport (Search 80, SCM 35). Each gesture
kept the overlay hit target and rectangle, and immediate versus 300-ms-later
title, list, and bar crops differed by zero pixels. The Search list initially
ignored wheel input because its native vertical bar is hidden under the
overlay; `CSearchWorkbenchTool` now routes wheel deltas to its list position,
including fractional wheel deltas. Pressing the middle button and dragging
changed no row in either list. Wheel rotation works; middle-button autoscroll
does not. The SENP tree probe passed 72 visual and 18 scroll trials across
three themes and three DPIs, with process cleanup confirmed.

The populated Search resize exposed a separate native child paint race. With
102 rows, the immediate after-release capture sometimes showed a blank result
list; the settled capture showed the rows at the same geometry. The first
canonical run failed in both of two trials, and additional ten-trial runs
reproduced the blank intermittently. Calling `UpdateWindow` on the Search list
after the immediate capture restored the rows without moving the window.
Reordering frame invalidation, painting on release, painting from a posted
message, changing list resize redraw flags, and suppressing other redundant
paints did not remove the failure across ten-trial runs, so those experiments
were reverted. The Search root now uses `WS_EX_COMPOSITED` to present its
owner-drawn list and overlay as one child cohort. Two complete lower-monitor
ten-trial repeats with the same 102-row fixture had zero Side Bar failures and
zero changed Side Bar pixels. The full-frame gate saw one and two differences
outside the Side Bar respectively. The accepted captures are under
`build/results/render-coherence/sidebar-search-composited-52/` and
`build/results/render-coherence/sidebar-search-composited-repeat-53/`; the
baseline is `sidebar-search-canonical-smoke-32/`. The repeated 11-gesture
Search scrollbar check after this change again had zero title, list, and bar
differences and retained wheel, track, and thumb movement; see
`sidebar-search-composited-scrollbar-54/`. These measurements cover this
fixture and monitor, not every desktop configuration.

Reviewing the whole-frame heat maps also exposed a title-bar paint gap:
one of those twenty immediate captures showed a bare title background before
its menu, caption, and buttons appeared. The custom frame had painted the
background and glyphs as separate GDI operations while the parent filled the
same title strip first. The parent now leaves that strip to the frame, and the
frame renders the title and menu into a compatible bitmap before one `BitBlt`.
Three further complete ten-trial populated Search resize runs had no blank
title and no Side Bar failures. Each run had one whole-frame difference caused
by the title's active/inactive text color changing between the immediate and
settled captures; the title text and buttons remained visible in both images.
The captures are in `sidebar-search-title-buffer-55/`,
`sidebar-search-title-buffer-repeat-56/`, and
`sidebar-search-title-buffer-third-57/`. The final 11-gesture scrollbar rerun
also had zero immediate-to-settled differences in its title, list, and bar
crops; see `sidebar-search-title-buffer-scrollbar-58/`.

The visual check covers the track and slider geometry, palette roles, overlap,
and stability during gestures; it does not establish complete VS Code design
parity. Upstream
[`ScrollableElement`](https://github.com/microsoft/vscode/blob/0896e62ebab6f42f24240a9ad72ab3147f7b297e/src/vs/base/browser/ui/scrollbar/scrollableElement.ts)
defaults both axes to `Auto`, reveals them on hover or scroll, and schedules
hiding after 500 ms. Its
[CSS](https://github.com/microsoft/vscode/blob/0896e62ebab6f42f24240a9ad72ab3147f7b297e/src/vs/base/browser/ui/scrollbar/media/scrollbars.css)
fades the invisible bar. The current native `COverlayScrollbar::Update()` leaves
a scrollable bar painted continuously. Treat automatic visibility and its
animation as an open behavioral difference in Issue #311, and measure it
separately before changing the shared control: the overlay is used by several
native views, and visibility changes must preserve wheel routing, track hit
testing, and stable paint.

For title-band diagnosis, use `DragSequence` and inspect the top 34 pixels in
each failed full-frame step. A blank title can coexist with a perfect Side Bar
crop. A redundant redraw candidate still produced blank title frames in three
of 40 held steps, so it was reverted. The sash `WM_MOUSEMOVE` handler now skips
layout when the clamped pending width has not changed. Two complete ten-trial
runs of that build had zero blank-title frames, zero strict Side Bar failures,
and zero post-redraw and dual-capture failures in 80 held steps. Each run had
one full-frame excess: one was an active/inactive title-colour transition, the
other a divider-line change. Those two captures are not evidence of a blank
title, but they keep the strict whole-frame gate above zero.

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

Use `-ActivityBarPage Projects`, `Explorer`, `Search`, or `SourceControl` to activate that
Primary Side Bar ViewContainer through the real Activity Bar child before the
gesture trials. The probe fails when the Activity Bar is missing or the page
activation does not change the visible child layout.

For a repeatable Side Bar survey, use `-PresentedScreenOnly` and keep the
window unoccluded. `-WindowLeft`, `-WindowTop`, `-WindowWidth`, and
`-WindowHeight` place the isolated editor on a chosen monitor; the tool rejects
a rectangle outside the virtual desktop. Run `SideBarResize` with each populated
page, `ActivityBarSwitch` for Explorer/Search alternation, and `OutlineToggle`
with `-ActivityBarPage Explorer`. The latter clicks the real Outline header and
alternates collapse and expansion; report the two directions separately. A
failed page fixture is unmeasured, not a passing repaint result. Read the
immediate/settled captures and heat map to distinguish Side Bar pixels from
title-bar or editor activation repaints.

For example, after building the Debug editor, measure ten Outline transitions:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/rendering/measure-frame-coherence.ps1 `
  -DocumentPath .\CLAUDE.md -WorkspaceFolder . -ActivityBarPage Explorer `
  -Gesture OutlineToggle -PresentedScreenOnly -Trials 10 -FailOnExcess
```

## Side Bar acceptance checklist

1. **Fixture:** Verify that every requested gesture produces `summary.json`
   with the requested trial count. Expect the real target child to change
   geometry or visibility on every trial; an activation timeout has no result.
2. **Presented pixels:** Verify at least ten transitions total, covering both
   directions, with
   `-PresentedScreenOnly -FailOnExcess`. Expect no trial above the configured
   `0.05%` full-frame limit, then inspect any surface crop and heat map to
   identify which control changed. During-drag and after-release resize runs
   answer separate questions.
3. **Independent capture:** Verify the same gesture without
   `-PresentedScreenOnly` and inspect `maximumExcessOverNoiseFloorPercent`
   and `screenStabilityMaximumPercent`. Expect both at or below `0.05%`;
   `PrintWindow` differences already present in the same-geometry noise floor
   do not count as new stale pixels.
4. **Cleanup:** Verify no probe-owned `sakura.exe` or `tests1.exe` survives and
   restore any foreground window moved for the unoccluded capture. Expect the
   user's desktop arrangement to be the same after the run.

Pass `-WorkspaceFolder <path>` to launch the editor through its real
`-FOLDER=<path>` command-line contract. Use this for Explorer and Source Control
measurements when the populated tree or change list is the subject of the
test; an empty ViewContainer is not an adequate substitute for rows of text.
For Search, combine it with `-SearchQuery <text> -MinimumSearchRows <count>`;
the probe writes the real query `EDIT` control through bounded `WM_SETTEXT`,
reads the control text back, and waits for at least that many stable result
rows before resizing. `-ProbePendingSearchPaint` is diagnostic only: after an
immediate Search resize capture it updates the list and records a second
capture, revealing whether a pending native paint can restore missing rows.

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
