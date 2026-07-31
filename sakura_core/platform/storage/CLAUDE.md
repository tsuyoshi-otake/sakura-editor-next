# Durable Storage Guidance

## Ownership and Durability

- `CAtomicFileStorageService` is a transitional control-process-only durable
  `IStorageService` backend. `Open()` retains its exclusive writer lock for the
  service lifetime; editor processes access it only through control IPC.
- A successful mutation means the versioned/checksummed state and bounded replay
  ledger reached flushed atomic replacement. Revision/CAS and durable
  `operationId` replay semantics must match `CInMemoryStorageService`.
- The full-file rewrite is O(N). Keep the public storage contract independent of
  this backend so a later database implementation can replace the dominant
  serial write path without changing callers.

## Failure and Security Contract

- Busy writer, corrupt data, unsupported schema, generation rollback/exhaustion,
  bounds failure, and I/O failure are distinct terminal outcomes. Composition
  decides whether the legacy application may continue without the platform
  service; it must not publish an empty replacement store under the same profile
  identity and generation.
- Replay records, entries, encoded bytes, snapshot size, subscriptions, and
  revisions stay bounded. Validation completes before durable state changes, and
  a failed atomic replace leaves the last committed file authoritative.
- This service is not a secret backend. Reject secret namespaces and keep secret
  values out of settings, Memento, exports, snapshots, diagnostics, and logs.

## Phase 1 Layout-Memento Persistence Checkpoint (2026-07-31)

- The layout adapter stores exactly the profile-scoped `workbench.layout` entry
  at target `Machine`; neither a process PID nor a transient native-window token
  may become durable identity.
- Before a changed shutdown write, the adapter captures the storage snapshot's
  global revision and uses it as the Apply CAS precondition. An Apply conflict is
  terminal for that save: preserve the remote entry, do not refresh-and-overwrite
  it automatically, and report the conflict to composition.
- The durable operation ID is generated once per intended save. Only an ambiguous
  post-Apply result can be replayed, using that identical ID, and it is bounded to
  one retry. Other failures have their own typed terminal outcomes.
- This backend rewrites the full state file in O(N). Therefore a layout event is
  never an authority to mutate durable storage; the Phase 1 adapter writes once
  only during orderly shutdown and skips an unchanged encoded memento.
- Corrupt or unsupported `workbench.layout` bytes are read as an `invalid` typed
  restore outcome and must not be overwritten merely because a later startup or
  shutdown occurs.

The control-platform adapter now implements these gates. Focused CAS, invalid
preservation, conflict, exact-replay, and retry-exhaustion tests pass; this does
not change the full-file O(N) backend or authorize per-event writes.
