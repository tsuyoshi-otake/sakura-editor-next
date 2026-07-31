# P4 Theme Guidance

Theme services own semantic color, icon, typography, density, focus, hover,
selection, error/warning, and high-contrast tokens. Workbench/window/view code
consumes tokens and must not infer semantics from raw RGB values.

Theme changes publish one revisioned snapshot and invalidate all affected
surfaces. Validate native and extension-contributed UI at supported DPI values
and in high-contrast mode. Missing tokens use a documented fallback; missing
capabilities are never silently presented as supported.
