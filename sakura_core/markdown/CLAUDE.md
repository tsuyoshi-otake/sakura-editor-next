# Native Markdown Preview Guidance

This directory owns Sakura Editor NEXT's lightweight native Markdown preview.
The production implementation is `CMarkdownPreviewWnd` plus the pure
`MarkdownParser` model. It must not depend on an embedded browser, a script
runtime, remote content, or downloaded-at-runtime rendering assets.

Follow [`PARITY.md`](PARITY.md). Visual resemblance alone is not acceptance:
commands, lock identity, refresh behavior, source mapping, and security outcomes
must remain explicit and testable.

## Native rendering and security

- `MarkdownParser` stays independent from HWND, HDC, DPI, theme, and command
  state. `CMarkdownPreviewWnd` owns native measurement, painting, scrolling,
  focus, hit testing, and accessibility-facing labels.
- Raw HTML is never executed. Supported harmless wrappers are projected into
  typed native blocks; script/style bodies, event attributes, unsafe URI schemes,
  and unsupported active content are discarded or represented as blocked.
- Do not add network fetching. Local resources must remain inside explicitly
  approved roots after final-path/reparse validation before they can be opened.
- Keep work bounded for large documents. Live updates coalesce to the newest
  revision, and every refresh branch has a terminal rendered, deferred, rejected,
  or failed state.

## VS Code command model

Use the exact upstream command IDs and keep their effects distinct:

- `markdown.showPreview`
- `markdown.showPreviewToSide`
- `markdown.showLockedPreviewToSide`
- `markdown.showSource`
- `markdown.showPreviewSecuritySelector`
- `markdown.preview.refresh`
- `markdown.preview.toggleLock`
- `markdown.reopenAsPreview`
- `markdown.reopenAsSource`
- `markdown.togglePreview`

`Ctrl+K V` invokes `markdown.showPreviewToSide`; `Shift+Ctrl+V` invokes
`markdown.togglePreview`.

This repository currently has one real EditorGroup. Therefore the exact
`showPreviewToSide` commands terminate as typed unsupported outcomes; they must
not silently alias to the existing Sakura-specific sibling pane. The legacy
function-code preview may still use that physical sibling, under its distinct
`NativeSiblingPane` placement. Current-group show/toggle/reopen uses replacement
layout and hides the source editor while the preview owns the group.

Dynamic preview follows the active Markdown editor. Locked preview retains the
source URI it was created for. Every document switch, close, refresh, lock, and
reopen branch must state who owns final visibility and which source identity is
observable.

## Asynchronous update ownership

- The UI thread captures only a bounded immutable source/options/revision/
  generation snapshot. One persistent worker owns parsing and code highlighting.
- Scheduling is bounded to one in-flight request plus one latest pending request.
  A newer generation makes older completion stale; only a custom HWND message may
  commit the latest result to `Document`, HWND, and GDI state on the UI thread.
- Closing or native destruction transitions the scheduler to `Closed`, requests
  stop, clears owned pending/completed values, and joins the worker before HWND
  teardown. Posting failure releases the completed value and records `Failed`.
