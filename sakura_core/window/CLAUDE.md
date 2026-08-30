# P4 Window Adapter Guidance

## Restricted Mode retirement (2026-08-15, #172)

Workspace Trust / Restricted Mode compatibility has been retired. The native window no longer owns a trust banner, status-bar entry, Workspace Trust editor page, startup prompt, or untrusted-file load gate. Normal file loading proceeds through the existing CLoadAgent path without a trust decision. Do not reintroduce these surfaces or gate loads on workspace trust unless a new, explicitly approved product capability replaces them.

`CEditWnd` and child HWND classes apply workbench snapshots and forward native
events. They do not own editor, command, configuration, profile, storage,
extension, or backend truth.

Multi-group/window, split, drag/drop, sidebar/panel movement, DPI changes, and
focus restoration must update the workbench model first, then apply one
snapshot. Do not add new peer-coordinate inference to `OnSize2`. Destroying or
hiding a window surface must explicitly transfer focus and must not terminate a
backend whose service owner remains active.

The I06 Pane Composite adapter binds all three physical Parts to one staged
projection. Side bars and the Panel query the retained native page registry for
their supported locations; the Panel keeps its horizontal Part chrome separate
from page content and applies host-relative wrapper plus wrapper-local child
coordinates in one required companion callback below that header. Existing
built-in location declarations are not widened by this native seam.

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
- During sash resize, the clamped `WorkbenchLayout` result is the geometry
  authority. Feed its applied extent back into `CWorkbenchPanelHost` before
  commit; never read a child HWND rectangle on button-up and persist that
  presentation output as model state. An intermediate child layout can be a
  valid transient projection but the wrong saved extent, producing a later
  Side Bar jump or collapsed scrollbar (verified 2026-08-23, #242).
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

## The window keeps a system frame on three edges (2026-08-20, #217)

- `WM_NCCALCSIZE` calls `DefWindowProc` first and then restores only
  `rgrc[0].top` to the window rectangle's original top, which is what Chromium
  (and therefore VS Code) and Windows Terminal do. The client is extended over
  the caption; the system frame survives on the left, right, and bottom.
- That surviving frame is not decoration we could skip. **DWM paints the 1px
  window border and applies the Windows 11 rounded-corner clip inside whatever
  non-client region is left after `WM_NCCALCSIZE`.** A handler that answers with
  the whole window rectangle — as this one did until #217 — leaves no such
  region, so the window has square corners and no border, and sits against a
  dark background as an edgeless dark rectangle. That was the reported defect.
- Diagnose this class of problem with `DWMWA_EXTENDED_FRAME_BOUNDS` (attribute
  9) against `GetWindowRect`, not by eye. When they are equal there is no frame
  and there can be no border; VS Code's extended bounds are inset by one resize
  handle on the left, right, and bottom. Measured on 2026-08-20 at 96 DPI:
  VS Code's border is a 1px `#69797E` ring with an antialiased corner arc, and
  Sakura now measures the same inset and ring.
- **A maximized window is sized one resize handle larger than the work area on
  every edge**, so the restored top must add `SM_CYFRAME + SM_CXPADDEDBORDER`
  back or the title bar is laid out above the top of the monitor. With that
  term, the maximized client comes out exactly equal to the monitor work area,
  which is why the previous manual clamp of `rgrc[0]` to `rcWork` is gone rather
  than merely unused.
- A non-zero `DefWindowProc` result carries client-preservation flags the system
  owns; return it untouched instead of rewriting rectangles it produced.
- `CalculateCustomFrameClientRect` is the pure form of all of this and is unit
  tested. Verify a change to it on screen as well: the geometry can be right in
  the model while the composited window still shows no border.
- `ApplyDwmFrameAppearance` owns every DWM attribute that decides how that frame
  is painted: dark mode and `DWMWA_BORDER_COLOR` (34). Keep them together — a
  system theme or setting change resets both, so `WM_THEMECHANGED` /
  `WM_SETTINGCHANGE` reapply the set rather than one member of it. The
  Windows 11 attribute value is written out because this SDK does not declare
  that `DWMWINDOWATTRIBUTE` member; on an older system the call simply fails and
  the window keeps the system default. **The corner preference is deliberately
  not set**: the Windows 11 rounded corners are what VS Code gets, and asking for
  square corners would be a divergence with nothing to gain.
- **A border color asked for at `WM_CREATE` time does not survive the window's
  first appearance.** Measured 2026-08-20: with the call only in `Attach`, the
  shown window still painted the system `#69797E`, while the identical call made
  from another process against the same HWND took effect immediately. The
  handler therefore reapplies on `WM_SHOWWINDOW`. Do not "simplify" that call
  away; verify on screen if you change this path, because nothing in the window's
  own state reports the attribute back (`DwmGetWindowAttribute` answers
  `E_INVALIDARG` for `DWMWA_BORDER_COLOR`).
- **Documented divergence (product decision, 2026-08-20, #217): the window border
  is the active theme's own `border` color, not the system border.** VS Code sets
  no border color, so it takes the system one, which follows the "show accent
  color on title bars and window borders" setting and measured `#69797E` here —
  a brighter, blue-tinted line than anything the workbench paints. The owner
  asked for a darker, untinted outline, so the frame passes `m_palette.border`,
  which the color-theme registry resolves from `sideBar.border` (measured
  `#2F2F2F` under the shipped dark theme). That is the same seam color the
  Activity Bar and side bars already draw, so the window's outline and its
  internal edges agree. This is a chosen appearance, not a faked capability: the
  frame region is exactly the one VS Code keeps, and deleting the one
  `DwmSetWindowAttribute` call restores upstream's border without touching any
  geometry.

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
- Every supported Activity Bar ViewContainer
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
- That toggle behavior applies only to the default vertical Activity Bar. When
  `workbench.activityBar.location` is `top` or `bottom`, VS Code embeds a
  horizontal composite bar in the Side Bar and an active icon click focuses the
  active view without hiding the Part. Keep the placement decision explicit;
  sharing the vertical toggle branch makes the horizontal bar disappear with
  its owning Side Bar and diverges from upstream's as-designed behavior.
- Run and Debug, Ports, Debug Console, arbitrary extension-owned views,
  reorder within a bar, moving the whole Panel
  (`workbench.action.movePanelToSecondarySideBar`), and panel alignment still
  fail closed at this adapter boundary. They remain explicit Phase 5/6 gates and
  must not be approximated with legacy active-tool state.

## Activity Bar Accounts and Manage (GlobalCompositeBar)

- In VS Code's default vertical Activity Bar, Accounts and Manage are lower
  **global actions** (`workbench.actions.accounts` /
  `workbench.actions.manage`), not ViewContainers. They live in
  `GlobalCompositeBar` and move to the title bar only when the Activity Bar
  itself is top or bottom.
- Sakura paints them at the bottom of the vertical Activity Bar with those
  upstream ids. They are typed as `ActivityBarEntryKind::GlobalAction`: they are
  not selected as Side Bar containers, are not composite drag handles, and open
  the existing Accounts / Manage menus. Account remains an explicit
  "No account provider configured" boundary; Manage contains only workbench
  commands with real native executors.
- The title bar does not host Account / Manage while the Activity Bar is
  vertical. The supported top/bottom placements move both actions to the title
  bar and remove them from the horizontal bars, preserving one placement only.

## Title-bar Update indicator (2026-08-06)

- The button is VS Code's `workbench.actions.updateIndicator`, title `"Update"`,
  contributed to `MenuId.TitleBarUpdate` at order 0
  (`contrib/update/browser/updateTitleBarEntry.ts`). Placing it in the title bar
  is upstream's own placement, not a divergence — Account and Manage sit on the
  Activity Bar for the default vertical position.
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
- It is a button **sitting on** the title bar, not a caption cell that happens to be
  coloured. `CustomFrameLayout::updateButton` stays full caption height because that is
  the action's hit-test and accessibility rectangle, but the fill, the hover/press
  feedback, and the focus ring are confined to `CustomFrameUpdateIndicatorPillRect`:
  inset by 4 DIP horizontally, drawn at the 22 DIP button height rather than the 34 DIP
  caption height, and rounded by 4 DIP. `MeasureCustomFrameUpdateButtonWidth` reserves
  that same horizontal margin, so the pill can never touch the Secondary Side Bar control
  or the minimize button no matter what the caption font measures.
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

## Custom-frame popup menus are localized (2026-08-20, #223)

- The Manage (gear), Layout, and Account popup menus built in
  `CCustomFrameController` take every label from the message resource through
  `LS(STR_WORKBENCH_MANAGE_*)`, `LS(STR_WORKBENCH_LAYOUT_*)`, and
  `LS(STR_WORKBENCH_ACCOUNT_NO_PROVIDER)`. `AppendUpdateMenuGroup` does the same
  for the `7_update` group. Matching upstream ids does **not** mean shipping
  upstream's English strings: VS Code localizes these same titles through its
  language packs, so an English literal here is a divergence, not fidelity. Every
  new item must be added to all three resources — `sakura_core/sakura_rc.rc`
  (ja), `sakura_lang/sakura_rc_en-US.rc`, and `sakura_lang/sakura_rc_zh-CN.rc`.
- Only the **label** is translated. The keybinding hint (`Ctrl+Shift+P`,
  `Ctrl+K Ctrl+S`) is a key sequence, not prose, so it stays in code and
  `MakeMenuItemText` joins the two with the `\t` that `AppendMenuW` right-aligns.
- `MakeMenuItemText` copies into a caller-owned buffer on purpose. `LS` returns
  one of four rotating static buffers, so a menu that held several `LS` pointers
  at once would paint the wrong labels; copying at the call site is what makes
  building a whole menu safe.
- Still English by design: `CustomFrameControlName` in `CCustomTitleBar.cpp`.
  Those strings are accessible names, and the `Update` one is also the painted
  label that `MeasureCustomFrameUpdateButtonWidth` measures, so translating them
  is a separate change that has to keep measurement and painting in agreement.

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
- **The version-8 File run reintroduced the node version 6 had just merged
  away.** `MigrateMainMenuV7DefaultToV8`'s replacement shape and
  `src/main/resources/MainMenu.ini` both still carried
  `F_FILE_RCNTFLDR_SUBMENU` + `F_FOLDER_USED_RECENTLY` after the combined
  `Open Recent` node, so the File menu offered the recent-folder list twice —
  once inside Open Recent's first group and again as a sibling submenu below it.
  Both are fixed at the source (2026-08-20); the File run is 46 items and Edit's
  top index is 46.
- `RemoveMainMenuRedundantRecentFolderSubmenu` is the version-9 migration for
  profiles that already persisted the v8 shape. **It gates on redundancy, not on
  the version number**: it fires only where the same model also carries an
  `F_FILE_OPENRECENT_SUBMENU` node whose child is the combined
  `F_RECENT_WORKSPACE_LIST` projection — the surface that already renders the
  folder list — and only where the legacy node is unrenamed and has that single
  projection as its only child. Unlike the v8 replacement it deliberately does
  *not* gate on `MainMenuModelFingerprint`, because a whole-model fingerprint
  would leave the duplicate in place forever for anyone who had customized an
  unrelated menu. It runs after the v8 path, since a customized menu keeps its
  persisted shape there and still needs the duplicate removed.
- A menu never renders a separator for an empty group.
  `RemoveRedundantMenuSeparators` drops leading, trailing, and consecutive
  separators after the model is projected, so an empty MRU list cannot leave a
  dangling rule. A submenu that becomes empty is then greyed out by
  `CheckFreeSubMenu`, exactly as before.
- Folder/workspace rows render VS Code's `labelService.getWorkspaceLabel(uri,
  { verbose: Verbosity.LONG })` labels, never raw URIs (2026-08-20): an
  explicit stored label wins; a file URI becomes the native Windows path with
  an uppercase drive letter, UNC as `\\server\share`, and percent-encoding
  decoded; a saved workspace drops its case-sensitively matched
  `.code-workspace` extension before the localized
  `STR_WORKBENCH_RECENT_WORKSPACE_LABEL` (`{0} (Workspace)`) format wraps it;
  a non-file URI keeps its canonical URI form. The formatting lives in
  `CRecentlyOpenedWorkspaceMenuProjection::FormatEntryLabel`, and `CEditWnd`
  passes the localized format string in so the projection stays HWND- and
  resource-free.
- `Clear Recently Opened...` (`workbench.action.clearRecentFiles`,
  `F_CLEAR_RECENT_WORKSPACES`) closes the submenu as a **static** contribution,
  exactly as upstream (2026-08-20): `BuildTrailing` emits it below every dynamic
  group, preceded by a separator only when something precedes it. The row
  therefore exists even with an empty history, so the submenu is never empty and
  `CheckFreeSubMenu` never greys it — matching VS Code, where the entry is always
  present and reachable. The command confirms through
  `STR_WORKBENCH_RECENT_CLEAR_CONFIRM` before clearing both the typed
  `workbench::recent` store and the legacy `CMRUFile`/`CMRUFolder` lists;
  `RecentlyOpenedWorkspaceService::Clear` treats an already-empty history as a
  success that performs no durable write.
- **Documented divergence:** `Reopen Closed Editor` (`Ctrl+Shift+T`) is absent.
  It needs a per-window stack of closed editors with their restorable state, and
  this fork has no such history — only the file MRU, which is a different concept
  (it survives across sessions, carries no view state, and never distinguishes a
  closed editor from a merely opened file). Approximating it with the MRU would
  be faking the capability, so the entry stays out until that stack exists.
- **Documented divergence:** `More...` (`Ctrl+R`) is absent. Upstream needs it
  because `MenubarRecentMenu` truncates to a handful of rows and the quick pick
  is the only way to reach the rest, with type-to-filter over the full history.
  Here the submenu already renders the entire history — `kMaximumRecentlyOpenedWorkspaces`
  is 64 and nothing truncates it — and `CQuickInputDialog` is a plain `LISTBOX`
  with no filtering, so the entry would re-present the same rows in a worse
  surface. Add it when the quick pick gains real filtering, not before.
- **Documented divergence:** the recent-file rows below the separator keep the
  legacy `CMRUFile` presentation (numbered mnemonic prefix plus encoding
  suffix such as `[UTF-8]`) instead of VS Code's plain-path labels. Keeping
  the Sakura-native file MRU behavior there is an explicit product decision
  for this fork, and those rows still come from the legacy MRU projection,
  not from `workbench::recent`.

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
  `status.host`, `status.scm`, `status.editor.selection`,
  `status.editor.eol`, `status.editor.encoding`, `status.editor.inputMode`,
  `status.editor.zoom`, and `status.notifications`.
- The leftmost entry is VS Code's remote host indicator (`status.host`,
  `$(remote)`, command `workbench.action.remote.showMenu`). Sakura paints that
  affordance for layout parity, but Remote Development is not a supported
  authority here: activating it fails closed with an explicit status message
  rather than simulating SSH/WSL/container windows. SCM `statusBarCommands`
  remain the next left-aligned items.
- The native status bar must not use `SBARS_SIZEGRIP`. VS Code anchors its
  rightmost item directly to the status-bar client edge; reserving a legacy
  resize-grip width leaves the notifications icon visibly too far left.
- Resizing belongs to the custom frame, not the removed status-bar grip.
  `WM_NCHITTEST` must expose every edge and corner; maximized windows continue
  to suppress resize hits. Since #217 the client is extended over the caption
  only (see "The window keeps a system frame on three edges" above), so the
  left, right, and bottom targets sit in the surviving system frame outside the
  client and reach
  the top-level window as negative or past-the-edge coordinates. The top band is
  the only one inside the client, and therefore the only one a child control
  could take the initial press away from: exactly one topmost input-only child
  overlay covers it and forwards its press to the non-client resize path. Do not
  reintroduce inner bands on the other three edges — they would eat the
  outermost pixels of the Activity Bar, the editor, and the status bar, which
  VS Code leaves clickable.
  **Documented divergence:** Sakura's character-code display and macro-recording
  indicator have no VS Code counterparts. Their IDs are therefore explicitly
  product-owned as `sakura.status.editor.characterCode` and
  `sakura.status.macroRecording`; do not disguise them under unrelated VS Code
  IDs merely to make the customization menu look canonical.


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

## Split boxes are Sakura-only, so they follow the theme (2026-08-20, #225)

`CSplitBoxWnd` (the vertical box above the editor's vertical scrollbar and the
horizontal box left of the horizontal one) has no VS Code counterpart: VS Code
splits an editor group through a command, not through a draggable box beside a
scrollbar. **Documented divergence:** the control is kept, because removing it
would remove a Sakura capability, but it no longer paints legacy 3D edges from
`COLOR_3DFACE`/`COLOR_3DSHADOW`. `OnPaint` fills the client with the active
theme's `canvas` and draws one centred 1px `border` grip, which is the same seam
colour the workbench sashes use. Its drag behaviour is unchanged.

## The status bar must not be WS_EX_COMPOSITED (2026-08-20, #226)

Reported symptom: right after startup the Source Control view drew its
"no Git repository / Initialize Repository" welcome content *above* a fully
populated 76-item Changes list, and the graph section, the terminal body, and
the status bar's SCM items never appeared at all. Nothing was wrong with the
model — every one of those surfaces had already computed the correct content.

Root cause: `CMainStatusBar::CreateStatusBar` created the `msctls_statusbar32`
control with `WS_EX_COMPOSITED`. That control is painted entirely by our own
`StatusBarSubclassProc` `WM_PAINT` handler, which calls `PaintStatusBar` — and
with the extended style set, its client region was dirty again the instant
`EndPaint` returned. `WM_PAINT` is *synthesised* by `GetMessage` whenever a
window has a non-empty update region, so one perpetually re-invalidated window
generates `WM_PAINT` forever and **starves every other window's pending update
region indefinitely**. The other parts were invalidated and simply never got a
paint message.

The style bought nothing anyway: `PaintStatusBar` already renders into its own
`CreateCompatibleDC` back buffer and blits once.

How this was pinned down, in order — repeat this method rather than reasoning
about it, because each step eliminated a plausible wrong answer:

- Tally messages by target window class in `CEditWnd::MessageLoop`. 11,931 of
  12,000 messages were `WM_PAINT` to `msctls_statusbar32`.
- Instrument `GetUpdateRect` around `BeginPaint` / `EndPaint`. Note that between
  `BeginPaint` and `EndPaint` the update region is empty *by definition*, so a
  reading of 0 there proves nothing about the paint body — only the reading
  after `EndPaint` is evidence.
- Rule out each candidate invalidator by counting it, not by inspection:
  `SetPalette`, `SetStatusbarViewSnapshot`, `RedrawWorkbenchFrameForCommittedLayout`,
  and `CCaret::ShowCaretPosInfo` all counted **zero** hits in the spin window.
- Bisect the paint body: skipping the `PaintStatusBar` call stopped the storm,
  which is what pointed at the control's own composited redirection rather than
  at any of our invalidation calls.

Two earlier conclusions were wrong and are recorded so they are not retried:
darkmodelib's status-bar subclass is innocent (disabling
`DarkMode::setDarkWndNotifySafeEx` entirely left the storm unchanged), and
suppressing the control's own `WM_NCPAINT` by returning 0 without chaining to
`DefSubclassProc` does **not** fix it — it merely leaves the non-client region
permanently invalid, which sustains the same paint synthesis.

Verification (x64 Debug, 2026-08-20): idle CPU over 5 s with the repository
folder open fell from 4,265 ms to 31.25 ms. `EnumChildWindows` + `GetUpdateRect`
6 s after launch reports `update=none` for every child, where twelve windows
previously held a permanent update region. Five fresh-process screen-versus-
`PrintWindow` trials (three unoccluded) measured 2.174% / 2.089% / 2.099%; the
diff heat map shows that residue is only the DWM-painted window border, which
`PrintWindow` does not render, and one blinking caret in the commit-message box.
The window interior is pixel-identical.

## The wheel follows the pointer, not the focus (2026-08-20, #227)

VS Code scrolls whatever the pointer is over. Win32 delivers `WM_MOUSEWHEEL` to
the **focus** window, and `SPI_GETMOUSEWHEELROUTING`'s hybrid default only
redirects to a hovered *other application* — inside one frame, a hovered Source
Control or Explorer list never sees the wheel while the editor pane holds focus.
Measured: with a repository open and the caret in the editor,
`GetGUIThreadInfo().hwndFocus` is the `SakuraView*` pane, so the wheel arrived
there no matter where the pointer sat.

`CEditWnd::HoveredScrollTarget` resolves the hovered descendant and
`WM_MOUSEWHEEL` forwards to it, so each control keeps its own scroll authority:

- Input-only overlays (`WS_EX_TRANSPARENT` sashes, the frame resize band) sit
  above the content they cover, so the walk climbs to their parent rather than
  swallowing the wheel at an overlay.
- Foreign-thread and foreign-root windows are rejected; a hovered window that is
  not part of this frame is not this frame's business.
- **The editor panes keep the historical path.** `OnMouseWheel` owns zoom, the
  caret, and the split-pane dispatch that a bare `WM_MOUSEWHEEL` forward cannot
  reproduce, so a hovered `CEditView` returns `nullptr` and falls through.
- Print preview keeps the historical path for the same reason.

Verified 2026-08-20 (x64 Debug, folder = this repository): with the caret in the
editor, three real `mouse_event(MOUSEEVENTF_WHEEL)` notches over the Source
Control change list moved `LB_GETTOPINDEX` 0 -> 9, the same gesture over the
Graph list moved it 9 -> 18, and five notches over the editor pane changed
12.830% of the pane's sampled pixels. Before the change every list stayed at 0.

## A folder target with no workspace makes every settings read fail (2026-08-20, #227)

`CConfigurationService::IsContextValid` rejects a target that names a
`folderUri` while leaving `workspaceUri` empty. A **Folder** workspace has no
`.code-workspace` file, so `WorkspaceContextSnapshot::workspaceConfigUri` is
empty there — a target built as "workspaceConfigUri, plus the folder when there
is exactly one" is therefore invalid for every single-folder window, which is
the normal case.

The failure is silent by construction: `GetValue` answers
`InvalidScope` with no value, and each caller falls back to its own default. It
looks exactly like an unset setting. Verified 2026-08-20 by logging the outcome:
`scm.countBadge`, `workbench.colorTheme`, and `http.timeout` all returned
`out=6 diag=target has an invalid URI/profile/language combination` with a
`-FOLDER=` window open, so **no** workbench setting was being read at all.

`CEditWnd::BuildWorkbenchConfigurationTarget` is now the single builder for every
workbench settings read. It follows `CWorkbenchRuntime`'s own rule: a Folder
workspace names its own folder as the workspace identity, and only a real
`Workspace` uses `workspaceConfigUri`. Do not hand-roll a target beside it —
prove a settings read works by changing the value and observing the window, never
by reading the code path.

## The caption is VS Code's `window.title`, and the minimap belongs to the editor (2026-08-20)

Two frame-level defects had the same root cause: a control was positioned or
composed against the frame rather than against the concept that owns it.

- **Caption.** The shipped default used `$N`, the abbreviated *full path*. VS
  Code's default `window.title` is
  `${dirty}${activeEditorShort}${separator}${rootName}${separator}${appName}`,
  which is the file's own name and the opened folder's own name — never a path.
  The defaults in `CShareData.cpp` now read
  `${U?● $}${w?$h$:アウトプット$:$f$n$}${W? - $W$} - $A...`, and
  `CSakuraEnvironment` gained `$W` (the root folder's name) with a matching
  `${W?...$}` condition so the separator disappears when no folder is open.
  `$W` resolves through `CEditWnd::GetWorkspaceRootName()`, which reads the
  **workspace model** (`GetSemanticWorkspaceRoot`), not the Explorer View: the
  caption must be right even when that View was never created.
  A profile still holding the previous shipped default is migrated in
  `CShareData_IO.cpp`; a caption the user edited is left alone.
  Verified 2026-08-20 on throwaway profiles: without `-FOLDER`,
  `CLAUDE.md - Sakura Editor NEXT`; with it,
  `CLAUDE.md - sakura-editor-next - Sakura Editor NEXT`.

- **Minimap.** `layout.minimap` is a frame-level band to the right of
  `layout.editor`, so a side-by-side Markdown preview left the minimap stranded
  against the frame, on the far side of the preview. In VS Code the minimap is
  drawn *inside* the editor group. `LayoutMarkdownPreview` therefore now
  receives the editor rectangle **plus** the minimap's column, reserves that
  column at the right of the editor half, and returns the resulting rectangle
  for `OnSize2` to apply. When no preview is open the split returns the original
  edge, so nothing moves. Verified 2026-08-20 with the preview open: editor view
  323..1263, minimap 1263..1363, preview 1364..1904.
