# Hover Widget Guidance

## Scope

This directory implements VS Code's hover as a real concept, not a look-alike:

| File | Upstream counterpart | Responsibility |
|---|---|---|
| `HoverMarkdown.h` / `.cpp` | `vs/base/browser/markdownRenderer.ts` (`renderMarkdown`) | Markdown source → a window-independent block model |
| `CHoverWidget.h` / `.cpp` | `vs/base/browser/ui/hover/hoverWidget.ts` (`HoverWidget`) | One `WS_POPUP` window that lays out and paints that block model with GDI |

`HoverMarkdown` is pure. It must not depend on an HWND, an HDC, a DPI value, a
palette, a font, or `CMainStatusBar`. That is what lets the whole Markdown
contract be unit-tested without a window
([`../../../src/test/cpp/tests1/test-hovermarkdown.cpp`](../../../src/test/cpp/tests1/test-hovermarkdown.cpp)),
and what will let a second hover host (editor hovers, Activity Bar, tabs) reuse
the same renderer without touching the status bar. Keep every measurement,
color, and font decision on the `CHoverWidget` side of that line.

## Why not `TOOLTIPS_CLASSW`

Real VS Code renders `StatusBarItem.tooltip` / `vscode.MarkdownString` as a
floating hover element whose content is genuine rendered Markdown: proportional
headings, real bold/italic emphasis, monospace code, theme icons inline with
text, and a column-aligned table. A Win32 common-control tooltip carries one
plain-text string, so with that control the *only* honest option was to project
the Markdown to plain text — which is what this repository previously did.

Plain-text projection is lossy in a way users can see. `odangoo.otak-usage`
builds a two-brand table whose middle column holds a literal `│` (U+2502)
divider, and whose absent values are the literal placeholder `&nbsp;`. Flattened
to one line those become runs like `Claude Code | │ | Codex CLI` and
`制限 (max) | │` — visible markup debris that has no counterpart in VS Code.
The fix is not a better flattener; it is to render the concept VS Code renders.

## Rendering rules that mirror upstream

- **Tables have no border strokes.** VS Code's Markdown-in-hover renders a plain
  `<table>` with no grid lines, so the divider a user sees is whatever the
  extension itself put in a column. Reproducing that means aligning columns into
  a padded grid and drawing *no* rules. Adding a border would make Sakura's
  hover diverge from VS Code precisely where the previous implementation was
  criticized. The layout keeps the literal `│` column as its own column, so the
  same glyph does the same job it does upstream.
- **`$(name)` is an icon only when `supportThemeIcons` was set.** That flag is
  the sole signal distinguishing "the extension meant a codicon" from "the
  extension literally typed `$(name)`", and it now survives the whole trip:
  `MarkdownString.supportThemeIcons` → the exthost wire payload → the dispatcher
  → `SExtensionStatusBarItem::tooltipSupportsThemeIcons` →
  `SParseOptions::supportThemeIcons`. With the flag clear the text stays
  literal, exactly as upstream. Do not reintroduce the old
  strip-`$(name)`-unconditionally behavior; it existed only because the flag
  used to be dropped at the shim.
- **The icon vocabulary is shared, not duplicated.** `$(name)` resolution goes
  through [`../icons/ThemeIconResolver.h`](../icons/ThemeIconResolver.h), the
  same header the status bar's `StatusBarItem.text` renderer uses, so an icon id
  can never mean one thing in the text and another in the hover. Resolution is
  global by icon id — contributed icons first, then a glyph of the bundled
  `codicon.ttf` — matching upstream's single `IconRegistry`. The hover must pass
  `CCodiconFont::Instance().FaceName()` as the built-in face, exactly as the
  status bar does; passing an empty face silently drops the hover back to the
  degraded vector/substitute-dot path while the status bar still draws real
  glyphs, which is precisely the "same id means two things" failure this bullet
  exists to prevent.
- **Colors map to `editorHoverWidget.*`.** Background, border, and foreground
  come from `theme::ThemePalette`, not from hard-coded constants, so the hover
  follows the theme like every other workbench surface.
- **The delay is `workbench.hover.delay`.** `kHoverDelayMilliseconds` is 500,
  VS Code's default. The host arms one timer per hovered target and disarms it
  on leave, click, item replacement, and window destruction.

## Bounded against untrusted extension input

Everything in a tooltip comes from an extension, so the parser fails soft and
never unbounded. It caps input characters, block count, table rows and columns,
inline-span lookahead, nesting depth, icon-id length, and entity lookahead, and
it caps *displayed* characters separately; exceeding the display budget appends
a visible `...` block rather than truncating silently. Unterminated markup is
left as literal text instead of swallowing the rest of the string. The layout
side caps positioned runs and content width. None of these paths throw.

Two orderings are load-bearing and must not be rearranged:

1. **Inline HTML is stripped before line splitting**, so a `<br>` can create a
   line break while every other tag disappears without an alt-text placeholder.
2. **HTML entities are decoded per literal fragment, after table cells are
   split.** Decoding earlier would let an encoded pipe (`&#124;`) become a real
   `|` and fabricate an extra column, desynchronizing a row from its header.
   A cell that decodes to only `&nbsp;` is treated as blank — U+00A0 is trimmed
   alongside ordinary whitespace, because `std::iswspace` does not classify it
   as space under the C locale — so the cell collapses exactly as it does in a
   browser-rendered hover.

**Documented divergence — entity coverage.** `kHtmlEntityTable` holds about
fifteen names plus all numeric (`&#NNN;` / `&#xHHH;`) references, encoding
non-BMP code points as correct UTF-16 surrogate pairs. It is not the WHATWG
HTML5 named character reference list (over 2000 names). A name outside the table
still renders literally. Extend the table when an extension is found to depend
on a name it does not yet cover.

## Documented divergence — the hover is non-interactive

`CHoverWidget` is created `WS_EX_NOACTIVATE | WS_EX_TRANSPARENT` and answers
`WM_NCHITTEST` with `HTTRANSPARENT`. Consequences, all deliberate:

- A `command:` link inside the hover renders as link-colored text but cannot be
  clicked. VS Code's hover does support that.
- The pointer cannot rest *inside* the hover to keep it open; moving toward the
  hover leaves the anchoring item and dismisses it.

The reason is that a hover which accepts mouse input covers its own anchor. The
host would see `WM_MOUSELEAVE`, hide the hover, immediately see `WM_MOUSEMOVE`
back over the anchor, and re-show it — a visible flicker loop. Upstream solves
this in the DOM with a hover-target/hover-element pair and a grace period;
reproducing that natively needs a real focus-and-grace state machine, which is
separate work. Until then a non-interactive hover is a *degraded presentation of
the same concept*, not a faked capability: nothing here pretends the link works.

## Host contract

A host (currently only `CMainStatusBar`; see
[`../../window/CLAUDE.md`](../../window/CLAUDE.md)) owns exactly one widget and:

- stores the **raw Markdown** on its hit target and calls `Parse` only at show
  time, so a repaint never re-parses;
- identifies the pending/visible target by its client-coordinate `RECT`, because
  hit targets are rebuilt on every paint and an index would go stale;
- forwards palette and icon-registry changes through `SetPalette` /
  `SetIconRegistry`, and hides the hover before replacing items or destroying
  the owner window.
