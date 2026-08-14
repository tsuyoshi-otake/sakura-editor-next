# Phase 6 Problems and Marker Guidance

## Ownership

`MarkerService` is the process-local, HWND-free authority corresponding to the
stable part of VS Code's marker service. It owns accepted diagnostic
collections, resource identity, deterministic Problems snapshots, and change
notifications. Document navigation, editor decoration, problem matching, and
native Problems rendering are adapters outside this directory.

## State and Lifecycle Rules

- Identify every producer by stable owner ID and nonzero generation. Reloading
  an owner advances its generation; an older callback can never replace or
  delete newer markers.
- Replace is atomic for one collection/resource. Empty replacement is an
  explicit delete. Invalid, stale, oversized, or revision-conflicting requests
  preserve the last accepted snapshot.
- Ranges are zero-based and half-open. Preserve URI identity and the complete
  start/end range through navigation; opening only the resource is not a
  compatible Problems action.
- Keep collection, resource, marker, payload, subscription, and queued
  notification work bounded. Snapshot ordering must not depend on insertion or
  callback order.
- Commit under the model lock and deliver queued notifications after unlocking.
  Listener reentrancy and exceptions must not strand delivery or mutate the
  already committed result.
- Bound retained owner identities as well as marker collections. A tombstoned
  owner generation is a lifetime fence and must not be evicted in a way that
  lets stale callbacks become current again.
- `Stop` is terminal. An external caller waits for active callbacks to drain;
  a callback-originated Stop returns the typed deferred result and transfers
  finalization to the safe outer delivery boundary. Service destruction from
  inside its own callback is unsupported because the callback still borrows the
  service. Stop clears retained data/subscribers and rejects every later
  mutation. Adapters must dispose only the exact owner generation they created.

## Verified Checkpoint

`CWorkbenchRuntime` owns `MarkerService`; `CEditWnd` projects authoritative
snapshots through a coalesced UI-thread gate; and extension diagnostic RPC
updates the service before best-effort legacy decoration projection. Atomic
collection clear removes only the exact owner generation and collection with
one committed revision. Missing/repeated delete, clear, and empty-set requests
are accepted desired-absence operations so legacy decorations cannot remain
after the authoritative marker is gone.

The integrated Phase 6 cohort passes 210/210. Task problem matchers remain
unwired. Native activation deliberately opens only the resource for now:
full-range navigation and decorations must first convert VS Code's zero-based,
half-open UTF-16 code-unit positions to Sakura document coordinates without
truncating the stored URI/range.
