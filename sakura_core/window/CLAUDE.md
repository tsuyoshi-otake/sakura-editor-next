# P4 Window Adapter Guidance

`CEditWnd` and child HWND classes apply workbench snapshots and forward native
events. They do not own editor, command, configuration, profile, storage,
extension, or backend truth.

Multi-group/window, split, drag/drop, sidebar/panel movement, DPI changes, and
focus restoration must update the workbench model first, then apply one
snapshot. Do not add new peer-coordinate inference to `OnSize2`. Destroying or
hiding a window surface must explicitly transfer focus and must not terminate a
backend whose service owner remains active.

## Committed layout must invalidate the whole frame (2026-08-05, #17)

- A committed geometry change invalidates the frame **once, synchronously**,
  through `CEditWnd::RedrawWorkbenchFrameForCommittedLayout`. `OnSize2` and the
  `OnLButtonUp` sash-commit branch pass `immediate = true`; the remaining call
  sites are fallbacks for paths that bypass `OnSize2`.
- Children reposition through `SetWindowPos`/`MoveWindow`, which copies the old
  client bits into the new rectangle and invalidates only what became newly
  visible. Every other pixel — the area a sibling vacated and the copied bits
  themselves — stays valid and stale. Repositioning children is therefore never
  by itself sufficient to update the screen.
- **A queued invalidation is not enough, and this is measured, not assumed.**
  With `RDW_INVALIDATE` alone the stale pixels survived past 900 ms and the
  screen-versus-`PrintWindow` difference was indistinguishable from the
  uninvalidated build (6.490% / 13.367%). `RDW_UPDATENOW` took the same gesture
  to 0.000%. VS Code updates a resized or toggled frame atomically with no
  ghosting, so a one-frame synchronous commit is the compatible behavior, not an
  optimization.
- `CWorkbenchPanelHost::Layout` passes `SWP_NOCOPYBITS`, because the default
  bit-copy smears the old client content across the moved rectangle before
  `WM_PAINT` arrives.
- The helper compares the applied client and host rectangles and skips the
  repaint when nothing moved, so geometry-neutral projections (active-view
  switches, mirror updates) do not repaint the window. It also early-returns
  while `m_startupDrawState` is `Suppressing` or `Committing`: the startup draw
  transaction suppresses painting deliberately and commits one complete frame
  itself, so invalidating there would only duplicate work the commit repeats.
- Verify any change to this path with the screen-versus-`PrintWindow`
  differential method in the root [`CLAUDE.md`](../../CLAUDE.md); a layout
  assertion cannot see this class of defect, because the layout is already
  correct when it happens.

### What that repaint costs, and what does not make it cheaper (2026-08-05, #17)

Measure this path by the editor and control processes' own CPU time across a
whole gesture, drained to idle — never by the wall-clock time of the driving
`SetWindowPos`/`SendMessage` call. Those calls block until the target has
handled the message, so `RDW_UPDATENOW` pulls the paint *into* the call and
removing it pushes the same paint into the message loop just after it returns.
Per-call wall clock therefore measures when the paint happens, not how much
paint happens, and it overstates the saving of any change that defers work.
Absolute numbers also drift between sessions — the identical binary measured
45.6 ms and 31.9 ms per bottom-Panel toggle hours apart — so only A/B pairs
collected adjacently in one run are comparable.

Two attribution results bound the search space. x64 Release and x64 Debug cost
the same (10.286 ms versus 10.026 ms of CPU per resize step; 44.922 ms versus
45.573 ms per Panel toggle), so `/O2` buys nothing here and the cost lives in
GDI/USER32/DWM system work rather than in compiled C++. A bare top-level window
driven through the identical resize loop costs 0.979 ms per step, so the roughly
10 ms this window spends is our own painting, not a fixed OS charge. Hiding the
Primary Side Bar left the resize step unchanged (10.417 ms) but cut the Panel
toggle to 27.344 ms, which is what rasterization-dominated cost looks like: it
scales with the painted area, not with the number of layout decisions.

Two optimizations were implemented, measured, and rejected. Do not re-attempt
either without new evidence.

- **Gating the synchronous commit on changed geometry is a five-fold
  pessimization.** Dropping `immediate ||` from the change test, so that the
  `RDW_UPDATENOW` commit also requires the cached rectangles to differ, moved the
  bottom-Panel toggle from 43–46 ms to 220–230 ms of CPU per toggle and the whole
  24-toggle gesture from about 1.8 s to about 6.1 s, reproducibly across three
  gated and two ungated runs. The resize gesture was unaffected because its
  geometry always changes. One synchronous whole-frame paint is far cheaper than
  the many fragmented asynchronous paints that replace it.
- **Silencing per-child invalidation buys nothing.** Passing `FALSE` for
  `bRepaint` at every `MoveWindow` in `OnSize2` and deleting the 2006-era
  intermediate `UpdateWindow` calls for the status bar and function-key bar — on
  the theory that the committed frame repaint already covers them — measured
  33.203 ms per toggle and 9.375 ms per resize step against 31.901 ms and
  8.333 ms for the committed code in the adjacent pair. That is no improvement,
  and slightly worse inside the run-to-run drift.

Conclusion: inside this GDI architecture the commit's cost is essentially the
rasterization of the frame, and it cannot be reduced without shrinking the
painted area — which is exactly what caused #17. Further gains require a
different rendering architecture rather than a cheaper invalidation. Note that
nothing in the application renders through the GPU today: the editor view
composites with `CreateCompatibleDC`/`BitBlt`, and even
`terminal/window/TerminalDWriteRenderer.cpp` binds Direct2D through
`CreateDCRenderTarget` + `BindDC`, which rasterizes on the CPU into a GDI DC.

## Recovery and load projection

- Native recovery content is not selection authority. After lifecycle commit,
  prove that the Core contains exactly the expected single inactive recovered
  input, that `restoredInputId` and `effectiveActiveInputId` agree, then activate
  that exact ID and project one Core snapshot to the native surfaces.
- Core activation and native projection are distinct terminals. A failed active
  selection reports `CoreActivationFailed`; an invalid/hidden empty-editor
  projection reports `NativeProjectionFailed`. Startup fails closed and retains
  the durable backup in either case. Do not claim that this post-native-commit
  failure restores the pre-recovery native object graph.
- `CEditWnd::OnFinalLoad` owns the Core/UI finalization terminal for legacy file
  loads. Complete the pre-load persistence token only after Core adoption and
  native projection succeed. On projection failure or exception, fail-close the
  active Core input and retain the backup so native state is never published as
  an unrelated active workbench input.

## Phase 5 Live Workbench Projection

- Subscribe after the Workbench runtime is ready and before the initial layout
  snapshot is projected. The callback captures only the editor HWND and posts
  `MYWM_WORKBENCH_LAYOUT_CHANGED`; it never calls window objects or controls from
  the model callback thread.
- Reset the subscription before child-window teardown. A queued message after
  reset is stale and must be ignored. On a live notification, cancel any native
  drag and re-read the latest model snapshot on the UI thread.
- Validate Sidebar/Panel/Auxiliary Bar as one snapshot, then apply all three
  visibility/extent values. Projection failure preserves the last native state
  and is diagnostic; it is not permission to apply whichever fields happened to
  validate.
- Visibility toggles and splitter commits use fresh expected revisions and
  bounded non-wrapping operation IDs. Apply focus only after the accepted model
  snapshot has real visible native bounds.
- Runtime-backed windows mirror physical state into `CShareData` for legacy
  consumers but never hydrate physical layout from it. The no-runtime unit path
  may retain the historical shared-data behavior.
- The bounded native adapter projects committed built-in active surfaces for
  Explorer, Outline, Source Control, Extensions, Terminal, Problems, and Output.
  "Extensions" here is the whole ViewContainer — Marketplace and any
  contributed views together, see "Extensions ViewContainer Marketplace"
  below — never only its contributed-views section. Activation does not imply
  focus; an explicit coherent model focus is applied only after the projected
  native bounds exist. User-originated Outline expansion and bottom-panel tab
  selection first ask the model owner and may be vetoed; snapshot projection
  setters never call those request callbacks.
- Every Activity Bar ViewContainer, including `workbench.view.extensions`,
  starts in the Primary Side Bar host, matching VS Code's defaults. The
  Secondary Side Bar host starts empty and renders the empty-state message only
  while no container has been moved there; the Part toggle changes visibility
  alone and must never move the Activity Bar selection. Each container that
  renders in the Primary Side Bar must also project back into Activity Bar
  selection; a container that renders but never highlights its own icon is a
  defect, not a cosmetic gap.
- Both side bars share one `CViewContainerPages` pool, and a ViewContainer has
  exactly one location, so exactly one `CViewContainerHost` may claim a page at
  a time. `ApplySidebarPage` / `ApplyAuxiliaryBarPage` take the page away from
  the other host before showing it, and `CViewContainerHost::Paint` refuses to
  draw a page it does not currently own. Outline expansion is one shared model
  fact and is applied through the host that actually renders Explorer, never to
  both hosts, because the shared pool's setter early-returns on an unchanged
  value and would swallow the second relayout.
- An Activity Bar click reproduces VS Code's `ViewContainerActivityAction`. With
  the default `workbench.activityBar.iconClickBehavior` of `toggle`, clicking the
  already active, visible container runs
  `workbench.action.toggleSidebarVisibility`, and any other click opens that
  container. Hiding therefore belongs to the click gesture, never to
  `workbench.view.*`, which only ever reveals a container. The comparison follows
  `getActivePaneComposite()` and reads the active **ViewContainer** of the
  Primary Side Bar, so a nested View selection such as Outline inside Explorer
  must not change the answer. Reserve `IsBuiltinWorkbenchViewActive` for
  questions about the active View itself.
- Search, Run and Debug, Ports, Debug Console, arbitrary extension-owned views,
  reorder within a bar, moving the whole Panel
  (`workbench.action.movePanelToSecondarySideBar`), and panel alignment still
  fail closed at this adapter boundary. They remain explicit Phase 5/6 gates and
  must not be approximated with legacy active-tool state.

## Title-bar Account and Manage actions (2026-08-02)

- In VS Code's default vertical Activity Bar, Accounts and Manage are lower
  **global actions**, not ViewContainers. They move to the title bar only when
  the Activity Bar itself is moved to the top or bottom.
- **Documented divergence:** this native adapter has no Activity Bar position
  setting or typed lower-global-action section yet. It therefore retains Account
  and Manage in the title bar rather than adding them as `ActivityBarItem`s and
  falsely treating them as draggable Primary Side Bar ViewContainers. Account
  remains an explicit "No account provider configured" boundary; Manage contains
  only workbench commands with real native executors.
- When Activity Bar placement is implemented, add a typed global-action group
  and place Account/Manage at the bottom only for the vertical position, or in
  the title bar only for top/bottom positions. Do not duplicate those actions or
  add placeholder Search/Run and Debug containers in the meantime.

## Title-bar Update indicator (2026-08-06)

- The button is VS Code's `workbench.actions.updateIndicator`, title `"Update"`,
  contributed to `MenuId.TitleBarUpdate` at order 0
  (`contrib/update/browser/updateTitleBarEntry.ts`). Placing it in the title bar
  is upstream's own placement, not a divergence — unlike Account and Manage
  above, which are here because this adapter has no Activity Bar position
  setting yet.
- It is visible **only** for the three actionable states
  (`available for download`, `downloaded`, `ready`) and only while
  `update.titleBar` is true. `CCustomFrameController::SetUpdateIndicatorVisible`
  takes that answer rather than computing it; the composition root owns both
  halves of the decision because only it can see the update state and the frozen
  setting.
- It is a **labelled** button, not a glyph, so `CustomFrameLayout::updateButton`
  is measured from the caption font through
  `MeasureCustomFrameUpdateButtonWidth` instead of using the fixed compact
  control width. A hidden indicator passes width zero, and zero must leave every
  other title control's rectangle exactly where it would be without the
  indicator — that invariant is what `CustomFrameUpdateControlTest` pins.
- Prominence uses the `button.*` palette roles, not `activityBarBadge.*`; see
  [`../theme/CLAUDE.md`](../theme/CLAUDE.md) for why those are a separate role
  from `accent`.
- The frame does not know which command the press means. It invokes one callback,
  and `CEditWnd` executes `workbench.actions.updateIndicator`, whose registry
  executor resolves the current state to `update.downloadNow` / `update.install`
  / `update.restart` from the same context snapshot the button's own visibility
  clause was evaluated against. The frame therefore never holds a second copy of
  the update state.
- The gear menu contributes upstream's `7_update` group through
  `CustomFrameUpdateMenuEntry`. At most one entry is visible, matching the eight
  upstream items each gated on `updateState == '<state>'`; the four in-progress
  ones are contributed with `precondition: false` upstream and are drawn disabled
  here rather than omitted. `None` — the `disabled`/`uninitialized` case —
  contributes nothing at all.
- The window's update stack is optional. `update.*` commands are registered
  **before** it is composed, so an installation with no writable staging root or
  no readable update policy answers with a typed `Unsupported` instead of the
  `UnknownCommand` an unregistered id would produce. `InitializeUpdateProjection`
  runs after the frame callbacks exist so the first committed state paints the
  indicator immediately.
- The service subscription follows the existing HWND-only coalescing gate
  contract: the callback captures no raw `CEditWnd`, and `CloseWorkbench` calls
  `CloseUpdateProjection` — which closes the gate and joins the worker, possibly
  mid-download — before destroying the registry executors an update notification
  would otherwise reach. The same gate is what makes cancelled-restart detection
  safe; that reasoning lives in
  [`../update/CLAUDE.md`](../update/CLAUDE.md) and is not repeated here.

## Moving a ViewContainer between the side bars (2026-08-01)

- The gesture reproduces VS Code's `CompositeDragAndDrop`: an Activity Bar icon
  and a side bar **title** are both drag handles, a cross-location drop calls
  `moveViewContainerToLocation`, and the drop then opens the composite in its
  new home, which reveals that Part when it was hidden. `MoveViewContainerToEdge`
  mutates `WorkbenchLayoutStateService::MoveContainer` first, activates the
  container's View, and applies exactly one validated snapshot.
- Real VS Code's Activity Bar icon context menu contains only pin/unpin toggles.
  Relocation is drag and drop or the Command Palette quick pick. Do **not** add a
  "Move To" context-menu entry that upstream does not have.
- A container that now lives in the Secondary Side Bar has no Activity Bar icon
  at all, exactly as in VS Code: `SyncActivityBarEntries` reads the committed
  snapshot and removes the entry. A greyed-out placeholder would be a faked
  capability, not a degraded one.
- **Documented divergence:** the Secondary Side Bar is hidden by default, so
  there is no window to drop onto when it has never been revealed. VS Code shows
  an edge drop zone for exactly that case; here `HitTestSideBarEdge` falls back
  to a `kSideBarDropEdgeDip` (48 DIP) strip along either frame edge. The strip
  width is a native detail with no upstream counterpart and is scaled by the
  host DPI. Dropping outside both side bar windows and outside that strip is a
  no-op, never a guess.

## Extension-contributed ViewContainer projection (2026-08-07, #29)

`CEditWnd::SyncViewContainers` is the only place that turns contributed
ViewContainer declarations into Activity Bar icons and side bar pages. Four
invariants, all of which were violated at once by Anthropic.claude-code, which
declares the *same* container three times — once per location — and gates each
copy on `claude-code:doesNotSupportSecondarySidebar`:

- **A container's `when` is evaluated at projection time, not at registration
  time.** Registration deliberately keeps every declared container, so flipping
  the context key later reveals the other one without a re-register; the
  projection filters. A built-in container has no presentation record and
  therefore no clause, so it is always allowed. Dropping the clause is what made
  three mutually exclusive containers render as three identical icons.
- **`setContext` must re-run this projection.**
  `EExtensionWorkbenchChange::ContextKeys` exists for exactly that and is
  deliberately separate from `Commands`, so registering a command does not
  rebuild container projection. Without it the strip would only ever show the
  containers whose clauses happened to hold at register time.
- **The page pool is derived from the registry, not from the Activity Bar entry
  list.** `ProjectActivityBarEntries` renders Primary Side Bar containers only,
  so a container contributed to the Secondary Side Bar would otherwise get no
  page and its Part would render nothing. The page loop covers `Sidebar` and
  `AuxiliaryBar` and applies the same `when` filter.
- **Every icon must have a page, and the invariant is enforced, not assumed.**
  The Activity Bar click callback does nothing unless
  `CViewContainerPages::Contains` is true, so an icon whose page failed to be
  created silently absorbs clicks. `SyncViewContainers` therefore erases
  contributed entries with no page after `SyncContributedContainers`, and
  `ActivateSidebarPage` emits a diagnostic instead of returning silently when
  activation is rejected.

Consequently, installing Claude Code adds **zero** Activity Bar icons on a
default profile: its surviving container lives in the Secondary Side Bar, which
by design has no Activity Bar entry (see "Moving a ViewContainer between the
side bars" above). Its container renders the explicit
`kWebviewUnsupportedMessage` boundary, because all of its Views are
`"type": "webview"`.

## Extension `editor/title` menu projection (2026-08-06, #23)

`extension::menus::ProjectMenu(..., kEditorTitle)` is rendered by
`CEditWnd::ShowExtensionEditorTitleMenu` as a native `TrackPopupMenu`, built
recursively from the projection and reached through the product-owned command
`sakura.workbench.action.showEditorTitleActions`, which `InitializeWorkbench`
registers on the Command Palette surface. `ProjectExtensionEditorTitleMenu`
supplies the `WhenClauseEvaluator` and `CommandInfoLookup` from
`m_extensionService->Contributions()`, `EvaluateWhenClause()`, and
`SearchCommands()`; a selection dispatches through
`DispatchRegisteredCommandPaletteSelection` and falls back to
`m_extensionService->ExecuteCommand()`.

Two divergences, both real and neither hidden:

- **The trigger is the Command Palette, not the tab bar.** In VS Code
  `editor/title` is the icon row at the top-right of the editor tab bar, owned
  here by `CTabWnd` / `DocumentTabActionLayout.h`. Those files were outside the
  editable scope of the delivery that built this, so the projection is currently
  reachable only by command. This is a **placement divergence against the
  repository's highest-priority rule and is therefore temporary, not a settled
  design.** The projection, the `when`-clause filtering, the separator and
  submenu rules, and the dispatch are the real ones; only the surface that opens
  them is wrong. Moving it to the tab-bar toolbar is the completion of this work,
  not an enhancement of it. Do not treat the Command Palette entry as the final
  answer, and do not delete it until the tab-bar surface actually renders.
- **`altCommandId` is projected but not honored.** `SProjectedMenuItem` carries
  it, and the generic overload resolves it exactly as the palette path does, but
  a standard Win32 popup menu has no Alt-modifier detection without a message
  hook. The item shows its primary command; holding Alt changes nothing.

## Extension hover seam injection (2026-08-07, #23)

`CEditView` renders the `registerHoverProvider` popup and owns the dwell/poll
gesture, but it must never own an extension-host connection. The three
`std::function` seams it exposes (`HoverRequestHandler`, `HoverCancelHandler`,
`HoverResultPoller`) therefore have exactly one production supplier:
`CEditWnd::WireExtensionHoverHandlers()`. The rendering-side contract lives in
[`../view/CLAUDE.md`](../view/CLAUDE.md) and is not restated here.

- The helper is **idempotent and total**: it walks `GetAllViewCount()` and calls
  `SetHoverHandlers` on every pane, so re-running it after a split re-arms the
  new pane and merely overwrites the existing ones. `SetHoverHandlers` calls
  `CancelHoverTracking()` first, so an in-flight dwell is folded rather than
  orphaned.
- With no `m_extensionService` it installs **empty** handlers rather than
  skipping the call. An empty request handler is how `UpdateHoverTracking`
  learns this window is not a hover target, so a service-less window never arms
  a dwell timer whose request nothing could answer. Silently leaving stale
  handlers in place would be the fake-capability failure mode.
- Call sites are exactly two: `InitializeWorkbench()` (after the service is
  constructed, immediately before `EnsureNotificationHost()`) and
  `CreateEditViewBySplit()` (right after `m_nEditViewCount` is updated, before
  the new pane's HWND array is assembled). `Create()` builds view 0 before
  `InitializeWorkbench()` runs, so `GetAllViewCount()` is already non-zero at
  the first call.
- Teardown needs no counterpart. `CloseWorkbench()` calls
  `m_extensionService->Shutdown()` and then resets the pointer *before* the
  views are destroyed, and every captured lambda re-checks `m_extensionService`
  on entry, so each seam degrades to a no-op for the rest of the window's life.
- The document identity passed to `RequestHover` is the established
  `SExtensionDocumentId{ ::GetCurrentProcessId(), 1 }` convention already used
  elsewhere in this file. Do not introduce a second document-identity scheme for
  hover; a per-editor identity is a document-bridge change, not a window one.

## File Menu "Open Recent" (2026-08-01)

- VS Code's File menu exposes exactly one `Open Recent` submenu
  (`MenubarRecentMenu`), listing recent folders/workspaces first, then a
  separator, then recent files. This repository therefore has one
  `F_FILE_OPENRECENT_SUBMENU` node containing `F_FOLDER_USED_RECENTLY`,
  a separator, and `F_FILE_USED_RECENTLY`, replacing the two legacy sibling
  submenus. `F_FILE_RCNTFILE_SUBMENU` / `F_FILE_RCNTFLDR_SUBMENU` survive only
  as the tray menu's own labels.
- `CShareData_IO::MergeMainMenuOpenRecent` performs the version-6 migration by
  rewriting the exact legacy four-entry run in place. The entry count never
  changes, so `m_nMainMenuNum` and `m_nMenuTopIdx` stay valid, and any menu that
  does not match that exact shape is left untouched rather than repaired.
- A menu never renders a separator for an empty group.
  `RemoveRedundantMenuSeparators` drops leading, trailing, and consecutive
  separators after the model is projected, so an empty MRU list cannot leave a
  dangling rule. A submenu that becomes empty is then greyed out by
  `CheckFreeSubMenu`, exactly as before.
- **Documented divergence:** VS Code's Open Recent also carries
  `Reopen Closed Editor`, `More...` (`Ctrl+R` quick pick), and
  `Clear Recently Opened`. None of them has a Sakura equivalent yet, so they are
  absent rather than faked, and an Open Recent with no history is disabled
  instead of showing an always-enabled but inert menu.

## Extensions ViewContainer Marketplace (2026-08-01)

- Real VS Code's `workbench.view.extensions` *is* the Extensions Marketplace, so
  the OpenVSX pane is built as that container's own content rather than a
  separate floating dock. `InitializeWorkbench` supplies
  `CViewContainerPages::MarketplaceFactory` only when `m_workbenchRuntime !=
  nullptr`, constructing `CExtensionPane` from
  `Bootstrap().UserDataProfile().SelectedProfileId()`. OpenVSX is profile-scoped:
  with no runtime there is no profile authority, so no factory is supplied
  rather than guessing a profile, and a factory whose pane fails to open returns
  `nullptr`. Both are the same explicit absence; neither is backfilled with a
  placeholder pane.
- Activating the Extensions container puts focus in the Marketplace search box,
  matching real VS Code's Extensions view. Without a Marketplace (no runtime),
  activation falls back to the contributed-views control instead of focusing
  nothing.
- The former floating dock is retired. `m_pcExtensionPane`,
  `m_bExtensionPaneShown`, their `EndLayoutBars` / print-preview show-hide
  branches, and the `OnSize2` block that shrank `editorBounds` for the dock are
  gone. `ShowExtensionsViewContainer()` / `IsExtensionsViewContainerActive()`
  replace `ToggleExtensionPane()` / `IsExtensionPaneVisible()`. OpenVSX now
  renders in exactly one place, the same way Explorer and Source Control do.
- `F_EXTENSION_LIST` reveals the Extensions ViewContainer; it does not toggle a
  dock. Real VS Code's `workbench.view.*` commands only ever reveal a
  container, and hiding belongs to the Activity Bar click gesture (see "Moving
  a ViewContainer" above). The command's checked state reads
  `IsBuiltinWorkbenchViewActive(ids::view::Extensions)`, so it reflects
  whichever Part currently owns the container, including after a drag to the
  Secondary Side Bar.
- The Marketplace now projects the VS Code Extensions view's primary
  interaction shape: its search cue is `Search Extensions in Marketplace`,
  results are grouped into Installed/Popular-or-search sections, rows are
  card-like extension summaries, and selecting a row opens the typed native
  metadata surface owned by `CEditWnd`. The selection, install, and close
  paths remain connected to the real `CExtensionPane`/OpenVSX flow.
- **Documented divergence:** this is still not the full VS Code
  `workbench.editor.extension` input. OpenVSX remains the registry, and the
  composition root does not yet supply gallery icons, nor does it provide
  feature/changelog/extension-pack content, recommendations/outdated groups,
  or the filter quick-pick. README Markdown is fetched through the bounded
  OpenVSX text path, passed to the typed detail surface, and rendered as a
  native, non-executable projection; loading/error/unsupported states remain
  explicit. It is shown only when no native document input is active, so it
  never displaces an open Sakura document.
  Auxiliary-Bar Sessions is a separate VS Code Chat view and is intentionally
  not faked as part of Extensions.
- Installing through this Marketplace now takes effect in the running window.
  `CViewContainerPages`' factory hands `CExtensionPane` an install-completed
  callback owned by the composition root, and `CEditWnd` turns that into
  `CExtensionService::RequestInstalledExtensionRescan()`, so a newly installed
  extension activates without a restart. The pane still never reaches into
  `CExtensionService`; see [`../extension/CLAUDE.md`](../extension/CLAUDE.md)
  for the rescan and re-registration contract.
- The OpenVSX production factory's missing credential provider is recorded in
  [`../extension/CLAUDE.md`](../extension/CLAUDE.md); do not restate it here.

## Phase 5 Native Command Route Checkpoint (2026-07-31)

- A runtime-backed window owns one `WorkbenchContextKeyService` and
  `WorkbenchCommandRegistry`. Destroy the registry/context before the native
  hosts captured by their executor callbacks.
- Explorer Command Palette and inactive Activity Bar activation execute the
  reveal-only `workbench.view.explorer`. Legacy `F_TOGGLE_LEFT_EXPLORER`
  dispatch from the title-bar control, View menu, and `Ctrl+B` executes
  `workbench.action.toggleSidebarVisibility`. Strip only the base 16-bit
  function code for alias comparison; preserve source/high-bit flags until that
  boundary.
- Explorer TreeView item handles are transient refresh projections. Retain
  expansion by stable filesystem path across watcher refreshes, treat setting
  the same workspace root as a no-op, and activate files on single click or
  Enter while directory clicks remain navigation-only.
- Once the registry recognizes a stable command, Disabled, Unsupported, Failed,
  or context-refresh failure is terminal and must not fall through to the
  historical native action.
- Native executors commit the layout/view model first and apply one validated
  current snapshot. The same snapshot refreshes core context keys; no local
  button selection or `CShareData` field may become a competing command state.
- Production OpenVSX receives
  `Bootstrap().UserDataProfile().SelectedProfileId()`, not the Control authority
  profile ID.
- Problems and Output native requests route through
  `workbench.actions.view.problems` and
  `workbench.action.output.toggleOutput`. Extension `OutputChannel.show` uses
  the show-only executor, so an already visible Output panel remains visible.
  Runtime-backed windows borrow the runtime Marker/Output services, subscribe
  through an HWND-only coalescing gate, and re-snapshot on the UI thread.
  Service callbacks never capture raw `CEditWnd`; teardown closes the gate and
  unsubscribes before destroying the panel or stopping the runtime.
- Problems and Output content is service-backed. Projection setters remain
  callback-free, Output selection mutates the model before local selection, and
  a `ChannelShown` change reveals Output while honoring `preserveFocus`.
  Legacy diagnostics repaint only current-document decorations and never become
  Problems authority.
- Problem activation currently opens the retained URI only. Applying its exact
  range is unsupported until a dedicated UTF-16-code-unit to Sakura-position
  adapter exists; do not approximate it with byte or display columns.

The integrated runtime/service/native-bridge cohort passes 210/210 and compiles
through the native window. Command Palette, arbitrary extension view
contributions, Debug Console, Ports, and Run and Debug still require production
wiring.

## Status Bar Notifications and Visibility (2026-08-03)

- The rightmost entry is the built-in `status.notifications` item. It executes
  `notifications.showList` / `notifications.hideList`, shows the bell-dot glyph
  while unread notifications exist, and remains present even when every other
  status item is hidden. A timed-out toast retracts only its transient popup;
  the notification remains in the center until its explicit close or action.
- Status-bar customization is model-backed. Right-clicking the bar exposes the
  VS Code-shaped checked entry list, a context-specific Hide action, and Hide
  Status Bar. Hidden stable IDs are stored in the selected profile's User scope
  under `workbench.statusbar.hidden`; an extension item uses
  `<extensionId>.<itemId>` and its contributed `name` is the menu label.
- Use upstream stable IDs when VS Code owns the concept, including
  `status.workspaceTrust`, `status.scm`, `status.editor.selection`,
  `status.editor.eol`, `status.editor.encoding`, `status.editor.inputMode`,
  `status.editor.zoom`, and `status.notifications`.
- The native status bar must not use `SBARS_SIZEGRIP`. VS Code anchors its
  rightmost item directly to the status-bar client edge; reserving a legacy
  resize-grip width leaves the notifications icon visibly too far left.
- Resizing belongs to the custom frame, not the removed status-bar grip.
  `WM_NCHITTEST` must expose every edge and corner, including the invisible
  outer half of the DWM resize border after custom `WM_NCCALCSIZE`; maximized
  windows continue to suppress resize hits. Because custom `WM_NCCALCSIZE`
  makes the entire frame client-owned, topmost input-only child overlays must
  cover all four inside edge strips and forward their initial press to the
  top-level non-client resize path. Pure `WM_NCHITTEST` geometry is not enough:
  Activity Bar, editor, panel, and status child HWNDs otherwise receive the
  press first and leave only the legacy bottom-right grip reachable.
  **Documented divergence:** Sakura's character-code display and macro-recording
  indicator have no VS Code counterparts. Their IDs are therefore explicitly
  product-owned as `sakura.status.editor.characterCode` and
  `sakura.status.macroRecording`; do not disguise them under unrelated VS Code
  IDs merely to make the customization menu look canonical.

## Restricted Mode status-bar entry (2026-08-07, #36)

- The far-left item is VS Code's `status.workspaceTrust`: a `$(shield)
  Restricted Mode` label that appears **only** while the workspace is not
  trusted and clicking it runs `workbench.trust.manage`. It renders further
  left than `status.scm`, exactly where upstream places it, and shifts the SCM
  block's own origin right by its width when visible — `PaintStatusBar` seeds
  `scmWidth` from the trust item's measured width instead of `0`.
- Visibility gate: `IsStatusbarEntryVisible("status.workspaceTrust")` **and**
  `m_workspaceTrustState != config::EWorkspaceTrustState::Trusted`. Both
  `Unknown` and `Untrusted` paint as restricted, matching the three-state
  `config::EWorkspaceTrustState` contract in `config/CLAUDE.md`: withheld trust
  is never fabricated into a false `Trusted`, and an un-refreshed window
  (`Unknown`) must not silently claim to be trusted either.
- `CEditWnd` pushes the live value through
  `CMainStatusBar::SetWorkspaceTrustState(config::EWorkspaceTrustState)`, a
  callback-free projection setter in the same family as
  `SetScmStatusCommands`/`SetNotificationState` — it only stores state and
  invalidates the bar, it never invokes a command or notifies an owner. The
  push happens inside `RefreshWorkbenchCommandContext()`, right after that
  function reads `WorkspaceContext().Snapshot()` for the core context keys;
  that function already runs at every command dispatch and workspace/folder
  transition, so trust state and the core context keys can never disagree
  about what "trusted" means, and no second workspace-context subscription was
  added just for painting.
- This is the one status-bar item that paints its own fill: every other item
  rides the bar's single flat `FillSolidRect(target, client, m_palette.accent)`
  background, but this one fills its own rectangle with the new
  `m_palette.statusBarProminentBackground` role (VS Code's
  `statusBarItem.prominentBackground`) before drawing its label runs. See
  `theme/CLAUDE.md` for why that role could not reuse an existing one. The
  label foreground reuses the existing `m_palette.highlightText` role — no new
  foreground role was needed, because `highlightText` already equals
  `statusBarItem.prominentForeground`'s upstream default (`#FFFFFF`) for both
  dark and light.
- The click target routes through the same generic path `status.scm` uses:
  `InvokeBuiltinItemAt` dispatches any hit target's non-empty `command` string
  to `m_workbenchCommandCallback` without a special case, so pushing
  `{ "status.workspaceTrust", itemRect, "workbench.trust.manage" }` onto
  `m_statusbarHitTargets` was sufficient; no new routing branch was added.
  `workbench.trust.manage` was already registered
  (`WorkbenchCommandRegistry.cpp`) and its executor
  (`CEditWnd::ExecuteManageWorkspaceTrust`) opens the native Workspace Trust
  editor page — see "Workspace Trust editor page" below for the window-side
  facts and `config/CLAUDE.md` for the recorded divergences from VS Code's own
  rich trust editor page; neither is repeated here.
- **Unproven by unit test, deliberately and on the record.** The entry's
  presence/absence by trust state, its click command, and the
  `status.notifications`-stays-rightmost invariant have **no** automated
  coverage. `PaintStatusBar` takes an `HDC` and reads `m_hwndStatusBar`, the
  palette, and the icon-font registry, and the entry descriptor list is a local
  array inside `CEditWnd::RefreshStatusbarPresentation` — so proving any of this
  needs a real window, which is exactly the shape `EditWndTest.*` has and why it
  is excluded from the unattended smoke filter. `StatusbarViewModelTest` covers
  only the pure view model and knows no specific entry IDs, so it does not
  reach this either. Do not read the green cohort as evidence that this item
  paints correctly; verify it with the screen-versus-`PrintWindow` differential
  method in the root [`CLAUDE.md`](../../CLAUDE.md), or close the gap by
  extracting a testable seam, before relying on it.
- VS Code shows Restricted Mode in *two* places at once — this status-bar
  entry and the `workbench.parts.banner` (`RestrictedModeAction`'s banner
  beneath the title/menu bar). See "Restricted Mode banner Part" below for the
  second signal; this status-bar item's hide/show correctness still matters on
  its own, independent of whether the banner happens to be visible.

## Restricted Mode banner Part (2026-08-07, #38)

- `CWorkbenchBannerHost` (`workbench/CWorkbenchBannerHost.h`/`.cpp`) is a
  `CEditWnd`-child window that paints `workbench.parts.banner` and nothing
  more: it measures its own text, draws the message and action links it was
  handed, and reports clicks. It never decides its own visibility.
  `CWorkbenchRuntime::UpdateRestrictedModeBannerVisibility` is the sole
  authority: it treats both `Unknown` and `Untrusted` workspace trust as
  restricted (the same three-state contract as the status-bar entry above),
  reads the profile-scoped `security.workspace.trust.banner` setting, and
  writes the answer through `WorkbenchLayoutStateService::SetPartVisibility`.
  `CEditWnd::ApplyCurrentWorkbenchLayoutState` then projects that committed
  Part state onto the host with `Show()`/`Hide()` — the same
  authority-writes-model / adapter-projects-snapshot split every other Part in
  this file follows.
- `BuiltinPartProjection`'s `std::optional<BuiltinBannerProjectionState>
  banner` keeps "unregistered" (`nullopt`) and "registered but hidden"
  (engaged, `visible == false`) as two different facts, even though the native
  result is identical either way (the window stays hidden). `CEditWnd` reads
  `projection.parts.banner && projection.parts.banner->visible` rather than
  collapsing the optional into a bool, because a caller that only ever needs
  "is it on screen" is not the only caller this projection has to serve.
- The message follows workbench state, matching upstream
  `WorkspaceTrustUXHandler.getBannerItem`: `config::EWorkspaceKind` (Empty /
  Folder / Workspace) selects "Trust this window / folder / workspace to
  enable all features," appended to a fixed lead sentence carrying the
  banner's own `$(shield)` icon as a label run — the same icon vocabulary the
  `status.workspaceTrust` status-bar item uses, so one `$(name)` token cannot
  render two different ways depending on which surface drew it.
- The only action offered is `Manage` (`workbench.trust.manage`), and only
  while that command is actually registered in the workbench command
  registry. This follows `CWorkbenchBannerHost`'s own contract: the host draws
  exactly the actions it is given, so an action nothing could perform must be
  withheld by the caller rather than drawn dead.
- **Documented divergence — two upstream affordances are absent, not faked:**
  - `Learn More` is an `href` to an external documentation URL upstream; this
    product has no `env.openExternal` equivalent yet.
  - The close/dismiss button is absent because there is no durable
    per-workspace dismissal store to back upstream's `onClose`, which writes
    an `untilDismissed` memento. This is the same gap that makes
    `UpdateRestrictedModeBannerVisibility` treat a stored
    `security.workspace.trust.banner` value of `"untilDismissed"` as
    `"always"` (fail-closed): a session-only dismissal would hide the only
    banner-side Restricted Mode signal while claiming to have remembered a
    choice it never persisted.
- Layout: the banner sits directly below the title bar and above everything
  else, spanning the full client width — VS Code's own stacking order. It has
  no sash and no persisted extent: its height is content-driven
  (`CWorkbenchBannerHost::PreferredHeightPixels` measures the current text),
  so there is no user-adjustable size to save or expose, unlike the Sidebar/
  Panel/Auxiliary Bar extents elsewhere in this file. `OnSize2` positions the
  banner, toolbar, and function-key bar from `chromeLayout` — an early
  `CalculateWorkbenchLayout()` evaluation — instead of deriving the toolbar's
  top from `nCustomTitleHeight + nToolBarHeight`: once the banner can occupy
  that band, that peer-coordinate arithmetic would be wrong the moment the
  banner exists, and this function already forbids adding new peer-coordinate
  inference (see the rule at the top of this file).
- `RefreshRestrictedModeBannerContent()` sets the message and action list and
  is called from two places: once in `InitializeWorkbench`, immediately after
  `RefreshStatusbarPresentation()` — deliberately after the workbench command
  registry is built, because calling it earlier would permanently withhold
  `Manage` — and again from `ApplySemanticWorkspaceContext()`, so the message
  tracks the workspace kind across folder/workspace transitions instead of
  freezing at startup.

### Painted-result verification (2026-08-07, x64 Debug)

Because this Part occupies a chrome band that `OnSize2` now derives from
`chromeLayout`, it was verified with the screen-versus-`PrintWindow`
differential method in the root [`CLAUDE.md`](../../CLAUDE.md), not with a
layout assertion. The window ran under a throwaway `-PROF=` profile whose
`settings.json` carried `security.workspace.trust.emptyWindow: false`, which is
what makes an empty window resolve to `Unknown` trust and therefore restricted;
the profile was deleted afterwards. The window was made topmost for the run —
`SetForegroundWindow` from the measuring process is refused, and without it the
occlusion grid rejected every trial.

| Gesture | Valid trials | Stale median | Stale max |
|---|---:|---:|---:|
| Forced full `RedrawWindow` (noise floor) | 4/4 | 0.0000% | 0.0000% |
| Bottom-Panel toggle, banner visible | 10/10 | 0.0000% | 0.1281% |
| Frame resize, alternating sizes, banner visible | 12/12 | 0.0000% | 0.0000% |
| Banner shown/hidden live | 6/8 | 0.0000% | 0.0000% |

The single 0.1281% panel trial is not a stale-pixel defect: its heat map
localizes the whole difference to the TERMINAL prompt line, which the just-started
ConPTY shell printed between the two captures. Two banner trials were discarded
by the occlusion grid, as designed.

The banner gesture is a real product path, not a test hook: rewriting the
profile-scoped `security.workspace.trust.banner` value makes the watched
settings document reach `UpdateRestrictedModeBannerVisibility`, and the
`SakuraWorkbenchBannerHost` window was confirmed to move between
`visible=False (1600x0)` and `visible=True (1600x26)` across it. The status-bar
`status.workspaceTrust` entry stayed visible in both states, which is the
observable proof that the two Restricted Mode signals are independent.

## Workspace Trust editor page (2026-08-07, #39)

- `CWorkspaceTrustEditorSurface`
  (`workbench/editor/CWorkspaceTrustEditorSurface.h`/`.cpp`) is a
  `CEditWnd`-owned composition-layer surface, the same family as
  `CExtensionDetailSurface` and `CDiffSurface`: not an `EditorInput`, no
  document model, native GDI painting and `WC_BUTTONW` children only — no
  WebView2, no HTML. `CEditWnd` constructs it alongside those two other
  surfaces, wires its two callbacks (`SetOnGrantRequested` to
  `PerformWorkspaceTrustGrantFromPage`, `SetOnCloseRequested` to
  `ClearWorkspaceTrustPage`), and destroys it before the window itself tears
  down. The surface never calls `IWorkbenchRuntime` directly: it renders an
  already-resolved `workbench::WorkspaceTrustPromptModel` the composition root
  hands it through `ShowPrompt`, and only reports a result back through
  `SetGrantResult` after `CEditWnd::PerformWorkspaceTrustGrantFromPage` has
  actually performed the grant against the runtime. The decision itself stays
  entirely outside the surface, exactly as `config/CLAUDE.md`'s Workspace
  Trust Resolution Checkpoint requires for every trust decision.
- **Projection precedence.** `ApplyEditorCoreSnapshot` and the
  `LayoutMarkdownPreview` branch that `OnSize2` drives both apply the same
  fixed order whenever no document input is active: the trust page, then the
  diff comparison, then the extension metadata surface, then the empty-editor
  watermark. Exactly one of the four is shown at a time, and every non-winner
  is explicitly hidden alongside the winner rather than left in whatever state
  it was last in. Trust ranks first because, unlike the other three, it
  describes whether the window may run anything at all, not merely what it is
  showing in place of a document.
- **Cross-retraction.** Each of the other three composition-layer surfaces
  retracts the trust page (`ClearPrompt()` + `Hide()`) the moment it becomes
  the new projection winner — `ShowDiffSurface`, the Marketplace pane's
  `SetOnExtensionSelected` handler, and `ApplyEditorCoreSnapshot`'s
  document-input branch all do this explicitly, so a stale prompt cannot
  resurface the next time the editor group goes empty again. Symmetrically,
  the trust page's own entry point, `ShowWorkspaceTrustPage`, retracts the
  diff and extension-detail surfaces before showing itself. No two of the four
  projections can claim to be the active one at once.
- **The command is refused, never downgraded, when a document is open.**
  `ExecuteManageWorkspaceTrust` reports `Unsupported` when the window has no
  page at all and `NotApplicable` when `ShowWorkspaceTrustPage` refuses because
  `HasActiveEditorInput()` is true; it never falls back to a modal in either
  case.
- **The startup prompt is a separate message reaching a separate function, not
  this page.** VS Code's `requestWorkspaceTrust` startup prompt
  (`security.workspace.trust.startupPrompt`) is unrelated upstream surface
  area from `workbench.trust.manage`, and the two must never substitute for
  each other here. `CEditWnd::PostWorkspaceTrustStartupPromptOnce`, called
  from the end of `CommitStartupDrawTransaction()`, posts
  `MYWM_WORKSPACE_TRUST_STARTUP_PROMPT` (`WM_APP+245`,
  `config/system_constants.h`) at most once per window; the window procedure
  dispatches that message to `ShowWorkspaceTrustStartupPrompt`, which shows a
  `TaskDialogIndirect` modal, not this page. See `config/CLAUDE.md`'s
  `security.workspace.trust.startupPrompt` section for the once/always/never
  policy and the durable per-workspace record behind it.

## Untrusted file load gate (2026-08-07, #39)

- `CEditWnd::RequestUntrustedFileLoad(std::wstring_view path)` is the load-path
  enforcement for `security.workspace.trust.untrustedFiles`, VS Code's
  `requestOpenFilesTrust`. `CLoadAgent::OnCheckLoad` calls it immediately before
  the `next:` label, so a reload (`bRequestReload`) never re-prompts. It is a
  public member, not private like the other command-executor methods in this
  file: `CLoadAgent` is not a member or friend of `CEditWnd`.
- The decision comes from `IWorkbenchRuntime::WorkspaceTrustUntrustedFiles`
  (see `config/CLAUDE.md`'s `security.workspace.trust.untrustedFiles` entry for
  the resolution rules) with `allResourcesTrusted` computed from
  `WorkspaceTrustCoversResource(path)`. `Prompt` shows a `TaskDialogIndirect`
  modal offering "Open" / "Cancel"; accepting calls
  `RecordUntrustedFilesAccepted()` before returning `Allowed`. `Open` and
  `OpenInNewWindow` (this shell reports `newWindowSupported = false`, so the
  setting resolves `Unsupported` rather than silently widening) both return
  without a dialog; `Unsupported` and `Prompt`-then-Cancel return `Refused`. An
  empty @p path (an untitled buffer) is always `Allowed`.
- **Documented divergence — broader trigger than upstream.** Upstream's
  `requestOpenFilesTrust` only runs for `validateTrust`: OS shell-open and
  command-line file arguments into an existing window. This gate instead runs
  for every load `CLoadAgent::OnCheckLoad` sees, including File > Open and
  drag-and-drop. A narrower trigger would leave those paths pulling untrusted
  content into a trusted window with no gate at all, which is the exact outcome
  this setting exists to prevent; broadening the trigger is therefore the
  safer divergence, not an unfaithful one.
- **Documented divergence — no "Open in Restricted Mode" button and no
  "Remember my decision for all workspaces" checkbox.** Upstream's dialog
  offers both. This one does not: there is no per-load Restricted Mode
  variant to open into (Restricted Mode is a whole-window state here, not a
  per-file one — see `RestrictedModeAction`'s banner/status-bar entries
  elsewhere in this file), and there is no durable cross-workspace memento
  store to back a permanent "for all workspaces" choice, only the existing
  per-workspace `WorkspaceTrustMemento::untrustedFilesAccepted`.
- **Empty-window acceptance is session-only, not durable, and this is a real
  gap, not a simplification.** `RecordUntrustedFilesAccepted()` always sets
  `CWorkbenchRuntime::m_untrustedFilesAcceptedInSession` before attempting the
  durable write, because an empty window has no workspace identity to key
  `WorkspaceTrustMemento` on (`SaveWorkspaceTrustMemento` requires
  `m_workspaceTrustMementoReadable`, which `NoWorkspaceScope` never sets). The
  in-memory flag is OR'd into `WorkspaceTrustUntrustedFiles`'s acceptance check
  alongside the durable record, so the rest of the session stops re-prompting,
  but the process restarting or another window over the same loose files both
  ask again.

## Extension Status Bar Item Hovers (2026-08-01)

- Extension-contributed status bar items get a real **hover**, not a Win32
  tooltip. `CMainStatusBar` owns one `workbench::hover::CHoverWidget` — this
  repository's `HoverWidget` implementation, see
  [`../workbench/hover/CLAUDE.md`](../workbench/hover/CLAUDE.md) — created in
  `CreateStatusBar()` and destroyed in `DestroyStatusBar()` / `WM_NCDESTROY`.
  The former `TOOLTIPS_CLASSW` popup, its `TTF_SUBCLASS` tools,
  `SyncExtensionTooltips()`, and `ExtensionTooltipPlainText()` are all gone.
  They were replaced rather than extended: a common-control tooltip carries one
  plain-text string, so headings, emphasis, inline code, theme icons, and a
  column-aligned table are not "hard" there but impossible, and the previous
  plain-text projection left visible markup debris (`| │`) in
  `odangoo.otak-usage`'s two-brand table where VS Code shows a clean grid.
- The window layer owns the *gesture*, the hover owns the *rendering*.
  `WM_MOUSEMOVE` arms a `workbench::hover::kHoverDelayMilliseconds` (500 ms,
  VS Code's `workbench.hover.delay` default) timer through
  `OnExtensionHoverMouseMove`; the timer's `ShowExtensionHoverNow` re-hit-tests
  the *current* cursor position, parses that item's Markdown, and shows the
  widget anchored above the item's screen rectangle. `WM_MOUSELEAVE` (armed via
  `TrackMouseEvent`), `WM_LBUTTONUP`, `SetExtensionItems`, and window teardown
  all call `HideExtensionHover`, which always kills the timer.
- `ExtensionHitTarget` stores the **raw Markdown**, not a rendered projection,
  and `workbench::hover::Parse` runs only at show time. A repaint therefore
  never re-parses, and the parse sees the `tooltipSupportsThemeIcons` flag that
  came with the item. An item whose tooltip is empty gets no hover at all;
  `FindHoverTargetAt` skips those targets outright.
- The pending/visible target is identified by its client-coordinate `RECT`
  compared with `::EqualRect`, never by an index into `m_extensionHitTargets`.
  That vector is rebuilt on every paint, so an index would silently point at a
  different item after any repaint. Re-entering the same rectangle keeps the
  existing timer or the already-visible hover instead of restarting it, so an
  unrelated repaint (theme change, resize, value refresh) cannot dismiss an open
  hover.
- `SetPalette()` and `SetExtensionIconFonts()` forward to the widget, so the
  hover follows the theme and resolves `contributes.icons` glyphs the same way
  the status bar text does. Items with no `command` keep their hover but do not
  get the hand cursor and are not click targets, unchanged from before.
- **Documented divergence — the hover is non-interactive.** It is created
  `WS_EX_TRANSPARENT` and returns `HTTRANSPARENT`, so a `command:` link inside it
  renders as link-colored text but cannot be clicked, and the pointer cannot rest
  inside the hover to keep it open. This is deliberate: a mouse-accepting popup
  covers its own anchor, producing a leave/re-enter flicker loop. The reasoning,
  the upstream mechanism it stands in for, and the Markdown-side divergences
  (table borders, HTML entity coverage, bounded untrusted input) all live in
  [`../workbench/hover/CLAUDE.md`](../workbench/hover/CLAUDE.md); do not restate
  them here.
- **Corrected record — `supportThemeIcons` now survives the trip to native
  code.** An earlier version of this section stated that
  `vscode.MarkdownString.supportThemeIcons` was dropped by the exthost shim's
  `serializeThemeValue` before the tooltip reached
  `CExtensionWorkbenchDispatcher`, and that native code therefore had no way to
  tell an intended codicon from literal `$(name)` text — which is why the old
  projection stripped `$(name)` tokens unconditionally. That gap is closed. The
  flag is carried through `src/exthost/src/vscode-api.cjs`, the dispatcher, and
  `SExtensionStatusBarItem::tooltipSupportsThemeIcons` into
  `workbench::hover::SParseOptions`, so `$(name)` renders as a glyph when the
  extension asked for icons and stays literal when it did not — matching VS Code
  in both directions instead of guessing one.

## Extension-Contributed Status Bar Icons (2026-08-01)

- `StatusBarItem.text`'s `$(name)` tokens now resolve against
  `workbench::icons::CExtensionIconFontRegistry` — this repository's
  `contributes.icons` implementation — before falling back to the built-in
  codicon table. `CEditWnd` owns the registry and lends it to `CMainStatusBar`
  by raw pointer through `SetExtensionIconFonts()`; the status bar never owns
  it, never extends its lifetime, and treats `nullptr` as "no contributed
  icons, built-in table only." The registry is constructed *before*
  `CExtensionService` because `contributes.icons` is satisfied entirely by the
  manifest plus a font file on disk: a dead or unreachable extension host must
  not change which glyph an installed extension's status bar item draws.
- **Resolution order matches real VS Code, globally, not per extension.**
  Upstream `contributes.icons` registers into the single global
  `IconRegistry` (`platform/theme/common/iconRegistry.ts`) keyed by the icon id
  alone, and `ThemeIcon.fromString("$(id)")` resolves that id no matter which
  extension is rendering. `workbench::icons::ResolveThemeIcon` therefore calls
  the single-argument `Find(iconId)` overload. Do not "fix" this into a
  per-extension lookup: an extension using an id another extension contributed
  is a legitimate upstream state, not a bug.
- **The resolution vocabulary is shared with the hover, not duplicated.** The
  contributed-first/built-in-fallback decision lives in
  [`../workbench/icons/ThemeIconResolver.h`](../workbench/icons/ThemeIconResolver.h)
  as pure functions, and both `CMainStatusBar`'s `StatusBarItem.text` renderer
  and `CHoverWidget`'s inline icon runs call it. The former
  `ResolveExtensionStatusIcon` member of `CMainStatusBar.cpp` was extracted into
  that header for exactly this reason; an icon id must not be able to mean one
  thing in the item's text and another in its hover.
- `CEditWnd::RefreshExtensionIconFonts()` is the only place that populates the
  registry, and it is UI-thread-only. It enumerates through
  `CExtensionManager::EnumInstalled()` and must **not** read
  `CExtensionService::m_installedRoots`, which is worker-thread-only state. It
  runs once at composition and again from the install-completed callback, so an
  extension installed mid-session gets its glyphs without a restart, matching
  the rescan contract in [`../extension/CLAUDE.md`](../extension/CLAUDE.md).
- The status bar caches one `HFONT` per (face name, pixel height) pair in
  `m_iconFontCache` so repaints do not call `CreateFontIndirectW`. The cache is
  named for what it holds — icon-font handles — not for one of its two sources,
  because it now serves both extension-contributed faces and the bundled
  `codicon` face. `SetExtensionIconFonts()` and `DestroyStatusBar()` both
  release the cache; the release on registry swap is mandatory, not defensive,
  because a face name whose memory font has been unregistered would otherwise
  still resolve through GDI's font substitution and silently draw an unrelated
  glyph. Teardown clears the borrow explicitly rather than relying on
  `CEditWnd`'s member declaration order.
- **Built-in `$(name)` resolves through the bundled `codicon.ttf`, the same way
  real VS Code does.** VS Code ships the whole font and draws every built-in
  `$(name)` as one glyph of it; this product now does the same. `codicon.ttf`
  (npm `@vscode/codicons@0.0.46-24`, redistributed byte-verbatim) is embedded in
  the executable as the named `CODICONFONT RCDATA` resource and registered
  process-privately by
  [`../workbench/icons/CCodiconFont.h`](../workbench/icons/CCodiconFont.h)
  through the same `AddFontMemResourceEx` path
  `CExtensionIconFontRegistry` already uses for `contributes.icons`. The name →
  code point vocabulary is
  [`../workbench/icons/CodiconGlyphTable.h`](../workbench/icons/CodiconGlyphTable.h),
  generated one-to-one from the same package's `dist/codiconsLibrary.ts`, so all
  746 upstream names — aliases included — resolve, not a hand-picked subset.
  Resolution order in `ThemeIconResolver` is therefore: extension-contributed
  registry (upstream's global `IconRegistry` semantics) → bundled font glyph →
  the imported vector fallbacks.
- **The imported vectors and the substitute dot are now a degraded path, not the
  normal one.** The two dozen vector icons in
  [`../workbench/icons/CodiconsActivityIcons.h`](../workbench/icons/CodiconsActivityIcons.h)
  are the GDI fallback when the bundled font cannot be registered. The Activity
  Bar and native title bar normally use their matching bundled codicon font
  glyphs; the tab strip retains its component-owned GDI path.
  `ThemeIconResolver` still falls back to them and finally to
  the substitute dot `Icon::RecordSmall` — but for `$(name)` text resolution that
  only happens when `CCodiconFont::Instance().FaceName()` is empty, i.e. the
  embedded resource is missing or GDI refused it. Passing an empty built-in face
  name is what makes the fallback reachable in tests; production call sites in
  `CMainStatusBar` and `CHoverWidget` always pass the real face name. A name the
  upstream library does not declare at all still resolves to the substitute dot
  rather than leaking the literal `$(name)` markup, which matches VS Code showing
  nothing meaningful for an undeclared id.
- **The font resource is declared once, in `sakura_rc.rc2` only.** `tests1.exe`
  links `sakura.vcxproj`'s objects *and* its `.res`, so `CODICONFONT` is already
  present in the test binary; adding the same resource to `tests1_rc.rc` makes
  the link fail with `CVT1100 重複するリソース` / `LNK1123`. Declaring it in
  `sakura_rc.rc2` also means no project-file edit is needed for either build
  path, because `sakura_rc.rc` includes that file and both MSBuild and
  CMake/MinGW compile it.
- **Corrected record — `odangoo.otak-usage`'s other built-in `$(name)` tokens
  never reached the status bar `text` path, so they never drew the substitute
  dot.** An earlier version of this note claimed that with `odangoo.otak-usage`
  installed, its built-in `$(check)`, `$(warning)`, `$(rocket)`, `$(zap)`,
  `$(copy)`, `$(edit)`, `$(circle-slash)`, and `$(dashboard)` tokens drew the
  substitute dot in the status bar. Live investigation of
  `x64/Debug/extensions/odangoo.otak-usage-1.19.0/extension/out/formatter.js`
  and `out/extension.js` found that claim wrong: every one of those names
  appears only inside the Markdown source that `formatter.js` builds for
  `StatusBarItem.tooltip`, never in `StatusBarItem.text`, and the tooltip path
  of the time (`ExtensionTooltipPlainText()`, since replaced — see "Extension
  Status Bar Item Hovers" above) removed `$(icon-name)` tokens outright rather
  than resolving them to a glyph — so no dot, substitute or otherwise, was ever
  drawn for them. Those tooltip tokens now *do* resolve, through the hover's
  shared `ThemeIconResolver`, so `$(check)` and friends draw their real glyph in
  the hover when the extension set `supportThemeIcons`. Since the bundled
  `codicon.ttf` landed they resolve there directly, so the substitute dot is
  reachable for them only if the embedded font fails to register at all. The
  only built-in `$(name)` that reaches the status bar `text` render path for
  this extension (in its default `cost` mode, while a refresh is in flight) is
  `$(loading~spin)`; the other tokens that do reach `text`
  (`$(otak-claude)`, `$(otak-openai)`) are this extension's own
  `contributes.icons` contributions, resolved through
  `CExtensionIconFontRegistry`, not built-ins. `loading` resolves from the
  bundled font, and `Icon::Loading` is additionally imported as a vector (see
  below) so it survives the degraded path too.
- **`$(loading~spin)` now draws the real `loading` glyph, statically.**
  `ParseExtensionStatusText` already stripped the `~spin` (or any
  `~modifier`) suffix before this change, exactly as VS Code's own
  `ThemeIcon.fromString` does, so the name looked up was already `loading`;
  the gap was that `loading` was not yet an imported glyph, so the resolver
  fell through to the substitute dot. It is now
  imported as `Icon::Loading` (16x16 viewBox, single path, non-evenodd fill,
  adapted from `vscode-codicons`' `loading.svg` at the same pinned commit as
  every other entry in
  [`../workbench/icons/CodiconsActivityIcons.h`](../workbench/icons/CodiconsActivityIcons.h);
  see `CODICONS-ATTRIBUTION.md` beside that header), so `$(loading~spin)`
  draws the correct spinner ring shape instead of a dot. Real VS Code also
  applies a continuous CSS `@keyframes spin` rotation to that glyph while the
  extension host is busy; this native renderer draws one static frame of the
  same glyph and does not rotate it. This is a faithful, non-animated
  rendering of the correct glyph — a degraded presentation of the same
  concept, not an unrelated glyph standing in for it, and therefore does not
  violate the top-level "never fake a capability" rule. Animating it would
  require a periodic repaint timer (most naturally reusing the status bar's
  existing per-item invalidation path) driving a rotating `HDC` world
  transform each tick; that is separate follow-up work and is not implemented
  here.

## Closing the last tab closes in place, not by process replacement (2026-08-13, #145)

- Legacy behavior: with the default merged-tab settings (`m_bDispTabWnd` on,
  `m_bDispTabWndMultiWin` off, `m_bTab_RetainEmptyWin` on), closing the last
  window of a tab group via `MYWM_CLOSE` (tab ×, middle click, `Ctrl+F4`,
  `F_WINCLOSE`) launched a **replacement editor process** through
  `CControlTray::OpenNewEditor` and then destroyed its own window. The user
  saw "close a file → the process restarts": the editor PID changed on every
  last-tab close, all process-local state died with it, and VS Code has no
  such behavior — `workbench.action.closeActiveEditor` keeps the window and
  process alive in the empty-editor state.
- Current behavior: when the working-copy coordinator exists, that branch of
  the `MYWM_CLOSE` handler closes **in place** instead. It routes to
  `ExecuteActiveWorkingCopyCommand(CloseActiveEditor, suppress, /*disposeWindow=*/false)`,
  whose `CommitClose` → `CDocFileOperation::CommitFileClose(true)` fires
  `PP_DOCUMENT_CLOSE` once and reinitializes the empty document
  (InitDoc/InitAllView/caption/no-name number/auto macro). No new process is
  spawned and the window's PID is stable across the gesture.
- An already-empty window is a no-op returning `TRUE` — there is nothing to
  close, and the window is retained, which is exactly what the legacy
  respawn was simulating at the cost of a process. A cancelled or failed
  close returns `FALSE`, preserving the `MYWM_CLOSE` contract that
  `CAppNodeManager`'s close-all loop aborts on a veto.
- The legacy spawn-and-die path is retained, unchanged, for every case the
  in-place branch does not claim: `PM_CLOSE_EXIT` requests, multi-window tab
  groups (group window count > 1), non-merged tab modes, windows without a
  working-copy coordinator, and workspace replacement (which travels through
  `WM_CLOSE` with the one-shot
  `m_workspaceReplacementClosePreflightAccepted` token, and is additionally
  guarded out of the in-place branch explicitly).
- This is a removal of a divergence, not a new one: the in-place close is the
  VS Code-compatible behavior, and the process restart was the defect.
