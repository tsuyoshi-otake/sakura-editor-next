# Sakura Editor NEXT Claude Code Guidance

## Highest-Priority Rule: Real VS Code Compatibility, Not a Look-Alike

**This is the single most important rule in this repository. It outranks every
other convenience, shortcut, or local habit. When any other guidance conflicts
with it, this rule wins.**

The goal of this project is genuine behavioral compatibility with Visual Studio
Code — not a visual imitation of it. Looking like VS Code is never sufficient
and is never the acceptance criterion.

Every workbench change must satisfy all of the following:

- **Model the real VS Code concept, not the pixels.** Parts (Activity Bar,
  Primary Side Bar, Panel, Secondary Side Bar / Auxiliary Bar, Editor),
  ViewContainers, and Views are distinct layers with distinct lifetimes. A View
  nested in the Primary Side Bar is never the same thing as a physical Part, and
  a control that merely looks like a VS Code control must invoke the same
  concept VS Code invokes.
- **Use VS Code's own stable identifiers.** Command IDs
  (`workbench.action.toggleAuxiliaryBar`), Part IDs
  (`workbench.parts.auxiliarybar`), ViewContainer IDs
  (`workbench.view.extensions`), and View IDs must match upstream VS Code
  exactly. Do not invent a parallel naming scheme for a concept VS Code already
  names.
- **Match VS Code's placement, defaults, and keybindings.** Example: the
  Activity Bar containers (Explorer, Search, Source Control, Run and Debug,
  Extensions) live in the **Primary Side Bar**; the Secondary Side Bar is empty
  by default; `Ctrl+B` toggles the Primary Side Bar, `Ctrl+J` the Panel, and
  `Ctrl+Alt+B` the Secondary Side Bar. If this repository's behavior differs
  from real VS Code, this repository is wrong.
- **Verify against real VS Code before implementing.** When the correct behavior
  is uncertain, check what VS Code actually does — its command palette entry,
  its default keybinding, its container location, its empty/failure state — and
  implement that. Do not guess and do not rationalize a divergence because it is
  easier to build.
- **Never fake a capability to make a screenshot look right.** A surface that is
  not implemented must remain an explicit, typed unsupported boundary that fails
  closed. Approximating a VS Code capability with unrelated legacy state
  (toggling some other view, reusing a legacy active-tool flag, drawing a
  disabled placeholder) is a defect, not progress.
- **Divergence requires a written reason.** If a platform constraint makes exact
  VS Code behavior impossible, record the constraint and the chosen behavior in
  the owning subsystem's `CLAUDE.md`. An undocumented divergence is a bug.

## Repository Boundary: Fork-Only by Default

This checkout is `tsuyoshi-otake/sakura-editor-next`. Upstream repositories are
reference material only unless the user explicitly authorizes work against them.

- Do not create, modify, close, comment on, or label upstream Issues or Pull
  Requests.
- Do not fetch from, pull from, push to, merge, tag, release, create branches,
  or change remotes in an upstream repository.
- Do not infer upstream authorization from requests to research VS Code, compare
  repositories, synchronize code, or continue a fork-side implementation. Those
  requests authorize read-only reference work at most.
- When explicit upstream authorization is given, confirm the exact repository
  and operation first, then keep fork-side and upstream-side changes separate.
  Never omit an explicit `--repo`/repository target when using a hosted-repo
  CLI, and never assume a remote name identifies the intended repository.

## Scope

This file contains repository-wide guidance. More specific `CLAUDE.md` files are loaded when Claude works in their subtrees; follow the nearest file and keep local details out of this root file.

| Area | Scoped guidance |
|---|---|
| Core application and MSBuild projects | [`sakura_core/CLAUDE.md`](sakura_core/CLAUDE.md) |
| Command dispatch | [`sakura_core/cmd/CLAUDE.md`](sakura_core/cmd/CLAUDE.md) |
| Document model | [`sakura_core/doc/CLAUDE.md`](sakura_core/doc/CLAUDE.md) |
| Process composition | [`sakura_core/_main/CLAUDE.md`](sakura_core/_main/CLAUDE.md) |
| Platform services | [`sakura_core/platform/CLAUDE.md`](sakura_core/platform/CLAUDE.md) |
| Configuration, Settings, and workspace sources | [`sakura_core/config/CLAUDE.md`](sakura_core/config/CLAUDE.md) |
| Workbench, editor, working copy, and layout state | [`sakura_core/workbench/CLAUDE.md`](sakura_core/workbench/CLAUDE.md), [`sakura_core/workbench/editor/CLAUDE.md`](sakura_core/workbench/editor/CLAUDE.md), [`sakura_core/workbench/layout/CLAUDE.md`](sakura_core/workbench/layout/CLAUDE.md) |
| Filesystem resource/version boundary | [`sakura_core/platform/filesystem/CLAUDE.md`](sakura_core/platform/filesystem/CLAUDE.md) |
| Self-update and the installer relaunch contract | [`sakura_core/update/CLAUDE.md`](sakura_core/update/CLAUDE.md) |
| Terminal and debug capability boundaries | [`sakura_core/terminal/CLAUDE.md`](sakura_core/terminal/CLAUDE.md), [`sakura_core/debug/CLAUDE.md`](sakura_core/debug/CLAUDE.md) |
| Extension integration and extension host | [`sakura_core/extension/CLAUDE.md`](sakura_core/extension/CLAUDE.md), [`src/exthost/CLAUDE.md`](src/exthost/CLAUDE.md) |
| Build generation and shared resources | [`src/main/CLAUDE.md`](src/main/CLAUDE.md) |
| Tests and test infrastructure | [`src/test/CLAUDE.md`](src/test/CLAUDE.md) |
| Build helpers and build documentation | [`tools/CLAUDE.md`](tools/CLAUDE.md) |
| Language resource DLLs | [`sakura_lang/CLAUDE.md`](sakura_lang/CLAUDE.md) |
| CI workflows | [`.github/CLAUDE.md`](.github/CLAUDE.md) |

## Project Overview

Sakura Editor NEXT is a Windows text editor written in C++20 and licensed under the zlib License. Windows with MSVC is the primary build path. MinGW support is experimental, and its binaries may not behave correctly.

This repository is a fork of [sakura-editor/sakura](https://github.com/sakura-editor/sakura), which is a separate upstream project. Do not treat upstream's issues, pull requests, releases, conventions, or CI as this repository's own, and do not send changes from this repository to upstream unless the task explicitly calls for it.

The application uses two process types: one hidden control process owns cross-instance state, and each editor window runs in its own editor process. Detailed ownership and subsystem boundaries live under `sakura_core/`.

Third-party code lives under `externals/`, mostly as Git submodules. Treat it as upstream code: change it only when the task explicitly requires an upstream or integration update.

## Build Entry Points

Run build commands from the repository root.

| Goal | Command | Scope |
|---|---|---|
| Fast edit/compile loop | `build-dev.bat x64 Debug` | App project only; skips `tests1` evaluation and build |
| App and unit-test build | `build-sln.bat x64 Debug` | `sakura.sln` (`sakura` and `tests1`) |
| Distribution build | `build-all.bat x64 Release` | App, tests, help, installer, ZIP, and assembly artifacts |
| Experimental MinGW build | `build-gnu.bat MinGW Debug` | CMake/MinGW targets and tests |

- Set `NUM_VSVERSION=16` to select Visual Studio 2019 or `NUM_VSVERSION=17` for Visual Studio 2022. `ARG_VSVERSION` is internal to `find-tools.bat`; do not expose it as the user-facing setting.
- Normal MSVC builds do not generate `.asm` listings. `build-all.bat` and distribution CI enable them explicitly.
- x64 Release distribution builds produce one AVX-baseline `x64/Release/sakura.exe`. At process startup it selects isolated AVX, AVX2, or AVX-512F/BW implementations once from CPUID and XGETBV state, independent of Intel/AMD vendor.
- Use `SKIP_CREATE_GITHASH=1` only when a comparison requires stable generated version data.
- Build behavior and environment variables are documented in `tools/build.md`, `tools/build-batchfiles.md`, and `tools/build-envvars.md`; update those files when the interface changes.

## Verification

- `build-dev.bat` verifies the application build only. Use `build-sln.bat` before running `tests1.exe` or claiming that test targets build.
- `tests1.exe` contains UI and integration suites. A full local run can launch visible editor windows using an isolated/default profile. Read `src/test/CLAUDE.md` before unattended test execution.
- Match verification to the affected build path: test both Debug and Release for MSBuild orchestration changes, and test MinGW when changing shared CMake logic.
- Run static analysis with `run-cppcheck.bat <Platform> <Configuration>` when the change warrants it.
- `requirements.txt` is a hash-locked, `--no-deps` install list, not a normal pinned requirements file: every CI workflow installs it with `uv pip install --require-hashes --no-build --no-deps -r requirements.txt`. `--no-deps` is load-bearing — it stops pip/uv from resolving each package's own transitive dependencies, so `requirements.txt` only needs a hash entry for the packages it lists directly (e.g. `chardet`), not for something like `pytest`'s Windows-only `colorama` dependency. Installing it locally with a plain `pip install -r requirements.txt` (no `--no-deps`) makes pip resolve the full dependency tree, hit an unhashed transitive package under `--require-hashes` mode, and fail with "these do not [have pinned hashes]" for a package that was never meant to need one. When reproducing a Python-tooling CI check locally (e.g. `src/main/py/check_encoding.py`), install with the exact same flags the workflow uses, in a task-specific venv — not a bare `pip install -r requirements.txt`.

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

## Startup Performance Baseline

- Measure startup changes with [`tools/measure-startup-performance.ps1`](tools/measure-startup-performance.ps1) and the committed, non-confidential [`tools/startup-benchmark-sample.md`](tools/startup-benchmark-sample.md). Follow [`tools/startup-performance.md`](tools/startup-performance.md) for the environment controls, milestone definitions, output schema, and cleanup checks.
- Compare medians from at least five successful runs under the same binary, input, profile condition, cache state, power mode, and background load. Treat the measurements as a machine-specific comparison baseline, not a performance guarantee.

The historical reference below was measured on 2026-07-30 with the warm OS file cache and no Sakura process running before each trial. It used five runs per condition on Windows 11 Pro build 26200, AMD Ryzen 7 9700X (8 logical processors), and 61.6 GiB RAM. The target was `x64/Release/sakura.exe`, file version `2.4.3.6974`, product revision `6c120e85`, 2,734,080 bytes, built 2026-07-29 21:55 JST. The input was the uncommitted `Desktop\社内規定.md`, 1,249,037 bytes and 5,604 lines; its confidential contents must not be copied into this repository.

| Milestone | Fresh profile median (range), ms | Existing profile median, ms | Fresh minus existing, ms |
|---|---:|---:|---:|
| Process API returned | 3.638 (3.515–5.570) | 3.585 | +0.053 |
| Top-level window created | 92.718 (91.269–111.864) | 92.195 | +0.523 |
| Window visible | 123.943 (122.016–127.436) | 109.664 | +14.279 |
| DWM flushed | 134.331 (128.914–137.731) | 124.971 | +9.360 |
| Document title ready | 154.882 (136.967–158.799) | 138.555 | +16.327 |
| Input idle | 154.965 (137.248–161.083) | 141.586 | +13.379 |

`DwmFlush` is a compositor synchronization proxy, not proof that a physical display scanout completed. `documentTitleReady` externally observes the caption update that follows file loading and layout setup; it is not a first-content-pixel measurement. `WaitForInputIdle` is not the application's internal first-idle event.

The first reproducible committed-sample baseline was run on 2026-07-30 under the same machine and binary with the warm OS file cache (`runId` `20260730-082955-726-c5aebbeb`). All five runs in each condition succeeded, both screenshots showed the requested Markdown, and every run verified process and profile cleanup. `existingProfile` here means the second launch of the same generated profile within each iteration, not the developer's normal profile.

| Milestone | Fresh median (range), ms | Existing-profile median (range), ms | Fresh minus existing, ms |
|---|---:|---:|---:|
| Process API returned | 3.293 (3.136–6.121) | 2.036 (1.901–2.452) | +1.257 |
| Top-level window created | 95.335 (57.846–117.501) | 89.641 (71.091–102.731) | +5.694 |
| Window visible | 126.300 (109.593–156.176) | 126.925 (121.542–134.425) | -0.625 |
| DWM flushed | 135.776 (116.447–160.804) | 134.733 (127.473–147.754) | +1.043 |
| Document title ready | 136.896 (118.926–173.154) | 171.026 (140.900–185.537) | -34.130 |
| Input idle | 184.562 (158.744–196.553) | 190.815 (161.811–207.549) | -6.253 |

### 2026-07-30 Release optimization and ISA comparison

The x64 Release optimization/ISA validation used the same current source (`2.4.3.7163`, revision `5a910555`) and the warm OS file cache on the machine above. The confidential input remained outside the repository at `Desktop\社内規定.md`: 1,249,037 bytes, 3,604 physical lines, SHA-256 `8d45e72d30a2294a4d8ae5b9c82c1df424af2fb1c2deeb36264611d6ac5f5109`. Its 5,605 scrollbar maximum represents approximately 5,604 wrapped layout rows; do not confuse that value with the physical line count. The prior historical row count above predates this distinction.

Each row below combines two order-reversed blocks of five successful fresh-profile runs (`n=10`). `/O1 AVX` was a same-source diagnostic build only; it is not a distribution payload. AVX and AVX2 are the shipped `/O2 /Ob2 /Oi /Ot /GL` builds. Every run verified process/profile cleanup. The probe waits until the editor's vertical range covers every input line; screenshots copy the visible window pixels immediately after that external readiness check.

| Build | Window visible median (range), ms | Caption ready median (range), ms | Document ready median (range), ms |
|---|---:|---:|---:|
| Diagnostic `/O1` AVX | 317.958 (208.530–435.869) | 392.994 (266.430–523.938) | 789.506 (733.063–898.724) |
| Release `/O2` AVX | 290.020 (210.706–655.054) | 386.378 (304.756–720.502) | 779.595 (730.655–1,092.038) |
| Release `/O2` AVX2 | 296.032 (190.450–524.445) | 371.190 (302.295–629.779) | 784.096 (732.260–985.442) |

The document-ready medians differ by only 9.911 ms end-to-end and their ranges overlap heavily, so this sample does **not** establish a startup-speed advantage for `/O2`, AVX, or AVX2. Keep `/O2` as the throughput-oriented Release policy, but do not claim that ISA selection alone accelerates first display. The dominant startup path remains serial window/bootstrap, document load/layout, and workbench/UI initialization. Discarded final `DwmFlush`, second-`WaitForInputIdle`, and `PrintWindow` probes each produced a verified approximately 23-second delay on an inactive/large window; version 1.4 gates on `documentReadyMs` and copies the visible window rectangle instead.

Final packaging smoke on 2026-07-30 rebuilt both payloads after the project file's last modification: AVX was 4,218,368 bytes and AVX2 was 4,217,856 bytes, both `2.4.3.7163 (5a910555)`. Single fresh-profile committed-sample runs reached `documentReadyMs` in 856.882 ms for AVX (`runId` `20260730-101358-362-8a1054b1`) and 630.508 ms for AVX2 (`runId` `20260730-101411-029-4f1fa398`). These are functional smoke values, not a statistical comparison. The installer-selected AVX2 binary then opened the confidential Desktop input in 214.330 ms visible, 518.700 ms caption-ready, and 733.395 ms document-ready (`runId` `20260730-101427-452-aa940b34`); no confidential screenshot or contents were persisted. Every smoke run verified run-owned process and generated-profile cleanup.

### 2026-07-30 first instrumented serial-path baseline

The first internal-QPC baseline used the rebuilt x64 Release AVX payload and the same machine, warm-cache
condition, and confidential Desktop input described above (`runId` `20260730-123120-914-9bce6e84`). Each
condition had five successful runs. Every run reported scrollbar maximum 5,605, two trace files, 35 valid
records, zero invalid lines or parse errors, and verified process/profile cleanup. The benchmark report is
kept outside the repository because it contains the confidential input path.

| Milestone | Fresh median (range), ms | Existing-profile median (range), ms |
|---|---:|---:|
| Process API returned | 3.747 (3.558–10.726) | 2.005 (1.681–4.707) |
| Top-level window created | 103.546 (94.716–106.774) | 103.309 (71.051–185.924) |
| Window visible | 246.158 (215.410–284.932) | 241.055 (223.801–421.360) |
| DWM flushed | 254.424 (223.558–296.563) | 246.725 (232.124–430.959) |
| First document paint (internal QPC) | 477.332 (444.675–518.860) | 461.962 (437.054–645.788) |
| Document title ready | 1,086.357 (1,029.217–1,186.191) | 1,038.298 (1,022.018–1,303.474) |
| Input idle | 1,486.376 (1,415.256–1,506.961) | 1,421.382 (1,403.218–1,678.285) |
| Document ready | 1,508.780 (1,432.788–1,532.041) | 1,438.814 (1,421.346–1,695.409) |

The serial phase medians below come from matching begin/end records. “Document layout” excludes the
separate zero-line bootstrap layout event. UIPI has two records per run (editor and control); the other
rows have one record per run.

| Internal phase | Fresh median (range), ms | Existing-profile median (range), ms |
|---|---:|---:|
| Control ready wait | 47.827 (47.217–59.268) | 40.860 (38.946–76.975) |
| Control `SetEvent` | 0.007 (0.005–0.008) | 0.007 (0.006–0.008) |
| Control ready handoff | 0.010 (0.008–0.063) | 0.009 (0.007–1.422) |
| File read | 62.157 (57.292–68.381) | 61.389 (56.226–85.663) |
| Document layout | 72.957 (68.683–75.684) | 69.685 (68.423–72.157) |
| Startup document transaction | 203.690 (189.327–210.108) | 195.923 (189.698–224.291) |
| Startup draw commit | 83.317 (76.345–92.614) | 79.069 (74.922–82.868) |
| UIPI check (two roles per run) | 0.090 (0.052–0.256) | 0.106 (0.041–0.191) |

All five document-layout decisions in each condition used zero workers with reason `range_based_color` at
5,604 layout rows; the five bootstrap decisions used zero workers with `below_minimum_lines`. Therefore
ISA selection, `SetEvent`, UIPI, and cross-process handoff are not the next dominant targets. The measured
serial candidates are the startup document transaction and draw commit, with document layout accounting
for about 70 ms. However, the roughly 1.44–1.51 second external document-ready medians also contain
uninstrumented/post-commit UI work, so no listed internal phase alone explains the full end-to-end tail.

Keep `firstContentPaintedMs` separate from external `documentReadyMs`: the former is the first completed
document paint, while the latter verifies full layout through the scrollbar range. The commit explicitly
synchronizes every view's scrollbar after restoring drawing; without that step, layout could be complete
internally while the external range remained at its one-line bootstrap value.

### 2026-07-30 single-binary runtime-ISA validation

The first single-binary runtime-dispatch validation used the x64 Release AVX-baseline payload
(`2.4.3.7163`, revision `5a910555`, 4,508,672 bytes) and the same warm-cache machine and confidential
input described above (`runId` `20260730-131313-069-f2702ce0`). Five fresh-profile and five
existing-profile runs all succeeded and verified process/profile cleanup. Every editor and control process
selected AVX-512F/BW (`isa_dispatch.value1 = 3`). Across all 20 process records, CPUID/XGETBV detection
and immutable dispatch initialization took 19–98 QPC ticks at 10 MHz: 0.0019–0.0098 ms, with a
0.0025 ms median.

Fresh-profile medians were 448.883 ms window-visible and 295.653 ms document-ready; existing-profile
medians were 420.607 ms and 274.413 ms. Treat these as a functional/current-machine baseline rather than
evidence that AVX-512 itself reduced startup time. The confidential report remains outside the repository.

### 2026-07-31 prioritized startup hot-path comparison

The first comparison for the prioritized #2578 startup changes used the same machine, warm-cache
confidential input, and x64 Release runtime-ISA binary as the preceding validation. Build A was master at
`ae763378`; build B added deferred document-dependent workbench initialization, narrower punctuation text
batching, and loop-invariant layout snapshots. Startup tracing was disabled for timing. Ten adjacent
fresh-profile pairs alternated A/B and B/A order; all 20 launches succeeded and verified process/profile
cleanup.

| External milestone | A median (range), ms | B median (range), ms | B minus A medians, ms | B faster pairs |
|---|---:|---:|---:|---:|
| Process API returned | 4.286 (3.937–6.767) | 4.118 (3.705–5.190) | -0.167 | 6/10 |
| Top-level window created | 101.554 (85.096–188.948) | 95.458 (85.880–152.489) | -6.096 | 4/10 |
| Window visible / caption ready | 475.247 (414.987–680.120) | 394.142 (361.289–572.680) | -81.104 | 10/10 |
| Input idle | 121.868 (102.011–215.894) | 112.602 (101.559–237.855) | -9.266 | 5/10 |
| Document ready | 307.014 (282.168–435.943) | 274.642 (249.040–413.674) | -32.373 | 9/10 |

The paired median B-minus-A differences were -59.824 ms for visible/caption and -40.780 ms for document
ready. All ten visible/caption pairs and nine of ten document-ready pairs improved. Input-idle improved in
only five pairs and its paired median was +6.102 ms, so the pooled input-idle median is not evidence of an
improvement. This small machine-specific sample establishes the observed result for these runs, not a
general performance guarantee. Caption readiness and visibility are equal here because the caption changes
while startup drawing is suppressed and the external poll first observes both at the single display commit.

One trace-enabled launch of each binary was used only to attribute removed work, not as the statistical A/B
comparison. Both reports had 89 valid records, zero invalid lines/parse errors, and verified cleanup. The B
process selected AVX-512F/BW at runtime (`isa_dispatch.value1 = 3`).

| Internal measurement (single trace per build) | A | B | Change |
|---|---:|---:|---:|
| Pre-read settings | 8.056 ms | 1.252 ms | -6.804 ms (-84.5%) |
| Document layout | 92.643 ms | 110.384 ms | +17.741 ms (+19.2%) |
| Workbench UI inside startup transaction | 120.470 ms | 27.467 ms | -93.003 ms (-77.2%) |
| Startup document transaction | 341.190 ms | 222.208 ms | -118.982 ms (-34.9%) |
| Startup draw commit | 130.171 ms | 82.463 ms | -47.708 ms (-36.7%) |
| First content-paint duration | 72.890 ms | 42.638 ms | -30.252 ms (-41.5%) |
| Text-output duration within first paint | 69.202 ms | 39.870 ms | -29.332 ms (-42.4%) |
| First-paint text-output calls | 489 | 244 | -245 (-50.1%) |

The layout single-run value did not improve, so the observed attributable gains come from eliminating
bootstrap workbench work and reducing first-paint GDI fragmentation, not from ISA selection or a claimed
layout breakthrough. The trace reports remain outside the repository because their configuration includes
the confidential input path.

## Repository-Wide Change Rules

- Keep dependencies acyclic and pointed from UI/integration layers toward stable core abstractions.
- Preserve explicit completion and cleanup on every branch of stateful startup, command, retry, and process-control flows.
- Do not edit generated `Funccode_define.h`, `Funccode_enum.h`, or `version.h`; edit their source inputs instead.
- When adding C++ sources to an MSBuild project, update both the `.vcxproj` and matching `.vcxproj.filters`. The CMake build discovers source files separately, so verify both build paths when relevant.
- Preserve user-facing Japanese documentation and resources unless the task calls for another language; keep identifiers and code names in their established language.
- Every changed `.cpp`/`.h` must detect as `ascii` or `utf-8-sig`; `.rc`/`.rc2` must be UTF-16. `src/main/py/check_encoding.py` enforces this in CI over the event's base-SHA diff, and UTF-8 *without* a BOM is rejected. Adding a non-ASCII comment to an ASCII-only file therefore breaks the build unless a BOM is added too. Match the surrounding file: keep an ASCII-only subtree ASCII rather than converting it. Markdown is skipped by the checker, so subsystem `CLAUDE.md` files stay unconstrained.
