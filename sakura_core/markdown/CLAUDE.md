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

## Live resize and scrolling ownership (2026-08-22, #241)

- This is a native User32/GDI surface, not a WebView. The preview HWND has no
  `WS_VSCROLL`; `m_scrollY`, content height, and viewport height form its single
  pixel-based model. `COverlayScrollbar` presents that model through
  `ExplicitModel` and uses the preview only for geometry, focus, visibility, and
  wheel routing.
- Divider drag is a transaction. Mouse move updates only transient editor-group,
  minimap, preview, overlay, and divider geometry. It must not run full Markdown
  reflow or full workbench `OnSize2` for each sample. Mouse-up performs one
  committed reflow; Escape, `WM_CANCELMODE`, capture loss, layout-state change,
  and destruction explicitly commit, roll back, or abort.
- A parse completion, document swap, font/DPI change, or image-cache invalidation
  arriving during a transient drag is deferred as one generation. Do not publish
  a new document while old render lines point at deleted image handles.
- Wrapping is width-dependent. A height-only resize updates the viewport and
  explicit scrollbar model without rebuilding render lines. Hiding a preview
  must not lay the document out at zero width, and reopening an unchanged hidden
  preview reuses its committed document generation instead of parsing it again.
- Width commit is one `O(N)` document reflow, but it is no longer one UI
  handler. Keep the published render/image/diagram vectors as last-known-good,
  advance a staging generation through message slices of at most two completed
  blocks and at most 32 wrapped rows / 2 ms within a block. Preserve the
  within-block cursor between slices; do not restart a giant paragraph. Stable
  document text is viewed in place instead of copied, and inline-span
  normalization is prepared by the worker before the UI continuation consumes it,
  and swap generations only after staging is complete. A newer width/document
  request cancels staging and the already-posted wake services only the latest
  generation. Keep each wrapped-text scan linear: never pass the entire
  remaining paragraph to GDI once per output row. Bound the measurement probe
  and advance forced-break searches monotonically.
- Capture a logical source-line/ordinal/intra-line anchor before committed reflow
  and restore it afterward. User scrolling clears an earlier reveal request so a
  later width change cannot snap back to the editor caret.
- Verify the real painted frames with `tools/measure-markdown-preview-resize.ps1`.
  It compares `CopyFromScreen` and `PrintWindow`, checks the native style and
  sibling overlay count, measures every drag message, drives `WM_CANCELMODE`, and
  proves run-owned process/profile cleanup.

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
  generation snapshot. One persistent worker owns parsing, code highlighting,
  and admitted local-image WIC decode. Worker-prepared HBITMAP resources use
  shared ownership across staging and last-good layout generations; a missing
  worker-prepared image never falls back to synchronous UI-thread decode.
- Scheduling is bounded to one in-flight request plus one latest pending request.
  A newer generation makes older completion stale; only a custom HWND message may
  commit the latest result to `Document`, HWND, and GDI state on the UI thread.
- Closing or native destruction transitions the scheduler to `Closed`, detaches
  the HWND completion sink, requests stop, and clears owned pending/completed
  values. A pre-admitted bounded retirement service owns the eventual join;
  the UI thread never waits for parser/highlighter/image work. Posting failure
  releases the completed value and records `Failed`.


## Declared boundaries must be per concept, not per feeling (2026-08-20, #228)

Two declarations had drifted from the code, and the fix in each case was to make
the declaration state the real concept, not to flip a value until it matched:

- `PreviewCapabilities::editorPreviewScrollSync` was one flag for two independent
  directions, and it read `Unsupported` while both directions were wired. It is
  now `scrollPreviewWithEditor` and `scrollEditorWithPreview`, named after the
  upstream settings that gate them. Do not merge them back: a single flag cannot
  express one working direction and one broken one, which is the state this
  subsystem will be in the moment either wiring regresses.
- `tabbar::kMarkdownPreviewCommandId` declared `markdown.showPreviewToSide` while
  the button dispatched `F_TOGGLE_MARKDOWN_PREVIEW` into `ToggleNativeSibling`.
  Making the button execute the VS Code command would have aliased the sibling
  pane to a second EditorGroup this shell does not have, so the id became
  `sakura.toggleMarkdownSiblingPreview` instead. Rename it back only together
  with a real second EditorGroup.

A capability may only be declared `Supported` after the wiring is measured on the
running editor, not after reading the code that appears to connect it. The
scroll-sync directions were measured on 2026-08-20 by driving `F_GOFILETOP` /
`F_GOFILEEND` and `WM_VSCROLL` under a throwaway profile. That historical run
read both `SCROLLINFO` positions; after #241, preview `SCROLLINFO` is intentionally
absent and the explicit overlay model/painted thumb is the observable preview
state. The historical numbers and current verification method are in `PARITY.md`.

## Upstream drift is detected mechanically

`upstream-parity-manifest.json` pins the VS Code commit, the 13 preview settings
with their exact defaults and enums, the 10 preview commands, and the 2 default
keybindings. Regenerate it from the pinned path and diff, rather than extending a
hand-written list from memory: the hand-written list in #228 had invented
`markdown.preview.scrollBeyondLastLine`, which upstream does not define, and had
omitted `typographer`, `markEditorSelection`, `openMarkdownLinks`, and
`frontMatter`. The manifest records what upstream declares; it is not a claim
that Sakura honors those settings, and today it does not.

## Preview typography comes from markdown.css, not from the editor's font (2026-08-20)

The preview's text must read as VS Code's preview, so every typographic constant
in `CMarkdownPreviewWnd.cpp` is copied from
`extensions/markdown-language-features/media/markdown.css` at the commit pinned
in `upstream-parity-manifest.json`, plus the defaults of
`markdown.preview.fontSize` (14) and `markdown.preview.lineHeight` (1.6). Do not
re-derive them from the editor's document font or from GDI text metrics; both
had produced visible divergences:

- The prose face is `Segoe UI`, the Windows end of upstream's stack
  (`-apple-system, BlinkMacSystemFont, "Segoe WPC", "Segoe UI", system-ui, ...`).
  It was `Segoe UI Variable`, which is a different, Windows 11-only face that
  upstream never selects.
- Headings are sized in `em` per-mille — 2000/1500/1250/1000/875/850 — because
  per-cent rounds 0.875em to 88%. The ladder was 170/150/135/120/110/100, which
  matched no upstream level.
- Heading weight is `FW_SEMIBOLD`, matching CSS `font-weight: 600`. It was
  `FW_BOLD` (700).
- Line boxes follow the CSS line-height (22px prose, 1.357em code, 1.25em
  headings) with the measured text height only as a floor, rather than
  `tmHeight + 3`.
- Body padding is `0 26px` with a `1em` top pad, not 14px/12px.
- `lfQuality = CLEARTYPE_QUALITY` on **every** face the preview creates. VS Code
  renders through DirectWrite with subpixel antialiasing; a face left at
  `DEFAULT_QUALITY` falls back to greyscale and reads as a different weight
  beside the editor.
- The editor's font family is applied to code spans only, matching upstream's
  `code { font-family: var(--vscode-editor-font-family) }`. It falls back to
  Consolas when the editor font is unavailable.

Known remaining differences, deliberately not yet implemented: paragraph, list,
blockquote, and table blocks all use one uniform block gap (the browser default
`p { margin-bottom: 1em }`) instead of upstream's per-element margins, and the
h1/h2 bottom border with `padding-bottom: 0.3em` and `th/td { padding: 5px 10px }`
are not drawn. Headings do honor `margin-top: 24px` / `margin-bottom: 16px` with
h1 at `margin-top: 0`, with CSS margin collapsing applied against the preceding
block's gap.

## Mermaid is drawn natively, for the flowchart subset only (2026-08-20, #228)

`MermaidDiagram.{h,cpp}` owns the whole path from Mermaid source to placed
geometry. It is Win32-free - text measurement arrives as a `std::function`
callback and the result is plain integer coordinates - so the layout is unit
tested without a device context. `CMarkdownPreviewWnd` owns only the strokes.

The layout is the layered (Sugiyama) pipeline that dagre, and therefore Mermaid
itself, uses, in the three phases those implementations name: rank by longest
path from the sources after breaking back edges with a depth-first sweep, order
within each rank by iterative barycentre sweeps, then assign coordinates.
Multi-rank edges are broken by virtual waypoints before ordering, which is what
keeps a long edge from being drawn straight through an unrelated box. The
reference implementations consulted were the Rust crates `mermaid-text` (whose
ascii-dag backend documents exactly this ranking-plus-barycentre sequence),
`rusty-mermaid-svg` (a dagre port with explicit rank/order/position phases),
`mermaid-rs-renderer`, and `merman` (a JS-runtime-free typed semantic model plus
SVG renderer).

Layout is computed on abstract breadth/depth axes and mapped onto x/y only at
the end, so `TD`/`TB`, `BT`, `LR`, and `RL` share one implementation rather than
four transposed copies.

The supported subset is: the `graph` / `flowchart` header with those four
directions; node references `id`, `id[..]`, `id(..)`, `id([..])`, `id((..))`,
`id{..}`, `id{{..}}`; the link operators `-->`, `---`, `-.->`, `-.-`, `==>`,
`===`; both edge-label forms (`-->|text|` and `-- text -->`); node chaining in
one statement; `;` statement separators; and `%%` comments.

Everything else fails closed and returns `UnsupportedSyntax` or `LimitExceeded`,
and the caller keeps the pre-existing notice-plus-literal-source path. That
includes every non-flowchart family, and specifically `subgraph`/`end`,
`click`, `style`, `classDef`, `class`, `linkStyle`, and `direction`. Those are
refused rather than ignored because each of them changes what the diagram means:
drawing the graph with the keyword silently dropped would show a picture the
author did not write, which is the failure mode this repository's highest-
priority rule forbids. A diagram wider than the pane is also refused, because
this preview has no horizontal scrolling and would otherwise cut it off silently.

The capability was split into `mermaidFlowchartRendering` (`Supported`) and
`mermaidNonFlowchartRendering` (`Unsupported`) for the reason recorded in the
section above: one flag cannot express one family drawn and the rest literal.
