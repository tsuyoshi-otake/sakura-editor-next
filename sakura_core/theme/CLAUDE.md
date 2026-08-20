# P4 Theme Guidance

Theme services own semantic color, icon, typography, density, focus, hover,
selection, error/warning, and high-contrast tokens. Workbench/window/view code
consumes tokens and must not infer semantics from raw RGB values.

## Translucent VS Code Tokens

VS Code defines several foreground tokens as `transparent(base, alpha)`, but GDI
text rendering has no alpha channel. Those tokens are therefore stored
pre-composited over `canvas` in `ThemePalette`, and the derivation belongs in a
comment beside the literal so it stays checkable:

- `descriptionText` is `descriptionForeground`. Dark is
  `transparent(foreground #CCCCCC, 0.7)` over the `#1E1E1E` canvas, which is
  `#989898`; light is VS Code's own `#717171` literal.
- `disabledText` is `disabledForeground` (`#CCCCCC80` / `#61616180`) composited
  the same way.
- `diffInsertedLineBackground` and `diffRemovedLineBackground` are
  `diffEditor.insertedLineBackground` / `diffEditor.removedLineBackground`, whose
  registered defaults are the shared `defaultInsertColor`
  `rgba(155, 185, 85, .2)` and `defaultRemoveColor` `rgba(255, 0, 0, .2)` for
  both dark and light. `diffDiagonalFill` is `diffEditor.diagonalFill`
  (`#cccccc33` dark, `#22222233` light), the hatch drawn where one side of a
  side-by-side diff has no line at all. All three are backgrounds rather than
  foregrounds, but they composite over `canvas` for the same reason: upstream
  paints them over the editor background, not over the gutter.

High Contrast never imitates the translucency: the description role takes
`COLOR_WINDOWTEXT` and only the disabled role dims to `COLOR_GRAYTEXT`, so the
overlay cannot lower system-guaranteed contrast. The three diff roles register
`null` for `hcDark`/`hcLight` upstream — High Contrast paints no inserted or
removed wash and no diagonal fill at all — so `HighContrastPalette` gives them
the window color, which is that absence rather than a chosen highlight color.
Recompute every literal when a canvas color changes; a palette field added
anywhere but the end also updates every positional initializer, including
`HighContrastPalette` and the exact-token tests.

Theme changes publish one revisioned snapshot and invalidate all affected
surfaces. Validate native UI at supported DPI values
and in high-contrast mode. Missing tokens use a documented fallback; missing
capabilities are never silently presented as supported.

## Bundled color-theme boundary

`CColorThemeRegistry` owns the bundled Sakura dark and light JSONC themes and
resolves `workbench.colorTheme` by their stable ids or labels. `colors` entries
are projected into the semantic `ThemePalette`; translucent values are
pre-composited before they reach GDI controls. External package discovery and
externally contributed themes are not supported.

`ThemePalette.sideBar` represents `sideBar.background` for the Primary Side Bar
and its Explorer/Source Control containers. `ThemePalette.panel`
represents the Secondary Side Bar/legacy right-host surface, while
`ThemePalette.bottomPanel` represents the distinct `panel.background` role used
by the bottom Panel and its Problems/Output tools. Sakura's embedded Dark
default deliberately keeps Explorer at `#293134` and the right/Outline/Panel
surfaces at `#252526`; keep those roles separate when adding a Workbench
consumer.

## Active color theme kind

`CThemeService` holds the process-local active-theme projection: the palette
overlay, the syntax overlay, and `IsActiveColorThemeLightKind()`. That last one
is a published fact about the loaded theme, not a colour and not the saved
`ThemeMode`. VS Code emits a file icon theme's `light` section under the `.vs`
body class alone, which is exactly `ColorThemeKind.Light`; High Contrast and
High Contrast Light keep the base section even though `ModeForKind` maps the
latter to `ThemeMode::Light`. `CEditWnd::ApplyWorkbenchTheme` is the single
writer and assigns it on every path, including the fallback and catch paths, so
no reader can observe a value left over from a previously selected theme.

Consumers pull it the way `CEditView_Paint` pulls the syntax overlay. Do not
thread it through Part hosts or add it to a palette struct: a host that renders
a borrowed page must not carry theme facts the page alone reads, and inferring
the kind from background brightness would silently give High Contrast Light the
wrong icon colours.

`tokenColors`, `semanticTokenColors`, and `semanticHighlighting` are parsed and
projected into the editor's typed native categories (comment, string, number,
keyword, type, function, variable, constant, regexp, tag, attribute, and
invalid). The projection preserves the source rules and applies the published
rule order, and semantic foregrounds override TextMate foregrounds when
`semanticHighlighting` is true. This is real rendering integration, but it is
not a full TextMate grammar/scope-selector engine: unknown scopes and italic
font styles remain explicit unsupported portions of the boundary.

`TextMateScopeColorResolver` (`TextMateScopeColorResolver.h/.cpp`) is a second,
narrower boundary: given a full scope path (outermost to innermost, the same
shape `sakura_core/textmate`'s tokenizer produces per token) and a theme's
already-parsed `ThemeTokenColorRule` list, it resolves the single
highest-specificity matching rule using VS Code's own dot-segment specificity
scoring and later-rule-wins tie-break. It has no compile dependency on
`sakura_core/textmate` — callers pass a plain `std::vector<std::wstring>` — so
it can be exercised and tested independently of the grammar engine's own
completeness. It supports only whitespace-separated descendant-combinator
selectors; the `>`, `-`, and `|`/`,` combinators are not recognized and fail
closed rather than being misinterpreted. No production caller wires real
per-token TextMate scopes into it yet: see `sakura_core/textmate/CLAUDE.md`
for the tokenizer side of that still-missing connection.

The registry owns the two built-in Sakura defaults as embedded JSONC theme
documents (`sakura.default-dark` and `sakura.default-light`).
`CEditWnd::RefreshColorThemes()` registers them on every refresh, so the Color
Theme picker, `workbench.colorTheme` writeback, JSONC loading, and
`ProjectPalette` projection use one path. An empty or unknown setting resolves
to the built-in theme matching Sakura's saved Dark/Light preference. The compiled
`CThemeService::PaletteFor()` values remain the deterministic fail-closed fallback
when the registry itself cannot load; focused tests require the embedded
documents to produce exactly the same `ThemePalette`.

## Button role colors (2026-08-06)

`ThemePalette` carries `buttonBackground` / `buttonForeground` /
`buttonHoverBackground`, mapping VS Code's `button.background`,
`button.foreground`, and `button.hoverBackground`. They exist because upstream's
`media/updateTitleBarEntry.css` paints the actionable title-bar Update button
with exactly those three variables; the button role is therefore **not** a
synonym for `accent`, even though `accent` already lists `button.background` as
one of its own fallback candidates. The two answer different questions — what
focus looks like versus what a prominent button looks like — and a theme is free
to set only one of them.

`button.hoverBackground` is derived from the *resolved* background rather than
from the compiled default when a theme supplies no explicit value, using
upstream's own registration: `lighten(button.background, 0.2)` for dark themes
and `darken(..., 0.2)` for light. The local `AdjustLightness` helper reproduces
VS Code's `lighten`/`darken`, which scale HSL lightness by `l * factor` and leave
hue and saturation alone. Deriving rather than defaulting is what prevents a
hover color from one theme sitting on a background from another.

The built-in defaults keep Sakura's own accent (`#1F8AD2` dark, `#B83268` light)
instead of importing VS Code's `#0E639C`/`#007ACC`, consistent with the rest of
 the compiled fallback palette.

## `gitDecoration.*` colors (2026-08-20, #229)

`ThemePalette` carries the ten `gitDecoration.*` roles the Git extension
registers, and `CColorThemeRegistry` resolves each from the loaded theme by its
upstream key. The built-in dark/light/High Contrast values are the extension's
own registered defaults, copied verbatim from its `contributes.colors`; do not
hand-tune them to match a screenshot.

**Divergence:** the High Contrast palette uses the extension's `highContrast`
(hcDark) values for both contrast themes. This product exposes one High Contrast
palette rather than VS Code's separate hcDark and hcLight kinds, and these
colors register no `highContrastLight` section upstream anyway.

The Explorer never reads these members directly. `CViewContainerPages` copies
them into `ExplorerPalette::decorationColors` indexed by `EFileDecorationColor`,
so the view resolves a role rather than a theme key. See
[`../workbench/explorer/CLAUDE.md`](../workbench/explorer/CLAUDE.md).
