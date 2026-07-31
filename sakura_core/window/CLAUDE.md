# P4 Window Adapter Guidance

`CEditWnd` and child HWND classes apply workbench snapshots and forward native
events. They do not own editor, command, configuration, profile, storage,
extension, or backend truth.

Multi-group/window, split, drag/drop, sidebar/panel movement, DPI changes, and
focus restoration must update the workbench model first, then apply one
snapshot. Do not add new peer-coordinate inference to `OnSize2`. Destroying or
hiding a window surface must explicitly transfer focus and must not terminate a
backend whose service owner remains active.

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
- **Documented divergence:** the Marketplace is Sakura's own OpenVSX
  search/install/remove list, not VS Code's Extensions view UI. There are no
  gallery cards, no per-extension detail editor, no recommendations/installed/
  outdated sections, and no filter quick-pick. It is the same concept in the
  same place with a narrower surface, not a faked parity claim.
- **Documented divergence:** this route only acquires and places an extension.
  There is still no way to *run* an installed extension through it.
- The OpenVSX production factory's missing credential provider is recorded in
  [`../extension/CLAUDE.md`](../extension/CLAUDE.md); do not restate it here.

## Phase 5 Native Command Route Checkpoint (2026-07-31)

- A runtime-backed window owns one `WorkbenchContextKeyService` and
  `WorkbenchCommandRegistry`. Destroy the registry/context before the native
  hosts captured by their executor callbacks.
- Explorer Activity Bar activation and legacy
  `F_TOGGLE_LEFT_EXPLORER` dispatch both execute
  `workbench.view.explorer`. Strip only the base 16-bit function code for alias
  comparison; preserve source/high-bit flags until that boundary.
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
