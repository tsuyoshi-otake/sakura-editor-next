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

## Status-bar prominent-background role (2026-08-07, #36)

`ThemePalette.statusBarProminentBackground` maps VS Code's
`statusBarItem.prominentBackground`, the fill behind the far-left Restricted
Mode status-bar entry (`status.workspaceTrust`; see `../window/CLAUDE.md` for
the paint/click side). It is the **last** field in `ThemePalette` — the struct
is positionally initialized, so a field added anywhere else silently
reinterprets every existing positional literal. Every positional initializer
was updated in lockstep in the same change: both `PaletteFor` branches,
`HighContrastPalette`, `CColorThemeRegistry::ProjectPalette`'s JSON-key
mapping, and the exact-token tests in `CThemeServiceTest.cpp` and
`CColorThemeRegistryTest.cpp`. Grep for `ThemePalette{` / `ThemePalette
expected{` / `return {` inside `CThemeService.cpp` before adding or removing a
field; a missed site fails silently (it still compiles, it just assigns colors
to the wrong roles).

Upstream registers **one** non-per-theme default for this token —
`Color.black.transparent(0.5)` — applying identically to dark, light, hcDark,
and hcLight. GDI has no alpha channel, so the value is stored pre-composited,
like the other translucent tokens in "Translucent VS Code Tokens" above — but
composited over a different substrate than those. The translucent tokens above
composite over `canvas` because upstream paints them over the editor
background. This one composites over the *resolved* `accent` (the status bar's
own background) instead, because it is the status bar's own translucent fill,
and `accent` is what it visually sits on: dark `#0F4569` and light `#5C1934`,
each `black.transparent(0.5)` blended over that mode's accent
(`#1F8AD2` / `#B83268`) using the shared `(channel * 127 + 127) / 255`
round-to-nearest convention. `CColorThemeRegistry::ProjectPalette` composites
over the palette's own already-resolved `accent`, not the compiled default, so
a theme that overrides `focusBorder`/`textLink.foreground`/`button.background`
also shifts this role's fallback — proven by
`CColorThemeRegistryTest.cpp`'s `DiscoversLoadsAndProjectsJsoncThemeWithInclude`
case. High Contrast has no compiled literal at all: `CompositeBlackHalfOver`
(a private helper in `CThemeService.cpp`) composites the same formula, at read
time, over the live `COLOR_HIGHLIGHT` — the same system color High Contrast
already uses for `accent` — because High Contrast colors must track whatever
the OS reports, not a value chosen ahead of time.

The role could not reuse `accent`, `danger`, or `warning`. `accent` is the
surface this role composites *over*, not a substitute for it — reusing it
would mean the Restricted Mode item painted no fill of its own at all, which
is not what upstream's registration says. `danger` and `warning` are
foreground roles drawn from unrelated upstream tokens
(`list.errorForeground`-family colors), not the neutral translucent black wash
`statusBarItem.prominentBackground` actually is; borrowing either would tint
Restricted Mode as an error/warning state VS Code does not intend. No new
foreground role was added: the label reuses the existing `highlightText` role,
which already equals `statusBarItem.prominentForeground`'s upstream default
(`#FFFFFF`) in both dark and light, so a second foreground field would have
been redundant.

This is also the **one exception** to the status bar's flat single-fill
painting model. Every other item — SCM commands, extension items, the
built-in editor entries, notifications — is drawn on top of one bar-wide
`FillSolidRect(target, client, m_palette.accent)` and never paints its own
background. The Restricted Mode item fills its own item rectangle with
`m_palette.statusBarProminentBackground` before drawing its label runs,
because upstream's `statusBarItem.prominentBackground` is exactly that: a
per-item override of the bar's own background, not a shared one. Do not
generalize this into a per-item background mechanism for other entries; it is
scoped to this one token because this is the only status-bar item upstream
gives its own background color to.

## Banner Part color roles (2026-08-07, #38)

`ThemePalette` carries `bannerBackground` / `bannerForeground` /
`bannerIconForeground`, mapping VS Code's `banner.background`,
`banner.foreground`, and `banner.iconForeground` — the Banner Part shown under
the title bar, of which Restricted Mode's "This workspace is not trusted"
strip is the motivating instance, though the Part itself is a generic upstream
banner mechanism. They are the **last three** fields in `ThemePalette` —
following the same positional-initialization discipline as
`statusBarProminentBackground` above, every site was updated in lockstep in
the same change: both `PaletteFor` branches, `HighContrastPalette`,
`CColorThemeRegistry::ProjectPalette`'s JSON-key mapping, and the exact-token
tests in `CThemeServiceTest.cpp` and `CColorThemeRegistryTest.cpp`.

All three roles are registered upstream as **bare aliases** of another color,
not an independent per-kind object, which is a different shape from
`statusBarProminentBackground`'s single translucent literal above:

- `banner.background` is `{ dark: list.activeSelectionBackground, light:
  darken(list.activeSelectionBackground, 0.3), hcDark/hcLight:
  list.activeSelectionBackground }`. Dark stays `#04395E` unchanged; light is
  `darken(#0060C0, 0.3)` = `#004386`, computed by hand with the same HSL
  `AdjustLightness` reproduction this codebase already applies for
  `button.hoverBackground` (the compiled `PaletteFor` literals cannot call the
  runtime helper, since HSL derivation is not `constexpr` here).
- `banner.foreground` is `list.activeSelectionForeground`, `#FFFFFF` in both
  modes.
- `banner.iconForeground` is `editorInfo.foreground` (`#59A4F9` dark, `#0063D3`
  light) — the editor's "info" diagnostic blue, a color family no other
  `ThemePalette` role represents.

`CColorThemeRegistry::ProjectPalette` offers each alias name as a second
fallback candidate behind the primary token, the same multi-candidate shape
`accent`'s chain already uses: a theme that sets
`list.activeSelectionBackground`/`list.activeSelectionForeground`/
`editorInfo.foreground` but not `banner.*` directly still reaches the color
VS Code's own default resolution would produce, and a theme that sets
`banner.*` directly still wins over the alias. Both directions are proven by
`CColorThemeRegistryTest.cpp`'s `ProjectsBannerRolesFromAliasedTokensWhenBannerKeysAreAbsent`
and `PrefersDirectBannerTokensOverTheirAliasedCandidates`.

None of the three roles could reuse an existing field. `accent` is a different
question (what focus/prominent UI looks like) already answered by a disjoint
token family; `highlightText` equals `list.activeSelectionForeground`'s value
today but is a status-bar-scoped role by name and could diverge from the
banner's own alias chain if either is themed independently; `danger` and
`warning` are unrelated error/warning foregrounds, not this info-diagnostic
blue. A picked "info blue" literal was also rejected for High Contrast:
`HighContrastPalette` gives `bannerBackground` the same system `highlight`
color it already uses for `accent` (both are ultimately `hcDark`/`hcLight`
aliases of `list.activeSelectionBackground`), and gives both
`bannerForeground` and `bannerIconForeground` the paired `highlightText` —
rather than upstream's own hardcoded `hcDark`/`hcLight` literal for
`editorInfo.foreground` — because a chosen literal cannot promise the contrast
an arbitrary system High Contrast theme guarantees, the same reasoning already
applied to the three button roles and to `statusBarProminentBackground`.
