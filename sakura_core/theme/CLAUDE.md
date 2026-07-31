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

High Contrast never imitates the translucency: the description role takes
`COLOR_WINDOWTEXT` and only the disabled role dims to `COLOR_GRAYTEXT`, so the
overlay cannot lower system-guaranteed contrast. Recompute both literals when a
canvas color changes; a palette field added anywhere but the end also updates
every positional initializer, including `HighContrastPalette` and the exact-token
tests.

Theme changes publish one revisioned snapshot and invalidate all affected
surfaces. Validate native and extension-contributed UI at supported DPI values
and in high-contrast mode. Missing tokens use a documented fallback; missing
capabilities are never silently presented as supported.
