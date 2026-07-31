# P0/P2 Extension Integration Guidance

## Existing Boundary

Retain the control-owned broker, editor-to-host named-pipe connection, bounded
JSON-RPC framing, generation checks, cancellation, and version-gap resync.
`CExtensionService` adapts the shared Node host to stable platform/workbench
services. Production OpenVSX uses the shared Request/Proxy graph, and
SecretStorage uses only the control-owned Secret Vault adapter; the historical
per-editor DPAPI class is a migration/test adapter, not a second authority.

## P0 Session Contract

Every logical extension-host session carries stable `profileId`, `windowId`,
`workspaceId`, `extensionHostSessionId`, protocol version, capability revision,
connection generation, and cancellation ownership. Reconnect performs
snapshot-before-subscription; stale-generation responses/events are discarded.
Physical host sharing never implies shared logical window/workspace state.

OpenVSX uses the shared request/proxy/credential service. SecretStorage uses the
profile/extension-namespaced secret service. Neither opens durable state files
directly. Keep retries bounded, deduplicate in-flight operations, honor
`Retry-After`, and produce one terminal result on every branch.

## Phase 4 Production Composition Checkpoint (2026-07-31)

- The Extensions pane and installer now consume one typed
  `IOpenVsxRegistryClient`; they no longer construct the legacy direct HTTP
  client. The production factory takes one coherent profile network-policy
  snapshot and returns a self-contained Request/Proxy/WinHTTP graph suitable
  for detached job ownership.
- Search, VSIX retrieval, and optional SHA-256 retrieval share the same request
  contract, cancellation, timeout, proxy policy, response limits, and typed
  terminal outcomes. UI diagnostics redact URLs, profile IDs, proxy values,
  credentials, response bodies, and hash values.
- `CExtensionSecretVaultStorage` is the production editor adapter. It binds only
  after an authenticated host Hello, acquires short-lived capabilities through
  narrow control IPC, preserves mutation operation IDs across the one permitted
  ambiguous replay, clears the session before releasing the host lease, and
  never exposes secret values in diagnostics or events.
- The control process owns one DPAPI-backed Vault, legacy migration,
  installed-extension eligibility, host-session grants, editor-PID leases, and
  reverse-order shutdown. The historical `CExtensionSecretStorage` remains
  available only for migration compatibility and isolated legacy tests.
- Editor lease acquisition is admitted only after the control tray validates a
  live registered Sakura editor HWND and its OS-reported PID. The controller
  pins that process object with a `SYNCHRONIZE` handle for the complete nested
  lease lifetime; owner count and per-owner nesting are bounded. Periodic health
  checks use fixed-capacity bookkeeping, and rollback, final release, and
  shutdown operate on the pinned object so PID reuse cannot transfer an old
  grant or turn a `noexcept` health path into an allocation failure.
- The current OpenVSX production factory still deliberately has no credential
  provider. The existence of SecretStorage does not imply registry/proxy
  authentication; challenge-scoped credential lookup remains unsupported.
- All installed extensions in one shared Node host are mutually trusted code,
  matching the VS Code extension-host model. `(extensionId, key)` is a logical
  API namespace and installed-ID eligibility check, not a hostile
  extension-to-extension security boundary. Strong isolation requires a
  dedicated authenticated process principal per extension.

See [`openvsx/CLAUDE.md`](openvsx/CLAUDE.md) for registry-client invariants and
[`../platform/request/CLAUDE.md`](../platform/request/CLAUDE.md) for the shared
request lifetime and proxy contract.

## P2 Contributions

Commands, menus, keybindings, configuration, views, diagnostics, output,
language features, and enablement are registered with an owner and disposed as
one transaction on disable/update/uninstall. Document additions precede editor
deltas; active editor may be absent. Unsupported APIs report capability state
instead of returning a false success.

## Phase 6 Workbench Service Bridge Checkpoint (2026-07-31)

- `CExtensionWorkbenchServiceBridge` borrows runtime-owned Marker/Output
  services; it does not own or stop them. Accepted RPC mutates the authoritative
  service first, then mirrors legacy diagnostics/output caches only as a
  best-effort compatibility projection.
- Diagnostics preserve canonical URI, complete half-open range, owner ID,
  collection ID, and connection generation. Empty set and missing/repeated
  delete/collection-clear are idempotent desired-absence operations and must
  still clear the matching legacy decoration cache.
- Every Output mutation requires a valid host-supplied operation ID when the
  service bridge is present. A reconnect resend keeps the same ID; generating a
  replacement ID after an ambiguous result would permit duplicate output.
- Host loss, reload, disable, and `ClearWorkbench` dispose only the exact
  owner-generation in both services before clearing legacy caches. A newer
  generation cannot be removed by an older callback.
- The bridge remains nullable for isolated legacy tests. Production
  `CExtensionService` receives runtime Marker/Output borrows after SecretStorage
  construction, and clears the bridge while the runtime is still alive.
