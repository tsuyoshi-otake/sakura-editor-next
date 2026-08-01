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

## Extensions ViewContainer Composition (2026-08-01)

- `CExtensionPane` is no longer a standalone dock. It is the content of the
  built-in `workbench.view.extensions` ViewContainer:
  `workbench::viewcontainer::CViewContainerPages` owns the instance, and the
  profile-scoped `MarketplaceFactory` supplied by the composition root is what
  constructs and destroys it. The pane must not construct or place itself.
- `CExtensionViewRegistry`-backed contributed tree views remain a separate
  section of that same ViewContainer, never the container's identity. An
  extension that contributes nothing leaves the Marketplace as the container's
  entire content; a contribution adds a section beside it and never replaces
  it.
- See [`../workbench/CLAUDE.md`](../workbench/CLAUDE.md) for the pool/host
  ownership, one-location, and typed-absence contracts this composition must
  satisfy, and [`../window/CLAUDE.md`](../window/CLAUDE.md) for the
  profile-scoped factory wiring and the retired legacy floating dock that used
  to hold this pane.

## Activation and Installed-Set Changes (2026-08-01)

- There is no per-extension execution confirmation. VS Code gates code execution
  with Workspace Trust — a per-workspace restricted mode — and has no "may this
  extension run?" dialog anywhere in its model. The former `CExtensionTrustStore`,
  its `workbench/extensions/ensureTrusted` round trip, `MYWM_EXTENSION_TRUST_PROMPT`,
  and the TaskDialog they drove are deleted. `WM_APP+234` is now an intentional gap;
  do not renumber the neighbouring prompt messages to close it, and do not
  reintroduce an activation-time permission prompt under any name.
- `workbench/extensions/didFailActivation` must land somewhere observable. Real
  VS Code's `AbstractExtensionService` shows a notification only under an
  extension development host and otherwise writes to the "Extension Host" log
  channel; this product does the same and never shows a modal. The dispatcher
  parses that notification leniently, bounds the extension-supplied message, and
  always acknowledges the RPC — a malformed or unrecordable failure report must
  never make the host believe this method failed.
- The "Extension Host" log channel is host-owned, not extension-owned. It uses a
  fixed `OutputOwner` that can never equal an extension ID and is deliberately
  absent from the bridge's tracked-owner set, so extension reload, disable,
  uninstall, and full host loss cannot dispose it. It has its own non-wrapping
  operation ID sequence, and a rejected create leaves the created flag unset so a
  later append retries instead of caching a permanent failure.
- Installing an extension takes effect in the running window. `CExtensionPane`
  reports install completion through a callback owned by the composition root;
  the pane never reaches into `CExtensionService`. The service re-enumerates on
  its worker thread, appends only roots this session has not seen, and is the
  first-connection trigger for a profile whose installed set was previously
  empty. Re-registration sends the accumulated set because the host registers an
  extension ID at most once and activates only records still in `registered`
  state; correctness here depends on that host-side idempotence, not on the
  native side tracking which roots were already sent.

## P2 Contributions

Commands, menus, keybindings, configuration, views, diagnostics, output,
language features, and enablement are registered with an owner and disposed as
one transaction on disable/update/uninstall. Document additions precede editor
deltas; active editor may be absent. Unsupported APIs report capability state
instead of returning a false success.

A `StatusBarItem` contribution carries its tooltip as Markdown **source** plus
the separate `tooltipSupportsThemeIcons` boolean, and
`CExtensionWorkbenchDispatcher` puts both on `SExtensionStatusBarItem` unchanged.
The dispatcher must not render, flatten, or strip the Markdown: the flag decides
whether `$(name)` is a codicon or literal text, and the decision belongs to the
renderer that actually draws it
([`../workbench/hover/CLAUDE.md`](../workbench/hover/CLAUDE.md)), not to the
transport. An absent flag means `false`, matching a plain-string tooltip.

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

## workspace/configuration/update Delivery (2026-08-01)

- `CExtensionWorkbenchDispatcher::DispatchConfigurationUpdate` (method
  `workspace/configuration/update`) is the native handler behind
  `vscode.workspace.getConfiguration(...).update(...)`. Parameters: `key`
  (required, non-empty, bounded by `CJsoncDocument::kMaximumObjectKeyLength`,
  and must already satisfy the canonical-ASCII key rule the document editor
  enforces), `value` (optional; a genuinely absent field removes the key,
  matching real VS Code's `value === undefined` deletion semantics -- a JSON
  `null` is a *value*, not an absence, and is written as `ConfigurationValue(nullptr)`),
  `configurationTarget` (optional; accepts the real `ConfigurationTarget` enum
  encoding `Global=1/Workspace=2/WorkspaceFolder=3` or a boolean, `true` =>
  Global, `false` => Workspace), and `overrideInLanguage` (optional string;
  when non-empty the write targets the `[languageId]` override block of the
  same physical document instead of its base scope). Success returns
  `resultJson = "{}"` with `changes = EExtensionWorkbenchChange::None` (no bit
  in that enum represents a configuration change). Failure is always a typed,
  ack'd RPC error, never a silently-accepted no-op: `-32602` for a malformed
  request (bad JSON, wrong-typed/oversized `key`, a `configurationTarget` that
  is not a ConfigurationTarget value or boolean, or a `value` that fails the
  bounded JSON-to-`ConfigurationValue` conversion below), `-32601` with an
  `"UnsupportedCapability: ..."`-prefixed message for a well-formed but
  unsupported target, and `-32001` ("workbench settings owner is not
  available") when no `CWorkbenchRuntime` is bound to the bridge or it is not
  currently `Ready`/`ReadyWithDiagnostics`.
- **Divergence: only `ConfigurationTarget.Global` is implemented.** Real VS
  Code additionally resolves an absent target to Workspace or WorkspaceFolder
  settings depending on whether the call is resource-scoped, and it throws
  rather than silently succeeding when a target cannot be honored. This
  bridge has no accessor to the runtime's dynamically-assigned
  workspace/folder settings documents (`CWorkbenchRuntime` tracks those
  through internal, session-scoped state the bridge does not borrow), so
  every non-Global outcome -- `Workspace` (2), `WorkspaceFolder` (3), the
  boolean `false` form, and *both* branches of the absent-target default,
  since neither ever resolves to Global -- is rejected up front as an
  explicit typed `UnsupportedCapability` failure. This is unconditional: it
  does not first check whether a workspace happens to be open, so "no
  workspace is open" is necessarily covered by the same rejection rather than
  needing a separate code path. Extending this to Workspace/WorkspaceFolder
  requires a new, narrow accessor on `CWorkbenchRuntime` (or an equivalent
  service) for its active workspace settings document identity; it is not a
  dispatcher- or bridge-local change.
- **Bounded, untrusted JSON-to-`ConfigurationValue` conversion.** The
  dispatcher's `ToConfigurationValue` helper reuses
  `CJsoncDocument::kMaximumDepth` (64), a 65536-node budget, and a
  1 MiB string-length budget as its own conversion limits, so an extension
  can never construct a `ConfigurationValue` the document editor would have
  rejected anyway. This is enforced entirely in the dispatcher, before
  `CExtensionWorkbenchServiceBridge::WriteGlobalConfiguration` is ever called,
  so a rejected value never reaches the file-write path.
- **Fixed, dispatcher-owned error text, never the coordinator's raw
  diagnostic.** `ConfigurationWriteFailure` maps every
  `config::ESettingsWritebackStatus` that is not `Succeeded()` to one of a
  small set of fixed English messages instead of forwarding
  `SettingsWritebackResult::diagnostic` verbatim across the RPC boundary.
  The coordinator's diagnostic is already documented as category-only, but
  this is deliberate extra margin: the dispatcher's own message set is the
  single place that must be audited for accidental leakage of a path, URI,
  profile ID, setting key, setting value, credential, or hash, independent of
  whatever the coordinator's contract does or does not guarantee today.
- **Resolved finding on descriptor gating (this delivery's write path does
  not need dynamic descriptor registration).** `CConfigurationService::Update()`,
  `Inspect()`, and `ReadSnapshot()` all reject a key that has no registered
  `ConfigurationDescriptor`. This delivery's write path never calls any of
  those three methods: `CWorkbenchRuntime::WriteSetting` goes through
  `CSettingsWritebackCoordinator::Write` -> `CJsoncConfigurationEditor::Edit`
  (a pure JSONC document edit, key-shape-checked only, no descriptor lookup)
  -> a file-source `Reload`, which lands in `CConfigurationService` through
  `ReplaceSources`/`ReplaceSource`. Those two methods store an undescribed
  key as intentionally "latent" rather than rejecting it. Net effect: an
  extension can durably `update()` a key with no built-in descriptor (for
  example `odangoo.otak-usage`'s own settings) and the write survives to disk
  and reload correctly, but that value remains invisible to any native code
  path that reads configuration through `Update()`/`Inspect()`/`ReadSnapshot()`
  until a descriptor for that key is registered. This is a pre-existing
  property of those three read/update methods, not something this delivery
  introduced or needs to fix; it only means "successfully written" and
  "visible to every native configuration reader" are not yet the same
  guarantee for undescribed keys.
- **The production wiring is closed, and it borrows the interface, not the
  concrete runtime.** An earlier draft of this checkpoint recorded the window
  composition root as an open gap: `CEditWnd::InitializeWorkbench` constructed
  `CExtensionService` with only seven arguments, so the eighth
  `workbenchRuntime` parameter took its `nullptr` default and every production
  `workspace/configuration/update` failed closed with `-32001` no matter what
  the dispatcher did. `sakura_core/window/CEditWnd.cpp` now passes
  `m_workbenchRuntime` explicitly. That member is a
  `workbench::IWorkbenchRuntime*`, and `WriteSetting` was only on the concrete
  `CWorkbenchRuntime`, so closing the gap meant choosing which way the
  dependency points. It was promoted to a pure virtual on `IWorkbenchRuntime`
  and the bridge's and `CExtensionService`'s parameters/members were retyped
  from `workbench::CWorkbenchRuntime*` to `workbench::IWorkbenchRuntime*`; the
  extension layer now depends on the stable workbench boundary rather than on
  the runtime implementation, and `CEditWnd` does not need a second, concrete
  handle to an object it already borrows. `CWorkbenchRuntime` remains the sole
  implementer, so this adds no new production authority. Existing callers that
  hold a `CWorkbenchRuntime*` (the dispatcher's integration fixture, for
  example) still compile through the implicit derived-to-base conversion.
- **Nullable is still a real state, and it still fails closed.** A window
  constructed without a runtime (the historical no-runtime unit path) passes
  `nullptr`, `CExtensionService` then builds no bridge at all unless a Marker or
  Output service is present, and `WriteGlobalConfiguration` returns `Stopped`
  when the pointer is null. `-32001` therefore remains the correct, reachable
  outcome; it is no longer the *only* outcome.
- **Informational, out of scope: `session.configuration` is never populated.**
  Independent of the above, `src/exthost/extension-host.cjs`'s session object
  and `CExtensionService::SendRegisterExtensionsWorker()` do not appear to
  populate a `configuration` snapshot into the extension-host session at
  connect time, which real VS Code extensions can read synchronously via
  `vscode.workspace.getConfiguration(...).get(...)` before ever calling
  `update()`. This is a pre-existing, separate gap from the `update()` path
  this checkpoint delivers, and was not modified here.
