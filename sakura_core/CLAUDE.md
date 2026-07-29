# Core Application Guidance

## Scope

This file applies to the legacy C++ application under `sakura_core/` and to `sakura.vcxproj` / `tests1.vcxproj`. More focused rules for command dispatch and the document model live in `cmd/CLAUDE.md` and `doc/CLAUDE.md`.

## Process and Ownership Model

- `WinMain` delegates process selection to `CProcessFactory`.
- `CControlProcess` is the single hidden control/tray process and owns cross-instance coordination.
- `CNormalProcess` is an editor process. It creates `CEditApp`, which owns one `CEditDoc` and one `CEditWnd`.
- `CEditWnd` owns the toolbar, tabs, status bar, and up to four `CEditView` panes.
- `CShareData` / `DLLSHAREDATA` is the inter-process shared-memory boundary. Add state there only when it must be shared between processes; keep window-local and document-local state out of it.
- Do not bypass `CProcessFactory` for startup, profile selection, or control/editor process selection. Do not initialize document/view-dependent state before `CEditDoc` exists.

## Subsystem Map

| Directory | Responsibility |
|---|---|
| `_main/` | Entry point, process selection, process lifecycle |
| `_os/` | Windows and OS abstractions |
| `window/` | Frame windows, tabs, toolbar, status bar |
| `view/` | Editing views, caret, ruler, painting |
| `cmd/` | `EFunctionCode` dispatch and command implementations |
| `doc/` | Logical text, layout, editing, file operations, document type |
| `env/` | Shared data, keyword sets, types, filenames |
| `prop/`, `typeprop/` | Common and per-type settings dialogs |
| `types/` | Language-mode definitions |
| `macro/`, `plugin/`, `extension/` | Automation and extension integrations |
| `agent/` | Autosave, reload, backup, grep, load/save workers |
| `charset/`, `extmodule/`, `grep/`, `func/`, `util/` | Supporting services and utilities |

## MSBuild Project Maintenance

- MSBuild source lists are explicit. Add or remove files in both the owning `.vcxproj` and its `.vcxproj.filters` file.
- `StdAfx.h` / `StdAfx.cpp` and the test `pch.h` are precompiled-header boundaries. Avoid broad PCH additions that increase every translation unit's rebuild cost.
- Function-code headers and `version.h` are generated under the CMake tools build directory. Change `Funccode_x.hsrc` or the relevant CMake template, not generated output.
- The normal compiler path uses parallel compilation. Assembly listings are opt-in through `SAKURA_GENERATE_ASSEMBLY_LISTINGS`; keep them off for the regular edit loop.

## Incremental-Build Invariants

- Keep FileTracker enabled for the parent `sakura` and `tests1` projects. `TrackFileAccess=false` is scoped only to generated nested CMake/MSBuild children.
- Expensive child builds and generators must have complete `Inputs` / `Outputs` contracts. Do not put unconditional CMake builds on `ClCompile` or `Link` hot paths.
- `RemoveSakuraExe` and `RemoveTests1Exe` must remain incremental link prerequisites. Do not restore unconditional delete targets or fixed sleeps on no-op builds.
- Use `tools/Remove-FileWithRetry.ps1` for bounded executable replacement retries. Preserve the underlying failure exit code when deletion ultimately fails.
- Build-generation details and cross-generator constraints live in `src/main/CLAUDE.md`.

## Verification

- Use root `build-dev.bat` for application-only iteration and `build-sln.bat` for changes that affect `tests1.vcxproj` or shared targets.
- For project-file and build-target changes, verify a no-op rebuild after the first successful build; it should not compile, link, delete the target executable, or rebuild unchanged CMake children.

