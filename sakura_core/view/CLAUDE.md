# P4 Editor View Adapter Guidance

Editing views render and interact with the active editor pane selected by the
workbench. They must tolerate no active editor without constructing a fake
document or exposing a backing `CEditDoc` as open state.

Keep logical focus separate from Win32 focus. Keyboard-only navigation, screen
reader names/roles/states, high contrast, zoom, selection, hover, caret, and
100/125/150/200 percent DPI behavior are acceptance requirements. Painting and
layout consume theme/layout tokens rather than duplicating visual constants.

Visible whitespace figures (tabs, spaces, control characters, and EOL) use
`editorWhitespace.foreground` over `editor.background` while a Workbench color
theme is active. Their legacy type colors remain the fallback only when no
Workbench theme projection is active.

The minimap is a navigation surface, not a hover-preview surface. Its idle
timer must not create a floating `CTipWnd` containing a four-line source
excerpt; `CTipWnd` remains reserved for explicit keyword-help behavior in the
main editor. This keeps the minimap free of an unrelated yellow overlay while
preserving keyboard and pointer navigation.

## Editor scrollbars (#447)

- Both editor bars are as thick as every other workbench scrollbar:
  `workbench::controls::OverlayScrollbarThickness`
  (`workbench/controls/COverlayScrollbar.h`), 10 DIP scaled with the view's
  window DPI. `CEditView::OnSize` (bars, corner, split boxes, text area),
  `CTextArea` and `CSplitBoxWnd` take the strip from that one function. Do not
  read `SM_C*SCROLL` for editor geometry. Colors come from the shared
  `ResolveOverlayScrollbarColors` over the editor canvas.
- **Documented divergence (owner decision, 2026-11-28, #447):** VS Code's editor
  defaults are `editor.scrollbar.verticalScrollbarSize` = 14 and
  `editor.scrollbar.horizontalScrollbarSize` = 12. The owner asked for the
  editor bars to look like the Explorer's, which use VS Code's 10-pixel list
  scrollbar. Do not widen them back to the upstream editor defaults without a
  new decision.
- The hidden `WC_SCROLLBAR` children are the scroll model, and the overlay
  paints the whole reserved strip. A narrower bar therefore means a narrower
  strip, never a narrower paint, which would leave stale native pixels.
- An overlay callback passes the requested position to `ScrollAtV` /
  `ScrollAtH`. Never route it through `OnVScroll(SB_THUMBPOSITION)`: that path
  replaces the position with the control's `nTrackPos`, which is the current
  position for a control nothing is tracking. That routing made vertical track
  clicks and slider drags do nothing from the introduction of the overlay until
  #447.

## Bounded paint turns (#447)

`OnPaint2` paints at most one quantum (`CEditViewRenderState`, 4096 UTF-16
units) per `WM_PAINT` and continues from a saved cursor (#242). Every turn that
paints text must keep these rules:

- **Continue everything below the cursor.** A turn that yields invalidates from
  the cursor row to the bottom of the client area. Rows below the cursor were
  not painted either, so invalidating the cursor row alone leaves them
  unpainted. `WS_CLIPCHILDREN` keeps the scrollbar children out of that paint.
- **Resume only when nothing above is invalid.** `ResolvePaintStart` drops the
  cursor when the first invalid row is above it, and drops a cursor at draw
  column zero so that the row is drawn from its start, wrap indent included.
- **A turn without progress is not rescheduled.** `FinishPaintTurn` drops a
  cursor equal to the one the turn resumed. A self-invalidating paint that
  cannot advance would otherwise run forever.
- **Gutter-only turns take no part.** A paint whose rectangle ends at or left of
  the text area does not begin, resume or finish a quantum.
- **A row reference advances only within one logical line.** The color scan in
  `GetColorIndex` moves to the next layout row only for a wrap row of the same
  logical line (`GetLogicOffset() != 0`). The next logical line starts at offset
  zero, so an offset comparison alone counts one row per scanned character.
- A resumed row measures the selection highlight from column zero
  (`m_sDispPosBegin.ResetDrawCol()`), not from the resumed draw column.

Before #447, a view whose visible text exceeded one quantum could stop making
progress: the row reference drifted below the view, the row was never drawn,
and the same one-row rectangle was invalidated again without end. The UI thread
used about one CPU core, lower rows stayed unpainted (black), and the overlay
scrollbars received no `WM_PAINT`, so their thumbs did not move after a click.
`CEditViewRenderState.*` covers the cursor rules; the row drift needs a window
with more than 4096 visible UTF-16 units.

## Indentation guides

`editor.guides.indentation` belongs to the Editor and defaults to `true`, as it
does in VS Code. The guide color is `editorIndentGuide.background1`; the
deprecated `editorIndentGuide.background` remains its fallback. SENP extensions
may contribute indentation backgrounds, but their installation state must not
control the Editor-owned guide surface.

**Divergence:** active-indent highlighting (`editor.guides.highlightActiveIndentation`)
and the optional `editorIndentGuide.background2` through `background6` color
cycle are not implemented. Those settings/tokens remain unregistered rather
than being accepted as inert capabilities.

## Incremental TextMate tokenization

`CEditView` consumes `CDocLineMgr`'s bounded 64-record text-edit log through
`textmate::TextMateLineCache` (see `sakura_core/textmate/CLAUDE.md`) instead
of `CLayoutMgr::GetLayoutGeneration()` -- the generation also advances on
layout-only rewraps and over-invalidated the whole token cache. Session
lifetime: the language is selected the way VS Code does it, at model
creation, not on edits. `textEditorModel.ts`'s `getOrCreateLanguage` calls
`languageService.createByFilepathOrFirstLine(resource, firstLineText)` only
from `createTextEditorModel`; content edits never re-run first-line
detection (`TextFileEditorModel.onModelContentChanged` and
`UntitledTextEditorModel` only run ML `autoDetectLanguage`, and only for an
extensionless plaintext file). So `CreateSession` runs only on (a)
language-service revision change, (b) resource-path change, or (c) a
whole-document replacement -- a `full` record or an uncollectable edit-log
gap, Sakura's equivalent of model creation (a file load arrives as
`AppendAsMove`/`AddNewLine` `full` records, so a session created while the
document was still empty is re-selected once content arrives). Ordinary
edits, including line-0 edits, never call `CreateSession`. For the cases
that still select: a null session is adopted or dropped, a different
`LanguageId` adopts the new session and resets the cache, an unchanged ID
keeps the session. Divergence from VS Code kept deliberately: VS Code keeps
the model across a reload, while a Sakura reload is a whole-document
replacement and therefore re-selects; and Sakura has no ML language
detection, so an untitled document keeps its language. A third, narrower
divergence follows from the bounded log: a per-line `AddNewLine` load and
any burst of more than 64 edits between two paints (for example Replace
All) both reach the view as the same uncollectable gap, and the view cannot
tell them apart, so the burst re-selects too. It changes the outcome only
when first-line matching decides the language, that is, when no filename,
filename pattern or extension matches.

When a tokenized line's end state changes (`EnsureResult::
firstNewlyInvalidatedLine`), the lines below may need repainting even though
the legacy edit repaint region knows nothing about TextMate state; the view
invalidates from that line's layout row to the bottom, only when the row is
inside the visible text area. Two repaint triggers exist: (1) a state
transition reported by the current `EnsureResolved` call repaints below the
affected line, and (2) a requested line that is still unresolved (pending
work beyond the budget) schedules a whole-window `InvalidateRect` so a later
paint continues tokenization -- this is progress-driven, not a repaint loop.
Lines merely staying invalid without either trigger never repaint.

The minimap overview counts as token-ready only when the cache is fully
resolved (`IsTextMateFullyResolved`), not merely when the final line holds
stale tokens.

## File-drop ownership

The view owns each posted file-drop allocation until its own UI-thread message
is dispatched. An unrecognized handle is ignored; window destruction or Close
releases all pending allocations. During menu handling the view owns the drop,
except when transferring it to the live edit window's WM_DROPFILES handler,
which then releases it. Do not free the same drop after that transfer (#391).
