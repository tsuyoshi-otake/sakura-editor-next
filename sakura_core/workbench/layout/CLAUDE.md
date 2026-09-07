# Phase 1 Workbench Layout Guidance

## Pure Layout Boundary

`WorkbenchLayoutStateService` and `WorkbenchLayoutMementoCodec` are pure layout
components. They own stable-ID state and bounded memento encode/decode behavior,
but do not depend on HWND, native editor controls, profile paths, storage, IPC,
or process identity. A composition adapter outside this directory performs all
durable I/O.

## Extension Contribution Ownership (Issue #296)

- Registry access and publication are serialized by the native composition
  owner. Prepare copies and validates a whole candidate without changing live
  descriptors; Commit checks registry identity and base revision before a
  non-allocating move. Abandoning a candidate preserves the old owner.
- Registered Views and ViewContainers carry a native-issued owner/generation.
  Built-ins and the legacy v1 startup batch remain unowned. Dynamic owners are
  bounded to 64, with at most 16 containers and 64 Views each. Generations are
  globally increasing within this catalog, including after disposal; stale
  preparation is rejected and never retried automatically.
- `DisposeOwner` removes exactly the matching generation without allocating or
  calling observers. New registration reserves 64 revision values for terminal
  owner removal. No View may retain a dependency on a disposed foreign owner.
- Recorded unsupported SENP boundary: v2 supports its own containers and
  product-owned containers. A different extension's container returns typed
  `Unsupported`; an unknown destination is invalid. VS Code supports foreign
  containers and moves remaining Views into Explorer when a container is
  removed, then restores their original container when it returns (verified
  against `viewsExtensionPoint.ts` at
  `e6aeab60511647b9b00f0bf2f0f02b15f278abb5`). The current native page pool lacks
  cross-provider View retention and fallback relocation transactions, so SENP
  must not claim or approximate that capability. Implement both lifetimes and
  relocation before widening this boundary. This restriction does not apply to
  normal user movement of an already supported View/Part in the layout model.

## Phase 1 Layout-Memento Persistence Checkpoint (2026-07-31)

- The adapter persists one profile-scoped `workbench.layout` key at `Machine`
  target. Process PID and native-window identity are transient and are never a
  durable key or serialized memento field.
- It loads before native-window creation and exposes typed `Loaded`, `NotFound`,
  `InvalidStoredMemento`, `Unavailable`, or `Failed` results. Only `Loaded`
  reaches atomic hydration.
  Corrupt or unsupported durable data is `invalid` and remains untouched.
- It captures the global storage revision for the shutdown Apply CAS. A conflict
  preserves remotely committed state, with no refresh-and-overwrite behavior.
- It saves once only at orderly shutdown, after native-window destruction and
  before editor-control runtime stop; unchanged mementos skip the write. Because
  the current file backend is O(N) per write, never persist on a layout event.
- An ambiguous Apply outcome can replay once only, with the exact same durable
  operation ID. No other failure gets an automatic retry or replacement ID.
- Direct durable writes from views, editor controls, or these pure layout classes
  are forbidden. The interim policy is profile-shared: the last successful,
  nonconflicting orderly closer wins. Stable logical window/workspace identities
  remain deferred.

`CControlPlatformWorkbenchLayoutMementoStore` is the production composition
adapter. Its focused adapter/runtime/state/codec run passes 42/42 tests, and the
broader platform/configuration/workbench run passes 214/214. These results prove
this profile-shared Phase 1 boundary only; stable logical window/workspace
identity remains deferred.

## Phase 5 Live-State Projection Contract (2026-07-31)

- Built-in defaults follow the pinned VS Code layout: Sidebar visible, Panel and
  Auxiliary Bar hidden. Defaults are model state, not Win32 constructor policy.
- A mutation commits the complete state under the service lock, releases the
  lock, and only then queues listener delivery. Listener-triggered mutations are
  delivered in revision order without recursive notification.
- Projection adapters validate the whole snapshot before touching a native
  host. Missing/duplicate built-in Parts, unsupported positions, and invalid
  extents are one terminal projection failure and never a partial application.
- This directory owns active View, focus, placement, order, and panel-alignment
  truth even where the current Win32 adapter cannot yet render them. Never
  downgrade those fields to HWND-derived state or silently report them applied.
- Each physical ViewContainer location owns at most one live active container,
  and owns exactly one whenever that location has a visible candidate. Selection
  is deterministic by `(order, stable container ID)` when a fallback is needed.
  Container visibility, activation, active View, and keyboard focus are separate
  state axes.
- A live focus target must be a visible, active, coherent hierarchy: a View
  belongs to its stated container, the container is active for its location, and
  any stated Part matches that location and is visible. Mutations that hide,
  move, or dispose the focused hierarchy fall back to a visible Editor Part;
  activation never assigns focus to the newly activated container.
- Structurally valid persisted active/focus IDs from contributions that are not
  registered yet remain deferred and are materialized only when the complete
  visible hierarchy becomes live. A registered View whose persisted target
  container is still unknown remains live at its default/current placement while
  only the target placement is deferred. An ID that was live and is then
  disposed is dropped and must never be resurrected by a later owner generation.
- Layout memento writers emit canonical format v2 with `activeContainers`.
  Readers accept legacy v1 as empty active-container intent and upgrade it on
  the next write. A v2 payload without a bounded `activeContainers` object is
  corrupt; newer format versions are unsupported and remain untouched.
