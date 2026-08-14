# P0 Platform Foundation Guidance

## Scope

This directory owns the stable, UI-independent contracts introduced by Issue
#6: service registration, lifecycle, URI identity, file-system providers,
profile descriptors, storage, request/proxy, and credentials.

## Dependency Boundary

- Keep foundation contracts independent of HWND, `CEditWnd`, `CEditDoc`,
  `CShareData`, and profile INI implementation details.
- Dependencies point from workbench and process composition toward these
  contracts, then from contracts toward replaceable platform adapters.
- A service ID is stable and unique. Registration is explicit, duplicate
  registration fails, and required dependencies are validated before startup.
- Lifecycle startup follows dependency order. Shutdown and failed-startup
  rollback run exactly once in reverse order and return an explicit terminal
  outcome.

## Identity and Persistence

- Resource identity is URI-based. Raw pointers, HWND values, display names, and
  un-normalized paths are not public identities.
- A profile has an immutable `profileId`; its display name and legacy `-PROF`
  alias may change. Never derive a durable path from the display name.
- The control process is the only durable profile/storage writer. Editor
  processes use revisioned commands and snapshots; `CShareData` is not a new
  database.
- Every state key declares scope, owner namespace, target, and schema version.
  Structured state uses a single owner or revision/CAS; implicit lost-update
  last-writer-wins is forbidden.
- Request contracts carry an explicit proxy-support policy, bounded timeout,
  redirect, response-header, and response-body limits. `Off` performs no proxy
  lookup; other proxy modes are resolved by the injected proxy service and can
  terminate as unsupported. Transport implementations must enforce limits while
  streaming and must not offer TLS-validation bypasses. Proxy/PAC resolution,
  credential lookup, transport, and retry waits all share one monotonic deadline
  and caller cancellation; no adapter may extend the overall request budget.
- Credentials are retrieved only after a typed `401` or `407` challenge and are
  scoped to the challenged server/proxy. They are one-shot transport inputs,
  never settings or caller-owned HTTP headers. Every timeout, limit,
  authentication, proxy-policy, cancellation, and transport branch returns a
  distinct terminal `RequestResult`.

## P0 Gate

- Unit-test service cycles/duplicates, startup rollback, URI equivalence,
  profile migration idempotence, revision conflicts, replay deduplication, and
  snapshot resynchronization.
- Failure, cancellation, conflict, timeout, and unsupported paths must all have
  one observable terminal result.
- Keep all tests deterministic and free of UI and live network access.

## P0 Implementation Checkpoint (2026-07-31)

- Implemented and unit-tested: typed service registration and dependency-ordered
  lifecycle; URI identity; file-service/provider routing; immutable profile
  descriptors and workspace association; revisioned storage with CAS, bounded
  replay, and snapshot resynchronization; layered configuration; bounded request
  policy; control-IPC framing/RPC/named-pipe transport/current-user security; and
  the endpoint ABI.
- `ProfileAuthorityStore` durably anchors one opaque immutable `profileId` to a
  legacy profile directory and advances a control-owner authority generation.
  `CAtomicFileStorageService` provides a control-owner-only, versioned,
  checksummed, atomic-file backend with an exclusive writer lock, revision/CAS,
  and a bounded persisted operation replay ledger. Both expose corrupt,
  unsupported, busy, and I/O outcomes explicitly; no caller may silently mint a
  replacement identity or substitute an empty store after durable state fails.
- Endpoint ABI v2 publishes the canonical `profileId` together with generation,
  lifecycle, pipe identity, and control PID. `CControlPlatformClient` implements
  the pure editor-side, bounded and deduplicated discovery -> connect -> Hello ->
  full-snapshot/resnapshot state machine and fences stale attempts from making a
  cache ready. These are foundations, not proof of production availability.
- `CControlPlatformRuntime` now composes the durable authority, atomic storage,
  and service host with strict startup and reverse-shutdown ownership.
  `CControlProcess` owns this runtime, reaches it before publishing the legacy
  control-ready handoff, and stops it before releasing control ownership.
- `CEditorControlPlatformRuntime` owns one editor-side discovery reader, client,
  synchronized cache, and retry worker. `CNormalProcess` starts it only for a
  real editor, before plugins/workbench consumers, and stops it after those
  consumers are destroyed. Forward-only processes do not open a redundant
  session. Production requires Hello and a full nonzero-generation snapshot;
  transient or hard failures never expose an empty cache as defaults.
- P0 production composition is wired through both process roles. Remaining P0
  gates are cross-process conflict/resnapshot/restart coverage and durable
  failure smoke beyond the connected startup/reverse-shutdown path.
- Durable legacy-profile identity anchoring is implemented. Full profile
  registry/import/export/switch workflows and a challenge-scoped credential
  provider remain later slices. Do not bypass authoritative service contracts
  with direct INI, registry, legacy HTTP, or per-editor durable writes.
