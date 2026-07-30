# Sakura Editor NEXT Claude Code Guidance

## Scope

This file contains repository-wide guidance. More specific `CLAUDE.md` files are loaded when Claude works in their subtrees; follow the nearest file and keep local details out of this root file.

| Area | Scoped guidance |
|---|---|
| Core application and MSBuild projects | [`sakura_core/CLAUDE.md`](sakura_core/CLAUDE.md) |
| Command dispatch | [`sakura_core/cmd/CLAUDE.md`](sakura_core/cmd/CLAUDE.md) |
| Document model | [`sakura_core/doc/CLAUDE.md`](sakura_core/doc/CLAUDE.md) |
| Build generation and shared resources | [`src/main/CLAUDE.md`](src/main/CLAUDE.md) |
| Tests and test infrastructure | [`src/test/CLAUDE.md`](src/test/CLAUDE.md) |
| Build helpers and build documentation | [`tools/CLAUDE.md`](tools/CLAUDE.md) |
| Language resource DLLs | [`sakura_lang/CLAUDE.md`](sakura_lang/CLAUDE.md) |
| CI workflows | [`.github/CLAUDE.md`](.github/CLAUDE.md) |

## Project Overview

Sakura Editor NEXT is a Windows text editor written in C++20 and licensed under the zlib License. Windows with MSVC is the primary build path. MinGW support is experimental, and its binaries may not behave correctly.

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

## Repository-Wide Change Rules

- Keep dependencies acyclic and pointed from UI/integration layers toward stable core abstractions.
- Preserve explicit completion and cleanup on every branch of stateful startup, command, retry, and process-control flows.
- Do not edit generated `Funccode_define.h`, `Funccode_enum.h`, or `version.h`; edit their source inputs instead.
- When adding C++ sources to an MSBuild project, update both the `.vcxproj` and matching `.vcxproj.filters`. The CMake build discovers source files separately, so verify both build paths when relevant.
- Preserve user-facing Japanese documentation and resources unless the task calls for another language; keep identifiers and code names in their established language.
