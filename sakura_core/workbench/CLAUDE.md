# Phase 1/2/5 Workbench Guidance

## Scoped Guidance

- Editor inputs, Working Copies, operation coordination, backup, and sessions:
  [`editor/CLAUDE.md`](editor/CLAUDE.md)
- Contribution layout state and durable layout mementos:
  [`layout/CLAUDE.md`](layout/CLAUDE.md)
- Native projection of validated Workbench snapshots:
  [`win32/CLAUDE.md`](win32/CLAUDE.md)
- Markdown hover rendering (`HoverWidget` / `renderMarkdown` equivalents):
  [`hover/CLAUDE.md`](hover/CLAUDE.md)
- Stable commands, surface bindings, when clauses, and context keys:
  [`commands/CLAUDE.md`](commands/CLAUDE.md)
- Files Explorer view and its context-menu row model:
  [`explorer/CLAUDE.md`](explorer/CLAUDE.md)
- Workspace artifact routing and file/watch ownership:
  [`workspace/CLAUDE.md`](workspace/CLAUDE.md)
- Problems/markers, Output channels, and Tasks:
  [`problems/CLAUDE.md`](problems/CLAUDE.md),
  [`output/CLAUDE.md`](output/CLAUDE.md),
  [`tasks/CLAUDE.md`](tasks/CLAUDE.md)

## Stable Boundary

The workbench consumes platform, configuration, document, and extension
contracts. Pure models do not depend on HWND, `CEditWnd`, `CEditDoc`, or the
extension transport. Legacy and Win32 behavior enters through adapters only.

## P1 Editor Vertical Slice

- `EditorGroupModel` always exists and owns zero or more `IEditorInput` values.
  Its active editor is optional; zero editors is a normal state.
- Keep resolved documents, visible editors, and active editor as separate sets.
- New/Open/Show/Save/Revert/Close return exactly one of succeeded, cancelled,
  failed, or not-applicable and preserve the last stable state on failure.
- `CEditDoc`/`CEditView` are backing objects behind a strangler adapter. Their
  mere existence does not expose an open or active editor.
- Commands, context keys, focus, native menus, keys, and watermark actions all
  enter through the same command IDs.

### Editor Core Checkpoint

The pure Editor Core model now supports a legitimate empty group with no active
input. It keeps resolved documents, open inputs, and the active input separate;
tracks resolver and input references independently; and disposes a document only
after its final reference is released. Operations use optimistic model revisions
and bounded exact-request replay. Committed notifications are published in
revision order and drained outside the model lock without recursively delivering
reentrant changes; revision exhaustion fails explicitly instead of wrapping.

The production adapter now exposes the legitimate empty-editor state: no tab,
ruler, editor view, or document status surface is fabricated when no input is
open. The empty surface and native commands enter the same stable VS Code command
IDs, and opening the first input transitions the core and legacy backing objects
in one visible commit. Working-copy integration and broader save/dirty migration
remain pending.

### Bootstrap and Runtime Checkpoint

- `WorkbenchBootstrapContext` is an immutable value assembled after the editor
  has verified the control-owned Profile identity. Profile resources, explicit
  workspace shape, initial document, terminal fallback, and window identity are
  separate fields; no consumer may infer one from another.
- `CWorkbenchRuntime` owns the process-local configuration and semantic workspace
  services. Empty, Folder, and Workspace are explicit states, and only explicit
  Folder/Workspace roots participate in `.vscode/settings.json` loading.
- `CEditApp` starts the runtime before constructing `CEditWnd` and destroys the
  window before stopping the runtime. Runtime start/stop and configuration reload
  paths expose explicit terminal states and dispose workspace callbacks on stop.
- The `.code-workspace` settings body is parsed through the existing workspace
  transaction. `CWorkbenchRuntime` owns the configuration watch lifecycle:
  callbacks are advisory, coalesced, and resnapshot through the file-source
  controller; `Stop` cancels and joins watchers before final state so no
  post-stop apply can occur. For a missing `.vscode`, retain the folder-level
  lifecycle watch and recreate the member watch after lifecycle/rescan events.
- `CWorkbenchRuntime` is the sole production-facing Settings writeback owner.
  `WriteSetting` delegates to the bounded coordinator and the existing file
  source controller, which serializes source revision bookkeeping with advisory
  watch reloads. Runtime Stop first gates versioned reads/replaces and stops
  writeback before joining watches, so no post-stop resnapshot applies a
  setting. Current writeback accepts resolved `settings.json` resources; UI and
  `.code-workspace` nested-settings editing are still separate work.
- `WriteSetting` is declared on `IWorkbenchRuntime`, not only on the concrete
  runtime, because a borrower that needs it must not have to name the
  implementation. `CEditWnd` holds the runtime as `IWorkbenchRuntime*`.
  `CWorkbenchRuntime` is
  still the only implementer and still the only writeback authority — putting
  the method on the interface widens who may *call* it, never who may own it.

## P2 Workbench MVP

Use stable part, view-container, view, menu, and command IDs. PROBLEMS and
OUTPUT must bind to diagnostics/problem matching and output channels, not a
visual placeholder. Settings, Explorer, and Search must use the same services
and lifecycle rules as the rest of the native workbench.

### Contribution and Layout-State Checkpoint

- `WorkbenchContributionRegistry` is the immutable process-local catalog for
  built-in Part, ViewContainer, and View descriptors. Runtime contribution
  registration is not supported.
- Use the canonical VS Code IDs in `WorkbenchIds`. A physical Part ID is never a
  View ID: in particular `workbench.parts.auxiliarybar` and `outline` describe
  different layers and must remain independently movable and visible.
- `WorkbenchLayoutStateService` owns HWND-free visible, active, focused,
  position, alignment, order, and committed-extent state. Reveal, activation,
  focus, and movement are separate operations. Contribution changes reconcile
  automatically through `CWorkbenchRuntime`, with deterministic active-view and
  focus fallback when an owner is disposed.
- `WorkbenchLayoutMementoCodec` is bounded machine-owned JSON, not user-authored
  Settings JSONC. It persists only stable IDs and committed model state, keeps
  signed order values and structurally valid unknown IDs, and omits HWND,
  generation/revision, maximized state, drag state, and other transient geometry.
- Persisting a memento through the control-owned profile/workspace storage writer
  is a separate adapter responsibility. Do not add direct file writes or make the
  process-local layout model a storage authority.

### Phase 1 Layout-Memento Persistence Checkpoint (2026-07-31)

- `WorkbenchLayoutStateService` and `WorkbenchLayoutMementoCodec` remain pure:
  they must not depend on HWNDs, editor controls, storage interfaces, IPC, or
  profile-path details. A composition adapter is the only bridge between the
  layout service and control-owned storage.
- The durable record is the profile-scoped `workbench.layout` key, targeted at
  `Machine` state. PID-derived window identity is runtime-only and must never be
  a durable key or part of the memento payload.
- Restore must happen before creating the native workbench window. Its typed
  outcomes are `Loaded`, `NotFound`, `InvalidStoredMemento`, `Unavailable`, and
  `Failed`; only `Loaded` applies a decoded memento. Invalid data includes corrupt or unsupported durable
  payloads and must leave that payload untouched for diagnosis/recovery.
- On orderly shutdown, capture the global storage revision for the CAS write,
  destroy the native window, then save once before editor-control runtime stop.
  Skip an unchanged memento. Storage writes are full-file O(N), so layout events
  must update process-local state only and must not each write durable state.
- An ambiguous transport failure may retry the same mutation at most once, with
  the identical operation ID. A CAS conflict preserves the remotely committed
  value and reports a typed conflict; it is never hidden by a retry or overwrite.
- No editor, view, or layout-model class may write durable state directly. The
  current multi-window policy is profile-shared: the last successful,
  nonconflicting orderly closer wins. Stable logical window/workspace identities
  are deliberately deferred; do not approximate them with a PID.

The production control-platform adapter is composed through `CEditApp`, and the
focused layout run passes 42/42 tests. This checkpoint does not complete the
working-copy/editor-operation work above, native shell projection, or deferred
multi-window/workspace memento identity.

## P4 Layout and Interaction

Layout is model-first: part visibility, position, size constraints, persisted
extent, focus target, and DPI-independent tokens produce a snapshot applied by
Win32 adapters. Multiple groups/windows, drag/drop, panel movement,
accessibility, keyboard navigation, and restore extend the model rather than
adding one-off HWND branches. Unsupported capabilities are explicit.

### Native sash and Explorer scrolling compatibility (2026-08-04)

- Workbench Part boundaries reserve one DPI-independent pixel, matching VS
  Code's visible sash border. A sibling, input-only HWND overlays that line as a
  four-DIP interaction target without changing layout geometry or painting a
  thick bar. Parent-only hit-test math is insufficient because the adjacent
  child HWNDs consume three of those four pixels before `CEditWnd` can see the
  initial press; keep the overlay above siblings after every layout pass.
- Explorer never exposes a horizontal scrollbar. Its TreeView keeps
  `TVS_NOHSCROLL`, native scrollbars are suppressed, and the owned vertical
  scrollbar overlays the content instead of reserving a permanent gutter.
- The overlay scrollbar is one shared control, `workbench/controls/COverlayScrollbar`.
  Every workbench list that scrolls uses it -- the Explorer TreeView and the
  Source Control list today -- so no view can drift back to the platform
  scrollbar while its neighbours draw the VS Code one. The control reads the
  target's own `SB_VERT` `SCROLLINFO` and asks it to scroll through one
  callback, which is the only view-specific part (a TreeView walks visible
  items; a LISTBOX takes `LB_SETTOPINDEX`).
- The same control now also serves the Markdown preview pane and the editor
  text body, and it has a horizontal orientation
  (`OverlayScrollbarOrientation::Horizontal`) for the editor's horizontal bar.
  The Markdown preview is different from native-control consumers: it has no
  `WS_VSCROLL` and supplies one pixel-based `ExplicitModel`. The preview's
  `m_scrollY`, content extent, and viewport extent are the sole authority; the
  overlay must not call `GetScrollInfo`, `SetScrollInfo`, or `ShowScrollBar` for
  that source. This prevents native non-client geometry from competing with the
  sibling overlay during width changes.
  The editor owns real `WC_SCROLLBAR` children; they stay alive as the scroll
  model (`OverlayScrollbarSource::ScrollbarControl`), are hidden, and the
  overlay is given their rectangle through `SetBounds`. Do not re-show those
  controls: `ShowScrollBar(..., TRUE)` would put the platform bar back on top of
  the overlay.
- The overlay scrollbar uses TreeView scroll metadata for ordinary layout and
  paint work. Walking visible items is confined to an explicit thumb drag or
  track click, so refresh, resize, and hover do not add an O(N) tree traversal.

## Phase 5 Native Part Projection Checkpoint (2026-07-31)

- The process-local layout service is the authority for Primary Sidebar, Panel,
  and Auxiliary Bar visibility and committed extent. Native hosts subscribe to
  committed revisions and project a newly read snapshot on the UI thread.
- The pinned VS Code default is Primary Sidebar visible, Panel hidden, and
  Auxiliary Bar hidden. Outline remains a View inside the Sidebar; it is not the
  physical right-side Part.
- Native visibility commands and splitter commits mutate the model first with a
  bounded operation ID and expected revision. A rejected mutation restores the
  prior host state; a native host must not pre-commit a second layout truth.
- `CShareData` physical left/bottom/right fields are compatibility mirrors for a
  runtime-backed window. They are not inputs that can overwrite the current
  layout model.
- The bounded active-surface adapter also projects committed
  ViewContainer/View selection for Explorer, Outline, Source Control, Terminal,
  Problems, and Output. It validates the complete
  active/focus hierarchy before native mutation and applies an explicit focus
  only after native bounds exist. Activity Bar selection, Outline expansion,
  and bottom-panel tab selection are therefore projections of model truth
  rather than an independent `m_eActiveTool` authority.
- Every supported Activity Bar ViewContainer is registered into the Primary Side
  Bar, and the Auxiliary Bar registers none. That is VS Code's **default**, not a capability limit: the
  Secondary Side Bar starts empty and gains a container only when the user moves
  one there, and its toggle remains a pure Part-visibility change that never
  changes Activity Bar selection. Do not reintroduce a placeholder auxiliary
  container to make the right edge look populated.
- Relocation is a model operation. `MoveContainer` is the only authority for a
  ViewContainer's location; the native side bars project that fact and never
  hold a second one. Because a container has exactly one location, a container
  in the Auxiliary Bar has no Activity Bar entry, matching VS Code, and the
  Primary Side Bar must stop claiming it.
- The active side-bar ViewContainer and a container's active View are separate
  model facts. Activating Outline keeps `activeContainers.sideBar` at
  `workbench.view.explorer` while the Explorer container's `activeViewId` becomes
  `outline`. VS Code's Activity Bar toggle compares containers, so any code
  answering "is this Activity Bar entry already active" reads
  `activeContainers.sideBar`, not the container's active view.
- Recorded divergence (omit, don't fake): the bottom Panel header renders only
  Problems, Output, and Terminal. VS Code also contributes Ports
  (`~remote.forwardedPortsContainer`) and Debug Console (`workbench.panel.repl`)
  to this Part, and both stay in the contribution catalog, but neither gets a tab.
  Ports forwards through a Remote Development authority this fork already refuses,
  and a Debug Console reads a DAP adapter process and transport that do not exist
  here, so a tab for either could never resolve to a view. Both previously shipped
  as `WS_DISABLED` buttons; a permanently inert tab is chrome that promises a
  surface, so the container is omitted from the header instead. `PortForwardingService`,
  `DebugConsoleModel`, `DapSession`, `DapProtocolCodec`, and
  `LaunchConfigurationCatalog` are untouched and still pass their suites; only the
  header buttons and their `BottomPanelTab` selection paths were removed. Restoring
  a tab means implementing its view projection, not re-adding the button.
- Run and Debug, Ports, Debug Console, reorder within a bar, moving the whole Panel
  (`workbench.action.movePanelToSecondarySideBar`), and panel position/alignment
  remain typed unsupported boundaries. A generic contribution renderer and
  unified command/context route are still required. Moving a built-in Activity
  Bar ViewContainer between the Primary and the Secondary Side Bar is now
  supported and projected; see [`win32/CLAUDE.md`](win32/CLAUDE.md) for the
  location-set mapping and [`../window/CLAUDE.md`](../window/CLAUDE.md) for the
  drag gesture and its documented edge-strip divergence.

The original focused Part/host/state/memento/runtime filter passed 68/68. After
the active-surface integration, the state/memento/projection filter passed
45/45 and the callback/projection-separation filter passed 10/10; the x64 Debug
solution build completed with zero errors and the repository process audits
were clean.

## ViewContainer Pool/Host Boundary (2026-08-01)

- `CViewContainerPages` owns every window a page renders, not just the page's
  primary control. `Close()` destroys page-owned windows before host teardown.
  A host may render a page; it may never own one.
- A ViewContainer has exactly one location, and that location now carries every
  window the page owns, not only its primary control. Nested views such as
  Outline move with their owning Explorer container; nothing may leave a stray
  reparented copy behind in the vacated host.
- A host offers dialog-message translation only to the page content it actually
  owns. A message belonging to another surface is never offered to it, so
  Tab/arrow navigation inside one control cannot swallow input meant for another.

## Phase 3/5 Dual-Profile and Command Checkpoint (2026-07-31)

- `WorkbenchBootstrapContext` carries the pinned Control authority snapshot and
  the selected user-data profile snapshot as different values. It validates
  both, their shared authority ID/generation, the selected descriptor, and all
  URI-only profile resources before a runtime can become observable.
- Settings source identity and Settings file loading/watching/writeback use
  `UserDataProfile()`. Control endpoint and storage adapters continue to use
  `ControlProfile()` until each
  durable state key has an explicit selected-profile scope.
- The Default profile temporarily uses the explicit legacy-control-root bridge
  so existing settings are not silently abandoned. Named/transient profiles use
  opaque-ID namespaces. A durable migration marker and a control-owned stable
  empty-window identity store remain required before removing this bridge or
  supporting independently associated empty windows.
- The native Explorer entry points now route through the command/context spine
  described in `commands/CLAUDE.md`. This does not yet prove complete palette,
  menu/keybinding, or arbitrary View routing.

The integrated dual-profile/runtime/command/control-client cohort passes 79/79.
The complete Phase 3 profile-management UI and the complete Phase 5 command
surface remain open.

## Phase 6 Service Foundations Checkpoint (2026-07-31)

- `CWorkbenchRuntime` owns the process-local Marker and Output authorities.
  Their ready-only accessors return null before runtime publication and after
  stop begins; the service objects remain alive in their typed stopped state
  until runtime destruction so native borrowers cannot observe a dangling
  pointer.
- Marker and Output keep bounded state, owner-generation fencing,
  deterministic copied snapshots, committed notifications delivered outside
  locks, and callback-draining terminal Stop behavior. Runtime shutdown stops
  Output before Marker and does not publish `Stopped` while a callback-origin
  service stop is still deferred.
- `.vscode/tasks.json` and the `tasks` workspace member feed a pure bounded
  Task catalog; `.vscode/launch.json` and its workspace member feed the Debug
  launch catalog. Workspace/folder artifacts remain separate from Settings.
- Multi-root ownership stays explicit: do not select the first folder
  implicitly or put independent folder revisions into one global task/launch
  catalog.
- Canonical Problems and Output command IDs reach service-backed native panel
  projections. `CEditWnd` subscribes through an HWND-only coalescing gate,
  re-snapshots on the UI thread, and unsubscribes before panel/runtime teardown.
  The extension bridge mutates Marker/Output first and mirrors accepted state
  into legacy decoration/view caches only as a best-effort compatibility
  projection.
- Pure Task execution owns bounded run/session lifecycle through a split
  begin-close/deadline-wait contract. The injected DAP session serializes
  physical sends and owns request/transport shutdown. Debug Console and Ports
  have generation-fenced pure state authorities.
- `CTerminalSession` now provides nonblocking, idempotent `BeginClose` and an
  absolute-deadline `WaitForClose`; every externally returned terminal result
  owns quiesced backend/workers. Callback/self-destruction paths retain the
  implementation until the close worker completes and do not detach live work.
- Runtime now owns `TaskExecutionService` and an explicit per-folder Task
  catalog registry. Production injects `CTaskTerminalSessionFactory`; its
  process and shell paths preserve argument tokens and publish the real root
  exit code from the post-quiescence Terminal completion boundary. Artifact
  batching makes multi-root selection atomic and never guesses the first folder.
- This does not yet make Task output a native Terminal tab. The production
  adapter is currently composed without a presentation sink. A later
  runtime-owned terminal presentation authority must be shared by Task
  execution and the native panel; raw Task bytes must not become a second
  HWND-local or Output-channel truth.

The earlier integrated runtime/service/native-bridge cohort passed 210/210.
The later Task/Terminal checkpoint adds the production Task adapter,
folder-scoped Task catalogs, real ConPTY exit-code integration, and runtime
Task-stop fencing; exact evidence belongs in the goal-loop journal. Folder-
scoped Launch catalogs, DAP adapter processes/controllers, Debug Console and
Ports production ownership, extension Task/Debug/Ports RPC, Task Terminal
presentation, and exact Problems UTF-16 range navigation remain open gates.
