---
name: stale-pixel-verification
description: Dual-capture (CopyFromScreen vs PrintWindow) methodology for proving what the window actually painted. Use whenever a change touches layout, repaint, invalidation, or window movement, or when diagnosing stale-pixel defects (parts vanishing, controls stranded at old coordinates, seams outliving boundaries). Includes the trial protocol, noise-floor rules, and the Issue #17 baseline results.
---

## Verifying What the Window Actually Painted

A screenshot proves that the layout model is right; it does not prove that the
screen shows it. Stale-pixel defects — a part that vanishes, a control stranded
at its previous coordinates, a seam that outlives the boundary that drew it —
are invisible to any check that reads the window's own state, because that
state is already correct. Use this method whenever a change touches layout,
repaint, invalidation, or window movement.

### Capture the same instant two ways

Capture the same window at the same moment through two independent paths and
compare them pixel by pixel:

- `Graphics.CopyFromScreen` over the window rectangle reads the **real
  composited screen**, including whatever stale bits survived.
- `PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT /* 2 */)` forces a **fresh render
  from the current layout**, so it shows what the window would draw if it were
  asked again.

The two agreeing means the screen is current. The two differing means the screen
holds pixels the current layout would not produce — which localizes the defect to
missing invalidation, not to wrong geometry. Save the screen capture, the
PrintWindow capture, and a diff heat map for any trial above the noise floor; the
heat map is what turns a percentage into a diagnosis. In Issue #17 it showed the
welcome action list drawn at two x positions at once and the whole Panel present
in PrintWindow but absent on screen.

### Rules that make the comparison trustworthy

- **Prove the window is unoccluded.** Sample a grid over the window rectangle
  with `WindowFromPoint` + `GetAncestor(GA_ROOT)`; any point owned by another
  top-level window invalidates that trial. Without this, another application's
  pixels are indistinguishable from a repaint bug. Park the cursor off the window
  first so hover highlighting cannot differ between the two captures.
- **Use a throwaway profile per run.** Launch with `sakura.exe -PROF=<name>` and
  delete that profile directory before and after the run. Panel visibility and
  sash extents persist there, so reusing a profile makes run N's end state run
  N+1's start state and silently destroys the A/B comparison.
  **The profile directory is `%APPDATA%\sakura\<name>\`, not a directory beside
  the executable** — the exe-adjacent location is used only when a `sakura.ini`
  already sits next to the executable. Verified 2026-08-07 while investigating
  why an edited setting had no effect: the default profile's `settings.json` is
  `%APPDATA%\sakura\settings.json`, and the `settings.json` and
  `.sakura-platform\` left in `x64\Debug\` are stale artifacts of earlier runs
  that the running editor never reads. Before concluding that a setting is
  ignored, prove which `settings.json` the process actually loaded — change a
  setting with an unmistakable effect (`workbench.colorTheme`) in the candidate
  file and confirm the window changes.
- **Repeat the identical gesture many times in one process.** Stale-pixel
  survival is a paint-timing race: the same binary produced 6.948% on one
  single-trial run and 0.000% on the next. Report the distribution over the
  trials, never one measurement.
- **Repeat the launch when the defect is first-time-only.** Some corruption
  appears only on the first open of a surface, because that is when the content
  windows are created and the race is widest. That case needs N fresh processes
  with one trial each, not one process with N trials.
- **Prove the gesture actually happened.** Read a real property of the window
  before and after — for example the visible `SakuraWorkbenchPanelHost` children
  and their rectangles — and fail loudly if it did not change. A gesture that
  silently did nothing reports a perfect 0.000%.
- **Drive commands through `WM_COMMAND` with the function code, not synthesized
  keystrokes.** `PostMessage(WM_KEYDOWN)` never updates physical key state, so
  `GetKeyState(VK_CONTROL)` stays up and a `Ctrl+`-prefixed keybinding never
  matches; the message is delivered and does nothing. Even `keybd_event` proved
  unreliable here. `PostMessage(hwnd, WM_COMMAND, F_TOGGLE_BOTTOM_PANEL, 0)`
  reaches the same dispatch the keybinding would.
- **Establish a noise floor.** Force a full external
  `RedrawWindow(RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW)`,
  wait, and measure again. Whatever remains is capture-method noise, not a
  defect, and it sets the threshold below which a difference means nothing.
- **A/B against the same tree.** Reverting the changed files to `HEAD` does not
  produce the pre-change binary when the working tree carries other uncommitted
  work — in Issue #17 it failed to compile on a header that only exists in the
  working tree. Disable the change in place with a clearly marked temporary edit,
  measure, then restore from a saved copy.
- **Confirm the measured process exited** and that the throwaway profile is gone
  before reporting results.

### Issue #17 result (2026-08-05, x64 Debug)

| Gesture | Fix disabled | Fix enabled |
|---|---|---|
| First bottom-Panel open, fresh process | 4.031–4.360% stale, 4/4 runs | 0.000%, 6/6 runs |
| Panel open/close, 10 alternating toggles | — | 0.000% median, 0.001% max, 10/10 |
| Frame resize, alternating sizes | 6.456% median, 12.691% max, 12/12 | 0.000%, 7/7 |

`sakura.exe` has a separate, pre-existing tendency to exit with code `-1` after
several rapid frame resizes; it predates this change and produces no Application
Error event. Run the phase under investigation on its own rather than trusting
the tail of a long mixed run.
