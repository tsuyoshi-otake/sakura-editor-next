# Frame-Coordinated Rendering Foundation

## Stable boundary

`FrameCoordinatorModel` is a pure, single-writer scheduling model. It owns no
thread, HWND, worker, Direct2D/Direct3D/DXGI/DirectComposition object, callback,
or wait. The per-window presentation owner is the only production caller that
may mutate it.

`FrameCoordinatorRuntime` supplies that presentation-owner thread. Producers
publish through bounded queues and payload-free event wakes; only the owner
mutates the model. Content callbacks run outside the runtime mutex. UI teardown
uses non-blocking `BeginClose()` and transfers final `Wait()` ownership to an
external non-UI owner; self-wait fails closed. Cadence is an explicit/compositor
tick, never a fixed 16 ms application timer.

`FrameDeviceDomainModel` is the pure recovery state machine beside the
coordinator. One loss advances `DeviceEpoch` once, stops submission, and reaches
Hardware, WARP, coherent software fallback, or Closed through explicit states.
Hardware reprobe is bounded by exponential backoff and advances the epoch before
replacement so completion from the previous domain cannot publish.

## Invariants

- Atomicity means one coherent `LayoutEpoch`; it never means waiting for every
  dirty surface to become ready.
- Each surface has one latest-only pending content request. A newer request
  replaces pending work and supersedes active work at a safe boundary.
- Every request and completion is fenced by surface lifetime, request, content,
  layout, and device epochs. A stale completion never mutates current state.
- GPU work that has crossed submission is withdrawn from publication and
  explicitly retired; it is never presented merely because cancellation was
  late.
- Scheduling is deterministic, interaction-aware, and aging-bounded so a
  continuously interactive surface cannot starve background work forever.
- Device reset is a window device-domain transition. It invalidates all queued
  and active frame work from the previous device epoch.
- Presentation/device callbacks never run while a coordinator mutex is held,
  and one stalled surface never becomes a readiness barrier for another.
- No state transition silently terminates in an intermediate state. Closed,
  idle/requested, publishable, and withdrawn work have explicit owners and next
  operations.

## Migration rule

Consumers first publish CPU-only immutable plans through this contract. Do not
move existing HWND or GPU ownership into a worker while connecting a surface.
SCM is the first native consumer under Issue #238; Editor, Markdown, Terminal,
and the other ViewContainers follow through separate, reversible slices.
