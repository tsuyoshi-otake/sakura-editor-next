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
