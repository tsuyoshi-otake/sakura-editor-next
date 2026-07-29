# Sakura Editor Claude Code Guidance

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

Sakura Editor is a Windows text editor written in C++20 and licensed under the zlib License. Windows with MSVC is the primary build path. MinGW support is experimental, and its binaries may not behave correctly.

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
- Use `SKIP_CREATE_GITHASH=1` only when a comparison requires stable generated version data.
- Build behavior and environment variables are documented in `tools/build.md`, `tools/build-batchfiles.md`, and `tools/build-envvars.md`; update those files when the interface changes.

## Verification

- `build-dev.bat` verifies the application build only. Use `build-sln.bat` before running `tests1.exe` or claiming that test targets build.
- `tests1.exe` contains UI and integration suites. A full local run can launch visible editor windows using an isolated/default profile. Read `src/test/CLAUDE.md` before unattended test execution.
- Match verification to the affected build path: test both Debug and Release for MSBuild orchestration changes, and test MinGW when changing shared CMake logic.
- Run static analysis with `run-cppcheck.bat <Platform> <Configuration>` when the change warrants it.

## Repository-Wide Change Rules

- Keep dependencies acyclic and pointed from UI/integration layers toward stable core abstractions.
- Preserve explicit completion and cleanup on every branch of stateful startup, command, retry, and process-control flows.
- Do not edit generated `Funccode_define.h`, `Funccode_enum.h`, or `version.h`; edit their source inputs instead.
- When adding C++ sources to an MSBuild project, update both the `.vcxproj` and matching `.vcxproj.filters`. The CMake build discovers source files separately, so verify both build paths when relevant.
- Preserve user-facing Japanese documentation and resources unless the task calls for another language; keep identifiers and code names in their established language.
