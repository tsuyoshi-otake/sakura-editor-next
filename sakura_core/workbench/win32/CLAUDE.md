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
- The verified active mappings are the side-bar pair `Explorer/Explorer`,
  `Explorer/Outline`, `SourceControl/Changes` (`workbench.scm`), and
  `Extensions/Extensions`; and Panel `Terminal/Terminal`, `Problems/Problems`,
  and `Output/Output`. `Editor` is a focus-only surface and is never an active
  ViewContainer/View slot. The native Source Control host also paints the
  non-selectable `workbench.scm.repositories` and `workbench.scm.history`
  sections; they intentionally do not enter this mapping until the layout model
  can express VS Code's provider-driven visibility conditions.
- A mapping declares a **set** of locations, not one. VS Code renders the same
  composite bar in the Primary and the Secondary Side Bar and its
  `CompositeDragAndDrop.drop` moves an Activity Bar ViewContainer between them,
  so every side-bar mapping is valid in `SideBar` and `AuxiliaryBar` alike and
  projects into whichever slot the model says owns it. A ViewContainer has
  exactly one location, so exactly one of `sidebar` / `auxiliaryBar` can carry
  it.
- Panel mappings remain Panel-only. Relocating the whole Panel is VS Code's
  separate `workbench.action.movePanelToSecondarySideBar` family and is still an
  unsupported gate, so `Terminal`, `Problems`, or `Output` placed in the
  Auxiliary Bar fails closed rather than being approximated.
- The Secondary Side Bar still starts empty. That is a default, not a capability
  limit: an empty auxiliary slot means no container has been moved there yet,
  and it must never be populated with a placeholder container to make the right
  edge look inhabited.
- Activation never implies focus. The projection emits focus only for an
  explicit, coherent model focus: its container/view must be active and
  visible, the View must belong to that container and be its active View, and
  the explicit or implied physical Part must exist and be visible. A valid
  editor-only fallback focus projects to the focus-only Editor surface.
- Unsupported active pairs and malformed active/focus hierarchies return one
  typed terminal failure with no partial logical projection. Unsupported
  surfaces are RunAndDebug, Ports, DebugConsole, and an AuxiliaryBar
  placement of a Panel-only container. Search is supported: `viewContainer::Search`
  / `view::Search` are mapped in `kSideBarLocations` and project into either side
  bar exactly as Explorer and Source Control do.

## Dependency and Completion Boundary

`ProjectBuiltinWorkbench` is the composite boundary used by the Window adapter:
it validates the physical Part projection and the active ViewContainer/View
projection from the same committed snapshot, and exposes neither half when
either one fails. The Window adapter owns UI-thread application and preserves
the last native state if this projection fails. Layout mutations always go
through `WorkbenchLayoutStateService` before projection; this directory is
never a second authority.

The current checkpoint covers built-in Part visibility/extent and the bounded
active ViewContainer/View/focus mapping above. A later generic adapter must
still project arbitrary host identity, visibility, ordering, movement, and
panel position/alignment. Those capabilities remain unsupported even though the
pure layout model can represent them.
