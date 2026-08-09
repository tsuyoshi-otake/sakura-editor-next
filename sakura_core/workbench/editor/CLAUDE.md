# Phase 2 Editor and Working Copy Guidance

## Scope and Dependency Direction

This directory owns presentation-neutral editor inputs, shared document and
working-copy state, editor groups, command coordination, and the codecs and
interfaces used for backup/session restore. Dependencies point inward in this
order:

`native/legacy adapters -> operation coordinator -> editor/working-copy models`

The selection pilot is a deliberately narrower leaf. `sakura_editor_selection`
owns the presentation-neutral selection phase, mode, and lock (`SelectionSession`);
its public contract is under `sakura/editor/SelectionSession.h` and must not
include a document, layout, HWND, or legacy view type. `CViewSelect` remains the
compatibility adapter for native input/drawing and still owns selection ranges
and geometry. `End()` terminates only the transient input phase, while `Clear()`
is the explicit terminal for phase, mode, and lock; preserve that distinction
when translating mouse release versus full selection clear. Do not treat this
pilot as full selection independence or move the remaining range/geometry fields
without a typed coordinate/lifecycle contract and a standalone contract runner.

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
- The empty-editor watermark mirrors VS Code's `.editor-group-watermark`: one
  vertically centered column holding a square product letterpress capped at
  256 DIP, a 24 DIP gap, and the shortcut list. `EmptyEditorSurfaceModel` owns
  that geometry; `CEmptyEditorSurface` only paints it. The square shrinks with
  the viewport and is dropped entirely before the action list loses space, so
  an empty letterpress rectangle is a normal state rather than a failure.
- VS Code paints no text wordmark there, so the product name lives in the
  accessible name instead of the canvas. Both the label and its keybinding use
  the `descriptionText` token; do not reintroduce `secondaryText` or the removed
  `editorWatermark.foreground` token, which no longer exists upstream.
- The entire empty editor group is a double-click target. Match
  `EditorGroupView.registerContainerListeners`: a double-click anywhere on the
  empty surface creates the pinned Untitled input, while the group continues to
  show no document tab before that explicit transition.
- `CExtensionDetailSurface` is a native composition-layer metadata projection,
  not an `EditorInput` and not a second document model. `CEditWnd` may show it
  only while the native editor has no active document, and must hide it before
  projecting a document. Its gallery media, feature/changelog/pack rendering,
  and recommendation/filter surfaces must remain explicit rather than being
  represented by stale or fake document state. README Markdown may be supplied
  through its typed state API after a bounded OpenVSX text fetch; the surface
  renders that content natively and never executes remote HTML or links.

### Marketplace README rendering (2026-08-06)

The README body is the shared native Markdown preview,
`markdown::CMarkdownPreviewWnd` — the same renderer the editor's Markdown
preview uses — hosted as a child of the detail surface. A Marketplace README is
ordinary Markdown, so it gets real headings, lists, tables, inline styles, and
syntax-highlighted code instead of a reduced private approximation. Do not
reintroduce a second parser here: this surface owns metadata projection, not
Markdown semantics.

Keep the following invariants, and note that they are *not* the extension
webview boundary. `createWebviewPanel` renders arbitrary extension-authored
HTML/JS and remains a typed unsupported capability; rendering a Markdown README
is a different problem and is supported. Do not collapse the two.

- **No network access moves into this subtree.** The composition root fetches the
  README text and hands it in through the typed state API. The surface and the
  preview both stay fetch-free.
- **Every README resource is external and therefore blocked.** `ParseOptions`
  is built with an empty `documentPath` and an empty `workspaceRoot`, because a
  Marketplace README has no local root. Every image and link in it resolves as
  `ResourceDisposition::ExternalBlocked`, so it is reported rather than fetched.
  That is a deliberate fail-closed divergence from VS Code, which does load
  Marketplace images: acquiring them would add a new network boundary to a
  surface whose contract is that it has none.
- **Header pinned, body scrolled, boundary footer pinned.** VS Code's extension
  editor keeps its metadata header fixed while the README scrolls, so the parent
  carries no `WS_VSCROLL` and adds no `WM_VSCROLL`/`WM_MOUSEWHEEL` handler; the
  preview child owns the only scroll authority. `PaintHeader` and
  `PaintBoundaryFooter` each measure with a null `HDC` and paint with a real
  one, so the paint pass and the child-layout pass cannot disagree about where
  the body starts and ends.
- **FEATURES and CHANGELOG stay explicit.** VS Code puts them in editor tabs;
  this surface has no tab strip, so they are a pinned two-row footer instead of
  scrolling away inside the README. FEATURES is an invariant boundary
  (`SOpenVsxExtension` carries no contributed-command or configuration data at
  all); CHANGELOG branches on `sChangelogUrl` and reports "available upstream,
  not fetched" rather than claiming nothing exists. EXTENSION PACK is omitted
  entirely, matching VS Code, because the DTO has no pack field to check.
- **A blank README is an answer, not a failure.** `Ready` with whitespace-only
  Markdown hides the preview and states that the extension supplied no README,
  instead of rendering an unexplained empty body.
- **Renders are generation-ordered.** Each publish takes a new
  `markdown::PreviewRenderKey` generation, so a superseded parse can never
  commit over a newer one when the user switches extensions quickly.
- `CDiffSurface` is the same kind of native projection for a side-by-side
  comparison: not an `EditorInput`, no document model, shown only while the
  native editor has no active document, and hidden before a document is
  projected. It renders content handed to it and never reads files or runs git.
  It keeps its own presentation-neutral row type, `SDiffSurfaceRow`, so this
  subtree never names an SCM type; the composition root translates
  `workbench::scm::GitDiffViewRow` into it. Its `truncated` flag carries the
  diff model's bound to the user, so a bounded alignment is never rendered as a
  complete one. The comparison's own divergences from VS Code's diff editor are
  recorded in [`../scm/CLAUDE.md`](../scm/CLAUDE.md); do not restate them here.

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
