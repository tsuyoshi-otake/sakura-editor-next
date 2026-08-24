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
  and View IDs must match upstream VS Code
  exactly. Do not invent a parallel naming scheme for a concept VS Code already
  names.
- **Match VS Code's placement, defaults, and keybindings.** Example: the
  supported Activity Bar containers (Explorer, Search, Source Control, and Run
  and Debug) live in the **Primary Side Bar**; the Secondary Side Bar is empty
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

## Working Branch: Commit Directly on `main`

This fork has a single developer, so there is no branch/review flow to protect.
Work on `main` and commit there directly. This overrides the default habit of
branching before committing when the current branch is the default branch.

- Do not create a `work/issue-<N>-<slug>` branch, and do not open a fork-side
  Pull Request, unless the user asks for one in that turn.
- Commit only when the user asks, and push only when the user asks. Committing
  on `main` is not permission to push `main`.
- The commit message rules are unchanged: English, describing the change in
  reasonable detail rather than one line, and referencing the tracking Issue
  (for example `Refs #217`). Keep creating or reusing that tracking Issue.
- Everything in **Repository Boundary: Fork-Only by Default** still applies.
  `main` here means this fork's `main`; it is never an upstream branch.

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
| SENP package management and extension runtime | [`sakura_core/senp/CLAUDE.md`](sakura_core/senp/CLAUDE.md) |
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
- `requirements.txt` is a hash-locked, `--no-deps` install list, not a normal pinned requirements file: every CI workflow installs it with `uv pip install --require-hashes --no-build --no-deps -r requirements.txt`. `--no-deps` is load-bearing — it stops pip/uv from resolving each package's own transitive dependencies, so `requirements.txt` only needs a hash entry for the packages it lists directly, not for something like `pytest`'s Windows-only `colorama` dependency. Installing it locally with a plain `pip install -r requirements.txt` (no `--no-deps`) makes pip resolve the full dependency tree, hit an unhashed transitive package under `--require-hashes` mode, and fail with "these do not [have pinned hashes]" for a package that was never meant to need one. When reproducing a Python-tooling CI check locally, install with the exact same flags the workflow uses, in a task-specific venv — not a bare `pip install -r requirements.txt`.

## Verifying What the Window Actually Painted

Stale-pixel defects (a part that vanishes, a control stranded at old
coordinates) are invisible to checks that read the window's own state. Whenever
a change touches layout, repaint, invalidation, or window movement, use the
dual-capture verification methodology in the
[`stale-pixel-verification` skill](.claude/skills/stale-pixel-verification/SKILL.md)
— it defines the CopyFromScreen-vs-PrintWindow comparison, the trial protocol,
the noise-floor rules, and the Issue #17 baseline results.

## Startup Performance Baseline

Measure startup changes with
[`tools/measure-startup-performance.ps1`](tools/measure-startup-performance.ps1)
following [`tools/startup-performance.md`](tools/startup-performance.md).
Compare medians from at least five runs under identical conditions; results are
machine-specific comparison baselines, not performance guarantees. The dated
baseline tables and measurement history live in the
[`startup-benchmarks` skill](.claude/skills/startup-benchmarks/SKILL.md).

## Repository-Wide Change Rules

- Keep dependencies acyclic and pointed from UI/integration layers toward stable core abstractions.
- Preserve explicit completion and cleanup on every branch of stateful startup, command, retry, and process-control flows.
- Do not edit generated `Funccode_define.h`, `Funccode_enum.h`, or `version.h`; edit their source inputs instead.
- When adding C++ sources to an MSBuild project, update both the `.vcxproj` and matching `.vcxproj.filters`. The CMake build discovers source files separately, so verify both build paths when relevant.
- Preserve user-facing Japanese documentation and resources unless the task calls for another language; keep identifiers and code names in their established language.
- Every changed `.cpp`/`.h` must be ASCII-only or valid UTF-8 with a BOM; `.rc`/`.rc2` must be valid BOM-marked UTF-16 LE or BE. `src/main/ps1/check-encoding.ps1` enforces this deterministically in CI over the event's base-SHA diff, and UTF-8 *without* a BOM is rejected. Adding a non-ASCII comment to an ASCII-only file therefore breaks the build unless a BOM is added too. Match the surrounding file: keep an ASCII-only subtree ASCII rather than converting it. Markdown is skipped by the checker, so subsystem `CLAUDE.md` files stay unconstrained.
- `.claude/memory/` (`rules.md`, `journal.md`) holds Claude Code's working notes and verified lessons for this repository. Keep it local: do not `git add`, commit, or push it, even though it is not listed in `.gitignore` — write and update the files, but leave them untracked.
