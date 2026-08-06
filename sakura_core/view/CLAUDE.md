# P4 Editor View Adapter Guidance

Editing views render and interact with the active editor pane selected by the
workbench. They must tolerate no active editor without constructing a fake
document or exposing a backing `CEditDoc` as open state.

Keep logical focus separate from Win32 focus. Keyboard-only navigation, screen
reader names/roles/states, high contrast, zoom, selection, hover, caret, and
100/125/150/200 percent DPI behavior are acceptance requirements. Painting and
layout consume theme/layout tokens rather than duplicating visual constants.

## Extension hover popup (2026-08-07)

`CEditView` drives `vscode.languages.registerHoverProvider` end to end: mouse
dwell over a word triggers `CExtensionService::RequestHover`, and a published
`SExtensionHoverResult` renders in `CEditViewHoverPopup`
(`sakura_core/view/CEditViewHoverPopup.{h,cpp}`), which hosts the shared
`markdown::CMarkdownPreviewWnd` renderer -- the same one
`CExtensionDetailSurface` uses for Marketplace READMEs -- rather than a second
Markdown implementation. Every hover Markdown resource resolves as
`ResourceDisposition::ExternalBlocked`, identical to the README rule.

- **The dwell/poll driver is the view's existing `IDT_ROLLMOUSE` timer, not a
  new one.** `CEditView::Create()` already arms `IDT_ROLLMOUSE` and
  `CEditView::OnTimer` already drives the dictionary-tip dwell logic
  (`KeyWordHelpSearchDict`). Hover reuses that exact timer tick to call
  `FireHoverRequestIfDue()` then `PollHoverResult()`, instead of adding a
  second timer or reaching into `CEditWnd::WndProc`'s
  `MYWM_EXTENSION_WORKBENCH_CHANGED` push-notification path. This keeps hover
  entirely inside the view's existing per-pane timer ownership: no new
  cross-window message and no new push path.
- **The view owns the gesture; the composition root owns the connection.**
  `CEditView` never sees `CExtensionService`. `SetHoverHandlers` takes three
  `std::function` seams (request / cancel / poll) that only
  `CEditWnd::WireExtensionHoverHandlers` supplies -- see
  [`../window/CLAUDE.md`](../window/CLAUDE.md). A view whose handlers are empty
  never arms the dwell, so a window with no extension service cannot start a
  request it could not honor. **Corrected record:** an earlier version of this
  section claimed hover needed "no dependency on a `CEditWnd` change outside
  this subsystem's edit scope". That was true only while the seams had no
  production caller; wiring them is a real `CEditWnd` change, and pretending
  otherwise would have left the feature permanently dead.
- **Hover dwell is 500 ms, not the dictionary tip's 300 ms
  (`kHoverDwellMilliseconds` in `CEditView.cpp`).** Real VS Code's own
  `workbench.hover.delay` default is 500 ms
  (`contrib/hover/browser/*`), so the dwell threshold matches the concept it
  reproduces rather than reusing an unrelated native constant that happens to
  share the same timer.
- **`FireHoverRequestIfDue` re-validates the same gates
  `KeyWordHelpSearchDict` already uses** (`m_bInMenuLoop == FALSE`,
  `GetCaret().ExistCaretFocus()`, cursor still inside the client rect) before
  firing a request, so a hover request is never sent for a view that has lost
  focus or whose cursor has already left the window between the triggering
  `WM_MOUSEMOVE` and the next timer tick.
- **Cancellation and ordering follow `CExtensionService`'s existing hover
  sequence fence, not a second one here.** `CEditView` calls
  `m_hoverCancelHandler()` (wired to `CExtensionService::CancelHover`) on
  every event that invalidates the current hover -- moving off the word,
  starting a selection drag, autoscroll, entering the minimap, and view
  `Close()` -- and never renders a `PollHoverResult()` snapshot without first
  confirming the position it was requested for still matches
  `m_ptHoverLogicPos`. The service-side sequence counter is the single source
  of truth for "is this response still the latest one"; the view never
  maintains a competing generation of its own.
- **Documented divergence -- fixed bounded popup size, not natural-content
  size.** `CMarkdownPreviewWnd` has no API to measure a parsed document's
  natural size ahead of layout, unlike VS Code's DOM-based hover widget, which
  grows to its content up to a viewport-relative maximum. `CEditViewHoverPopup`
  is therefore always laid out at a fixed maximum size (460x300 DIP, see
  `kMaxWidthDip`/`kMaxHeightDip` in `CEditViewHoverPopup.h`) and relies on the
  preview child's own internal scrollbar for content that does not fit. Revisit
  this once an intrinsic-size measurement pass exists for the shared Markdown
  renderer; until then this is a real, bounded degradation of VS Code's
  behavior, not a look-alike substitute for it.
- **`registerCompletionItemProvider` / `provideCompletionItems` remain an
  explicit, typed, fail-closed boundary.** No native completion list exists
  yet. A registered completion provider's results are never requested and
  never surfacing them is the correct, honest state until real VS Code trigger
  semantics (trigger characters, incomplete-list re-trigger, filter/sort order)
  can be implemented completely; do not approximate it with an ad hoc word-list
  popup.
