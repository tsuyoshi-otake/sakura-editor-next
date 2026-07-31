# Phase 2 Editor and Working Copy Guidance

## Scope and Dependency Direction

This directory owns presentation-neutral editor inputs, shared document and
working-copy state, editor groups, command coordination, and the codecs and
interfaces used for backup/session restore. Dependencies point inward in this
order:

`native/legacy adapters -> operation coordinator -> editor/working-copy models`

Pure models and codecs must not include or retain HWND, `CEditWnd`, `CEditDoc`,
Win32 dialogs, `EFunctionCode`, IPC clients, profile paths, or direct filesystem
writes. Adapters may translate those facilities into bounded DTOs and typed
terminal outcomes, but may not make the legacy object graph authoritative for
whether an editor input is open or active.

## Identity and Ownership

- An editor input is a view over a working copy. It is not the file, native tab,
  window, or backing `CEditDoc`.
- Working-copy identity is the stable input `typeId` plus a canonical resource
  URI or a bounded opaque Untitled identifier. Display titles and process/window
  IDs are never identities or durable keys.
- The same canonical identity resolves to one shared working copy even when
  multiple inputs reference it. Inputs, resolver references, working-copy
  ownership, and active selection remain independent lifetimes.
- A group always exists and may contain zero inputs. `activeInput == null` is a
  normal ready state, not a startup failure and not an implicit Untitled file.
- Reserve stable group and lifecycle identities now even while the native bridge
  can project only one logical group and one backing legacy document.

## Working Copy State and Operations

- Stored-file state explicitly distinguishes `Saved`, `Dirty`, `PendingSave`,
  `Conflict`, `Orphaned`, and `Error`. Do not collapse external modification,
  deletion, I/O failure, cancellation, and unsupported capability into `dirty`.
- Content version increases monotonically after each committed edit. A save or
  revert captures the identity and version it began against; a late completion
  cannot clean, replace, or dispose newer content.
- Save, Save As, Revert, Close, backup, and restore have bounded operation IDs
  and exactly one terminal outcome. Exact immutable retries replay; reusing an
  ID with different intent is a conflict. Every cancellation/error branch keeps
  explicit finalization ownership.
- Close is a two-stage operation. Its prepare phase may prompt/save but must not
  clear the backing document. Core close commits next; only then may a guaranteed
  finalizer clear legacy content. Cancelled prepare or failed core commit leaves
  both input and backing document present.
- The pure coordinator owns a staged Revert transaction contract and tests its
  prepare/apply/Core/finalize/rollback terminals. The production `CEditDoc`
  adapter must continue to report explicit `Unsupported` until it can preserve
  and restore the full native document, undo/layout, and view state; an off-side
  line read alone is not sufficient rollback safety.
- Commands use the stable IDs in `EditorCommandIds.h`. Native menus/keys,
  watermark actions, command palette, and extension RPC converge on the same
  coordinator; observers such as extension document events run after commit.

## Backup and Session Persistence

- Dirty/Untitled content belongs to versioned Working Copy Backup. The session
  manifest stores safe untyped input descriptors, group placement, active input,
  and backup-generation references. Layout mementos are a third independent
  record. Never duplicate document text into layout or session records.
- Backup identity includes logical Profile/Workspace scope and working-copy
  identity, never PID/HWND. Records carry schema, content version, generation,
  encoding/EOL metadata, and an integrity checksum.
- Durable adapters are composed outside this directory and use control-owned
  storage. Writes are atomic/CAS-based; ambiguous transport loss permits at most
  one identical replay. Removing generation N must never remove N+1.
- Corrupt, unsupported, or partially invalid durable data is diagnosed and
  preserved. Startup must not overwrite it with an empty/default record.
- Restore waits for workbench/group readiness and registered input handlers.
  Lifecycle recovery adopts the singleton dirty input inactive and commits the
  staged native content. The composition layer then validates and activates the
  exact persisted/effective ID and projects it; it never infers selection from
  `CEditDoc`. Recovery never silently reports the source file as saved.
- A singleton session persists `activeInputId = snapshot.inputId`. A null active
  ID is accepted only as explicit migration of a legacy singleton record;
  inconsistent IDs fail before native prepare. `restoredInputId` and
  `effectiveActiveInputId` are exposed only after native commit succeeds.

## Phase 2 Capability Gate

The current native architecture owns one `CEditDoc` per editor process. Phase 2
may expose a single logical group and zero/one projected legacy input while the
pure contracts already support shared working-copy identity. Do not claim native
multi-input, split-group, multi-window session parity, transactional revert, or
remote-resource support until their adapters and tests exist.

Update this checkpoint only after the focused Phase 2 rubric passes. Aspirational
contracts above are requirements, not evidence of completion.

## Lifecycle Integration Boundary

The `persistence/` subtree owns the lifecycle coordinator and its pure DTOs.
`EditorWorkingCopyLifecycleBridge` is the only UI-facing bridge: it converts a
stable post-commit core snapshot into debounce/flush notifications and exposes
completion tokens for save and close. Callers capture a token before a native
operation, then notify the bridge only after that operation has committed; a
cancel, error, stale result, or unsupported operation must not delete a backup.
The token carries identity, content version, and an opaque persistence fence.
Exact retry is an idempotent success, while a stale fence conflicts. Save As can
replace identity only through the explicit identity-replacement completion mode
for the same accepted content version.

The bridge reads immutable core snapshots through
`EditorCoreWorkingCopyCaptureContextSource`; it must never scrape window text or
infer identity from a caption. Recovery is compensating: native staging commits
only after inactive core adoption, and a rejected native commit rolls back that
exact inactive core input. The one-document native projection remains a
capability limit, not permission to weaken the core identity/version checks.
After that commit, the composition owner alone performs exact active placement;
the persistence subtree does not select UI focus.
