# P0 Control IPC Guidance

## Scope

This directory owns the private, versioned IPC boundary between editor
processes and the control-process platform service host. It is not an extension
host protocol and must not depend on Node, JSON-RPC, HWND, `CShareData`, or the
legacy profile INI ABI.

## Trust Boundary

- Use local named pipes and endpoint metadata protected by a non-inheriting,
  current-user-only ACL. Reject remote clients, unsafe endpoint names, profile
  mismatches, stale generations, and unexpected peer process identities.
- The server verifies the impersonated client's user SID and always calls
  `RevertToSelf` before executing a service operation.
- Treat every frame and endpoint snapshot as untrusted. Validate length,
  version, kind, flags, identifiers, UTF-8, field counts, and configured bounds
  before allocation or dispatch.

## Protocol and State

- Transport `requestId` correlates one connection request. Durable mutation
  `operationId` is a separate replay identity and must survive reconnects.
- Every accepted request reaches exactly one terminal response. Cancellation,
  deadline, shutdown, overload, decode failure, and transport loss have
  explicit outcomes and owners.
- Mutations use the control-owned storage service's revision/CAS and replay
  semantics. Never generate a new operation ID when retrying an ambiguous
  transport failure, and never hide a storage conflict behind an automatic
  retry.
- Bound frame size, session count, pending requests, queued bytes, retry count,
  and all waits. Shutdown stops accepts, cancels I/O, resolves or closes pending
  work, joins workers, then withdraws endpoint metadata.

## P0 Slice

Implement and verify `Hello`, storage snapshot, storage apply, cancellation,
and terminal error framing first. Remote change subscriptions wait for a
revisioned push/resynchronization contract; polling is not an acceptable
substitute.

## Profile Command Slice

- `ProfileRequest`/`ProfileResponse` carries payload version 1 through one
  bounded `ProfilePayload` TLV after the normal storage Hello has pinned the
  endpoint profile and generation. A request may never supply an authority
  profile identity, HWND, PID, or filesystem path.
- The server delegates only to `ControlUserDataProfileRegistry`; it returns
  immutable snapshot/list/current/resolve views and forwards named/transient
  create, rename, delete, URI/empty-window association, import, and export.
  Durable mutations preserve operation-ID replay and storage-revision CAS.
- The synchronous slice has no pending operation to fake-cancel. The editor
  client reports pre-dispatch cancellation and local deadline expiry as typed
  terminal outcomes; adapter shutdown returns `ServerStopping` and closes the
  session. Any future asynchronous operation must add an owned pending-state
  cancellation contract before accepting a cancel frame.

## Implementation Checkpoint (2026-07-31)

- `ControlIpcProtocol` now provides the bounded, versioned byte-stream framing,
  request/response direction validation, explicit terminal statuses, UTF-8/TLV
  validation, sticky decode failure, and forward-compatible minor fields.
- `ControlIpcSecurity` and `ControlPlatformEndpoint` now provide canonical
  profile-hash endpoint names, protected current-user ACLs, named-pipe peer SID
  validation, and generation/lifecycle/PID-checked endpoint discovery. Endpoint
  ABI v2 adds the immutable canonical `profileId` to the fixed seqlock payload;
  old ABI payloads and malformed or mismatched identities are rejected instead
  of being interpreted. The control owner acquires the ID before publishing
  `Starting`/`Accepting`; editors consume it only from the endpoint descriptor
  and reject mismatches before opening the pipe. A client connects with
  `SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION`; after one bounded read, the
  server verifies the impersonated current-user SID, obtains the client PID,
  and reverts before dispatch.
- `ControlStorageRpc`, `ControlStorageRpcServerAdapter`, and the bounded
  named-pipe transport now implement `Hello`, snapshot, apply, cancellation,
  and terminal-error framing over per-connection sessions. Frame sizes,
  session count, queued bytes, and I/O waits are bounded; transport shutdown
  cancels accepts/I/O and joins session workers before returning. A
  callback-initiated stop only requests cancellation; worker identity is kept
  independently of the mutable session registry so ownership transfer during
  an external join cannot turn callback shutdown into an AB/BA deadlock.
- `CControlPlatformServiceHost` is the UI-independent composition root. It
  derives the pipe name exclusively from the endpoint profile hash, publishes
  `Starting` only after endpoint creation and `Accepting` only after a successful
  pipe bind, and serializes typed start/stop outcomes. Shutdown and failed-start
  rollback close the adapter gate, stop/join the pipe server, stop the adapter,
  best-effort publish `Stopped`, then close the endpoint; no lifecycle branch
  may remain in an intermediate state.
- `CControlPlatformClient` provides one bounded, deduplicated discovery ->
  connect -> Hello -> full-snapshot attempt, independently confirms `profileId`
  and generation in the endpoint and Hello acknowledgement, and invalidates and
  fences its cache during resnapshot. Retry is bounded with exponential backoff
  and jitter. Its ready-only storage Apply path uses a fresh authenticated
  channel, retains the caller's immutable operation ID, publishes committed
  changes into the synchronized cache, and exposes conflict, cache-gap,
  generation-change, shutdown, and ambiguous post-Apply transport loss as
  separate terminal outcomes. Only the last outcome permits replay, with the
  same operation ID; Apply never starts hidden bootstrap or retry work.
- `CControlPlatformRuntime` binds authority acquisition, durable storage open,
  and host startup into one typed lifecycle and rolls back in exact reverse
  order. `CControlProcess` now owns that runtime and publishes the legacy
  control-ready handoff only after the endpoint is `Accepting`.
- `CEditorControlPlatformRuntime` freezes the first trusted endpoint identity,
  owns one discovery reader/client/synchronized cache and exactly one retry
  worker, and requires Hello plus a full generation-matched snapshot before
  publishing Ready. Real `CNormalProcess` instances start this owner before
  plugins/workbench consumers and stop it after their destruction; processes
  that only forward a file to an existing window do not acquire a session.
- `CEditorControlPlatformRuntime` is also the narrow editor-side storage writer
  facade. It fences Stop against active client calls and schedules a full
  same-generation snapshot after conflicts or committed cache gaps; the retry
  worker remains the sole owner of reconnect/resnapshot completion.
- `CEditorControlPlatformRuntime::ExecuteProfile()` is the only editor-side
  Profile RPC facade. It is Ready-only, participates in active-call draining,
  opens one fresh authenticated Hello channel, pins the frozen generation, and
  accepts exactly one matching terminal response. Profile response data remains
  separate from the storage cache; a conflict or committed generation gap
  schedules the existing resnapshot worker without discarding the typed Profile
  result.
- Profile mutation replay is explicit: only ambiguous transport loss after
  dispatch returns `RetryWithSameOperationId`. Stop/resnapshot closes the active
  channel, one-in-flight arbitration covers storage and Profile operations, and
  diagnostics omit registry documents, paths, display names, and profile IDs.
- `CEditorControlPlatformRuntime::StorageCacheCoordinates()` is the read-only
  CAS handoff for editor persistence. It exposes `{profileId, generation,
  globalStorageRevision}` only in `Ready`; individual entry revisions, endpoint
  discovery, transport construction, retries, and local writer creation are
  intentionally excluded. `WaitForStorageCacheReady()` waits only for work
  already scheduled by `Start()` or `RequestResnapshot()`, uses a capped
  monotonic deadline plus caller cancellation, and is woken by every lifecycle
  transition and `Stop()`. Neither API may bootstrap, resnapshot, or mutate
  state.
- The remaining P0 IPC gates are production consumer composition, remote
  revisioned change subscription, control restart, and broader durable-failure
  integration tests; the connected production ownership path itself is now
  composed in both roles.

The focused control client/editor runtime suite passes 35/35, and production
startup now consumes Profile `Snapshot` only through this facade. Native
profile mutation commands/UI and remote profile-change subscriptions remain
uncomposed.

## Phase 1 Layout-Memento Persistence Checkpoint (2026-07-31)

The layout persistence composition adapter uses the existing ready-only storage
writer path. It carries the Machine-target `workbench.layout` mutation's captured
global storage revision and one durable operation ID. If transport loss makes an
Apply result ambiguous, it may issue exactly one replay with the same ID; it may
not mint a replacement ID or retry a conflict. A conflict remains observable to
the caller and the remote value remains authoritative. This IPC rule supports one
orderly shutdown save only; it is not a permit for per-layout-event writes.

Restore callers distinguish loaded, missing, invalid (including corrupt or
unsupported memento), unavailable, and failed storage outcomes. IPC/storage code
does not convert invalid durable bytes into a default write. The production
workbench adapter now consumes this ready-only facade, with focused persistence
and runtime tests passing.

## Phase 4 Secret Vault IPC Checkpoint (2026-07-31)

- `ControlSecretVaultRpc` adds bounded, versioned request/response codecs for
  capability issue/revocation, Get, Apply, and legacy migration. The server uses
  its OS-observed pipe peer PID and active host-session grant; payload-supplied
  profile, generation, extension namespace, operation ID, CAS revision, and
  capability are all untrusted inputs.
- `CEditorSecretVaultClient` uses a fresh authenticated channel for each
  operation, independently confirms endpoint/Hello identity, and preserves the
  exact operation ID only for the explicit ambiguous-Apply replay outcome.
  Conflict, retry, stale generation/session, revocation, transport loss, and
  shutdown remain distinct terminal results.
- Capability/session revocation precedes host lease release and control-runtime
  shutdown. A rejected handshake, migration failure, inventory mismatch, host
  loss, editor loss, or partial startup failure must leave no active grant.
- Secret values and capability bytes never enter endpoint metadata, storage
  snapshots, diagnostics, normal logs, or change events. `SecretStorage.keys()`
  is rejected in the host API and is not part of this wire protocol.
