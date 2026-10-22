# Core Application Guidance

## Scope

This file applies to the legacy C++ application under `sakura_core/` and to
`sakura.vcxproj` / `tests1.vcxproj`. The phase map below routes work to the
nearest scoped guidance; local ownership rules stay beside their subsystem.

## Process and Ownership Model

- `WinMain` delegates process selection to `CProcessFactory`.
- `CControlProcess` is the single hidden control/tray process and owns cross-instance coordination.
- `CNormalProcess` is an editor process. It creates `CEditApp`, which owns one `CEditDoc` and one `CEditWnd`.
- `CEditWnd` owns the toolbar, tabs, status bar, and up to four `CEditView` panes.
- `CShareData` / `DLLSHAREDATA` is the legacy inter-process shared-memory ABI. Keep window-local and document-local state out of it, and do not add new revisioned platform state or endpoint metadata there; those use the dedicated control IPC contracts under `platform/controlipc/`.
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
| `platform/` | UI-independent profile, storage, URI/filesystem, IPC, request, and secret contracts |
| `config/` | Settings descriptors, sources, precedence, workspace context, and network policy |
| `workbench/` | Editor/input, contribution, layout, memento, and workbench runtime models |
| `terminal/`, `debug/` | Process/session backends, task/launch inputs, DAP, and their explicit capability boundaries |
| `theme/` | Semantic theme and accessibility tokens |
| `env/` | Shared data, keyword sets, types, filenames |
| `prop/`, `typeprop/` | Common and per-type settings dialogs |
| `types/` | Language-mode definitions |
| `macro/`, `plugin/` | Sakura automation and plug-in integrations |
| `agent/` | Autosave, reload, backup, grep, load/save workers |
| `charset/`, `extmodule/`, `grep/`, `func/`, `util/` | Supporting services and utilities |

## VS Code Compatibility Phase Map

Issue #6's roadmap, contracts, and verified checkpoints are distributed beside
the owning code. A row identifies ownership, not completion; do not centralize
phase-specific rules back into this file.

| Priority | Owning guidance |
|---|---|
| P0 Foundation | [`platform/CLAUDE.md`](platform/CLAUDE.md), [`platform/profiles/CLAUDE.md`](platform/profiles/CLAUDE.md), [`platform/controlipc/CLAUDE.md`](platform/controlipc/CLAUDE.md), [`_main/CLAUDE.md`](_main/CLAUDE.md), [`config/CLAUDE.md`](config/CLAUDE.md), [`io/CLAUDE.md`](io/CLAUDE.md) |
| P1 Editor vertical slice and layout persistence | [`doc/CLAUDE.md`](doc/CLAUDE.md), [`workbench/CLAUDE.md`](workbench/CLAUDE.md), [`workbench/editor/CLAUDE.md`](workbench/editor/CLAUDE.md), [`workbench/layout/CLAUDE.md`](workbench/layout/CLAUDE.md) |
| P2 Workbench and Settings | [`workbench/CLAUDE.md`](workbench/CLAUDE.md), [`workbench/commands/CLAUDE.md`](workbench/commands/CLAUDE.md), [`config/CLAUDE.md`](config/CLAUDE.md) |
| P3 Process backends | [`terminal/CLAUDE.md`](terminal/CLAUDE.md), [`workbench/tasks/CLAUDE.md`](workbench/tasks/CLAUDE.md), [`workbench/problems/CLAUDE.md`](workbench/problems/CLAUDE.md), [`workbench/output/CLAUDE.md`](workbench/output/CLAUDE.md), [`debug/CLAUDE.md`](debug/CLAUDE.md), [`debug/launch/CLAUDE.md`](debug/launch/CLAUDE.md), [`debug/dap/CLAUDE.md`](debug/dap/CLAUDE.md) |
| P4 Layout/interaction parity | [`window/CLAUDE.md`](window/CLAUDE.md), [`view/CLAUDE.md`](view/CLAUDE.md), [`theme/CLAUDE.md`](theme/CLAUDE.md), [`workbench/CLAUDE.md`](workbench/CLAUDE.md) |

At every priority boundary, update the owning guidance with any newly verified
invariant, update the compatibility matrix, and record remaining legacy direct
dependencies.  A later priority must not bypass an unfinished earlier service
contract.

The 2026-07-31 Phase 1 checkpoint composes profile-scoped layout restore and
orderly-shutdown save through control-owned storage. Phase 2 now also composes
the zero-input editor state, Working Copy capture/backup/recovery, native
save/close completion fencing, and close-last-to-empty lifecycle. Production
Revert remains an explicit `Unsupported` gate until full native
document/layout/view/caret/caption rollback can be staged atomically. Phase 3
now separates the Control authority from the selected user-data profile and
routes production Settings through that selection; native profile
management, stable multi-empty-window identity, and durable namespace migration
remain open. Phase 5 has a verified Explorer/Problems/Output command spine but
not yet complete Palette/menu/keybinding compatibility. Phase 6 now
has runtime-owned workspace-artifact routing plus pure Marker, Output, Task
catalog/execution, Launch catalog, DAP codec/session, Debug Console, and Ports
state foundations. These lower layers are lifecycle- and failure-tested, but
production terminal/debug/forwarding adapters, runtime ownership of the
remaining catalogs/services and native service-backed
projections remain open; do not reopen Phase 1/2 ownership in native window
code.

## MSBuild Project Maintenance

- MSBuild source lists are explicit. Add or remove files in both the owning `.vcxproj` and its `.vcxproj.filters` file.
- `StdAfx.h` / `StdAfx.cpp` and the test `pch.h` are precompiled-header boundaries. Avoid broad PCH additions that increase every translation unit's rebuild cost.
- Function-code headers and `version.h` are generated under the CMake tools build directory. Change `Funccode_x.hsrc` or the relevant CMake template, not generated output.
- The normal compiler path uses parallel compilation. Assembly listings are opt-in through `SAKURA_GENERATE_ASSEMBLY_LISTINGS`; keep them off for the regular edit loop.
- A whole-program-optimized product object retains its `/Fa` listing destination. `tests1` relinks a support archive of those `/GL` objects, so its LTCG pass replays that destination and can write the product `.asm` path again, producing `C1083 ... Permission denied` / `LNK1257` against an apparently unrelated source. The canonical listing build must finish solution/tests with listings explicitly off before rebuilding only `sakura.vcxproj` with listings on and `/m:1`. In that product pass, `cl.exe`'s provisional listings are deleted immediately before Link so LTCG can write the final listings to the same paths, and the listing branch links with `/CGTHREADS:1`. `/m:1` and `/CGTHREADS:1` are the settings that matter: the first keeps the product pass away from the `tests1` relink, the second keeps the LTCG code generation that writes the shipped listings deterministic. The compile step itself runs at the full job budget. Serializing it too was measured against the `/m:1` baseline and produced all 539 `.asm` files byte for byte while costing 103.75 s of a 332.78 s pass, so `MultiProcessorCompilation` is no longer disabled here (issue #203). Keep the remaining settings and the phase separation scoped to the opt-in path; do not paper over the failure with retries or broad output cleanup.

## Incremental-Build Invariants

- Keep FileTracker enabled for the parent `sakura` and `tests1` projects. `TrackFileAccess=false` is scoped only to generated nested CMake/MSBuild children.
- Expensive child builds and generators must have complete `Inputs` / `Outputs` contracts. Do not put unconditional CMake builds on `ClCompile` or `Link` hot paths.
- `RemoveSakuraExe` and `RemoveTests1Exe` must remain incremental link prerequisites. Do not restore unconditional delete targets or fixed sleeps on no-op builds.
- Use `tools/Remove-FileWithRetry.ps1` for bounded executable replacement retries. Preserve the underlying failure exit code when deletion ultimately fails.
- Build-generation details and cross-generator constraints live in `src/main/CLAUDE.md`.

### A read log that collapsed to the PCH is never rebuilt (2026-10-22, #367)

- **Symptom:** the local x64 Debug `sakura.exe` crashed at startup on every launch, before any window appeared. Some runs failed with an access violation in `CEditView::Create` with `this == 0xFFFFFFFFFFFFFFFF`, others with `0xC0000374` heap corruption. Release and CI were unaffected.
- **Not a code defect.** The linker kept the out-of-line `CEditWnd::GetView` COMDAT from a stale object. That copy reads `m_pcEditViewArr` at `this+0x1B0D0`, while the current layout places it at `+0x1B0F8`. The 0x28-byte difference is the Debug size of the `std::wstring` member that 940d2810c added to `CEditWnd.h`.
- **Why one stale object breaks the whole program:** in Debug every in-class member function is a COMDAT, so a single stale object can supply the copy the whole program uses. The wrong-offset read or write then appears far from the header edit that caused it.
- **Why the objects were stale:** 117 objects from one build batch (2026-10-18 22:16) had `CL.read.1.tlog` entries that listed only `sakura.pch`. Each of those sources directly includes headers outside the PCH, such as `window/CEditWnd.h`. `MinimalRebuildFromTracking` compares only the recorded inputs, so no header change could ever rebuild those objects.
- **Detection:** a PCH-only entry is legitimate only when every header the source directly includes is already in the `/Yc` (`StdAfx.cpp`) entry's closure. 23 legitimate entries exist, for example `basis/` and `types/` sources. Any other PCH-only entry is poisoned.
- **Repair:** delete the poisoned objects, or rebuild the project. The next incremental build recompiles them with real tracking. After the repair, every Debug object agreed on `+0x1B0F8`.
- **Ruled out, in a minimal PCH project:**
  - `TrackFileAccess=false` in the environment removes the source's entries, so the next tracked build recompiles it.
  - A failed `/MP` batch also recompiles correctly.
  - The mechanism that wrote the PCH-only entries is unconfirmed. Record it here when it is found.
- **Diagnosing a "corruption" with an impossible value:** set a hardware write breakpoint (`ba w8`) on the field that holds the bad value. If it never fires, the reader is wrong, not a writer. Then compare the offset in the reader's disassembly against `?? #FIELD_OFFSET(Class, member)` from the PDB.

## Verification

- Use root `build-dev.bat` for application-only iteration and `build-sln.bat` for changes that affect `tests1.vcxproj` or shared targets.
- For project-file and build-target changes, verify a no-op rebuild after the first successful build; it should not compile, link, delete the target executable, or rebuild unchanged CMake children.
