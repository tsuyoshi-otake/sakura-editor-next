# Phase 5 Win32 Workbench Projection Guidance

## Scope

This directory translates immutable, presentation-neutral Workbench snapshots
into native projection DTOs. It may know Win32-facing physical edges, but it
must not own contribution registration, layout state, durable mementos, command
policy, profile state, or native control lifetime.

## Built-in Part Projection

- `ProjectBuiltinParts` consumes one complete `WorkbenchLayoutStateSnapshot`.
  It maps Sidebar to physical left, Panel to physical bottom, and Auxiliary Bar
  to physical right.
- Success contains all three required Parts with their visibility and committed
  DIP extent. Unsupported schema, missing or duplicate required IDs, unexpected
  physical positions, and zero/oversized extents return a typed failure with no
  projection.
- Unknown unrelated Parts are ignored so a newer registry can coexist with this
  bounded adapter. An invalid required Part can never be ignored.
- Projection is deterministic and independent of descriptor order. It performs
  no HWND calls, model mutations, storage access, focus transfer, or logging.

## Active Surface Projection

- `ProjectBuiltinActiveSurfaces` is a separate pure projection of committed
  active ViewContainer/View state. It has no HWND, `CShareData`, tool, storage,
  logging, mutation, or focus-transfer dependency, and it does not change the
  independent `ProjectBuiltinParts` physical-part contract.
- The verified active mappings are Sidebar `Explorer/Explorer`,
  `Explorer/Outline`, and `SourceControl/SourceControl`; Panel
  `Terminal/Terminal`, `Problems/Problems`, and `Output/Output`; and Auxiliary
  Bar `LegacyExtensionViewsAuxiliary/LegacyExtensionViews`. `Editor` is a
  focus-only surface and is never an active ViewContainer/View slot.
- Activation never implies focus. The projection emits focus only for an
  explicit, coherent model focus: its container/view must be active and
  visible, the View must belong to that container and be its active View, and
  the explicit or implied physical Part must exist and be visible. A valid
  editor-only fallback focus projects to the focus-only Editor surface.
- Unsupported active pairs and malformed active/focus hierarchies return one
  typed terminal failure with no partial logical projection. Unsupported
  surfaces are Search, RunAndDebug, the canonical Sidebar Extensions
  contribution, Ports, DebugConsole, and unknown future contributions.
- `x64\Debug\tests1.exe --gtest_filter=BuiltinPartProjection.*` passed 14/14
  after the integration build with zero errors; the required `tests1.exe` and
  Sakura-process survivor audit was clean. This verifies the pure projection
  boundary only, not native-host application or command routing.

## Dependency and Completion Boundary

The Window adapter owns UI-thread application and preserves the last native
state if this projection fails. Layout mutations always go through
`WorkbenchLayoutStateService` before projection; this directory is never a
second authority.

The current checkpoint covers built-in Part visibility/extent and the bounded
active ViewContainer/View/focus mapping above. A later generic adapter must
still project arbitrary host identity, visibility, ordering, movement, and
panel position/alignment. Those capabilities remain unsupported even though the
pure layout model can represent them.
