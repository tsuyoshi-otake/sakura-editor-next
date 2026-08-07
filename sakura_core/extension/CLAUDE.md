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

## Profile-Scoped Extension Enablement (2026-08-01)

- The installed extension payload remains in the control/profile-owned shared
  `extensions` directory, while the selected user-data profile owns a sparse
  enablement map at its `extensions.json` resource. This matches VS Code's
  important behavior: installing a package and selecting it for a profile are
  separate operations, and disabling one profile must not uninstall the shared
  package or change another profile.
- The active profile's enabled IDs are the exact extension-host registration
  set. Installing, enabling, disabling, uninstalling, or switching profile
  selection cancels the old session and rebuilds the host registration set;
  an empty set is terminal and must release the host lease rather than enter a
  reconnect loop. Corrupt or unreadable selection state fails closed.
- A missing selection file preserves the legacy default profile's installed
  extensions for backward compatibility. Named and transient profiles treat a
  missing file as empty. When a package is installed from a named profile, the
  default profile receives an explicit disabled entry so that the package does
  not leak into the default profile merely because the shared payload exists.
- **Documented divergence:** Sakura does not copy VS Code's private
  extension-management metadata format. Its existing profile bootstrap already
  owns the `extensions.json` resource and `CExtensionManager` owns global
  package transport, so Sakura stores only the bounded profile-selection
  projection needed by the native host. The observable profile/install,
  enable/disable, host-lifecycle, and fail-closed semantics are the compatibility
  contract; importing VS Code's internal metadata would create a second package
  authority.
- Icon-font contributions are rebuilt from the same active-profile selection,
  so a disabled or removed extension cannot leave stale status-bar icons in a
  running window.

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

## Contributed ViewContainer location and `when` (2026-08-07, #29)

- `EExtensionViewContainerLocation` has three values —  `ActivityBar`, `Panel`,
  `SecondarySidebar` — matching the exactly three keys VS Code's
  `contributes.viewsContainers` accepts. `CExtensionWorkbenchDispatcher`'s
  `ParseViewDeclarations` resolves them explicitly and **rejects** anything else
  rather than defaulting to `activitybar`; a container declared at a location
  that does not exist must not appear at a location the extension never named.
  `CExtensionWorkbenchServiceBridge` maps `SecondarySidebar` onto
  `workbench::layout::EViewContainerLocation::AuxiliaryBar`.
- `SExtensionViewPresentation::whenClause` now carries container clauses as well
  as View clauses. `contextualTitle` remains View-only. The clause is kept, not
  acted on, here: registration stays complete and the *projection* decides what
  renders, so a later context-key change can reveal a container without a
  re-register. See [`../window/CLAUDE.md`](../window/CLAUDE.md) for the
  projection-side contract.
- `DispatchContextSet` reports `Commands | ContextKeys`.
  `EExtensionWorkbenchChange::ContextKeys` is a distinct bit precisely so a
  `setContext` refreshes `when`-gated projections without every command
  registration rebuilding them.

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

## Extensions View Search Filtering, Row Icons, Context Menu, and Paging (2026-08-07)

`CExtensionPane` now wires `ParseExtensionSearchQuery` and
`ApplyExtensionSearchFilters` into the search box, renders each row's icon
through `ExtensionIconDecoder`, adds a per-row right-click context menu, and
supports `IOpenVsxRegistryClient::Search`'s existing offset-based paging as a
"Load More" button. All four pieces stay inside the pure/impure boundary: the
new pure helpers (`ExtensionSearchQuery`, `ExtensionSearchFilter`,
`ExtensionIconDecoder`) remain free of HWND/network/filesystem, and only
`CExtensionPane` (the adapter) performs Win32 calls or triggers jobs.

- **Search syntax and its fail-closed boundary.** `StartSearch()` parses the
  search box through `ParseExtensionSearchQuery` and rejects (status text, no
  list change) rather than silently treating as free text: any unrecognized
  `@`-token (`parsed.unknownTokens`), and `@recommended` specifically, because
  no recommendation data source exists anywhere in this application. Supported
  tokens are exactly what `ExtensionSearchQuery.h` models: `@installed`,
  `@enabled`, `@disabled`, `@outdated`, `@deprecated`, and `@sort:installs`/
  `@sort:rating`/`@sort:name`. `@installed`/`@enabled`/`@disabled`/`@outdated`
  are always answered from local installed-set state (`ShowFilteredInstalledList`)
  and never reach the marketplace, even combined with free text -- this matches
  VS Code, which also answers those four from local extension state rather than
  a server query. Free text (with or without `@sort:`/`@deprecated`) becomes a
  marketplace `Search` job; `@deprecated`/`@sort:` are then re-applied locally
  to the fetched page (`ApplySearchResultRefinement`) with `searchText` cleared,
  because the server already narrowed by the free text and re-running it as a
  local substring filter could drop server-relevant matches this local check
  does not attempt to reproduce. `@outdated` reports "no known outdated
  extensions" rather than fabricating one when only an installed-only
  enumeration is available (`installedVersion == sVersion` by construction);
  see `ExtensionSearchFilter.h`'s own documented scope for why.
- **Row icons: bytes only ever arrive from the composition root.** This
  subtree performs no network access anywhere; `SetExtensionIconBytes(id,
  bytes)` is the sole entry point for icon pixels, and it is the composition
  root's job to fetch `SOpenVsxExtension::sIconUrl` (when present) and hand the
  raw encoded bytes in. `SetExtensionIconBytes` decodes through
  `DecodeExtensionIconBitmap` (same WIC/`CreateDIBSection`/premultiplied-BGRA
  pattern as `CExtensionDetailSurface`'s icon path) and caches the resulting
  `HBITMAP` per extension unique ID; `OnDrawItem`/`DrawRowIcon` paint it with
  `AlphaBlend`. **Must be called on the UI thread**: like
  `CExtensionDetailSurface`, decoding requires an already-initialized COM
  apartment on the calling thread, and `DecodeExtensionIconBitmap` is
  `noexcept` and fails closed (indistinguishable from bad input) without one.
  Unavailable bytes (never supplied, empty, or failed to decode) always fall
  back to VS Code's real fallback shape -- an accent-colored initials tile (up
  to two initials, skipping space/hyphen/underscore, or "?" when the name is
  empty), the same pattern `CExtensionDetailSurface::PaintHeader` already
  established for its hero image -- never a fake or placeholder image.
  `ReleaseIconBitmaps()` frees every cached bitmap and is called from both
  `OnDestroy` and the destructor, matching this class's existing dual-teardown
  pattern for jobs/timers.
- **Context menu: only genuinely performable items, and it is mouse-only.**
  `ShowRowContextMenu` (invoked from `NM_RCLICK` in `OnNotify`) offers exactly:
  "Enable"/"Disable" + "Uninstall" when the row is installed, or "Install" when
  it is not installed and has a download URL. If neither branch adds an item
  (for example, a not-installed row with no download URL), no menu is shown at
  all -- an empty popup would look broken, so absence is explicit rather than
  faked. **There is no "Update" item anywhere, ever**, because no working
  reinstall-over-an-installed-extension capability exists in this application:
  `CExtensionManager::Install` fails closed whenever the destination directory
  already exists, and `CExtensionPane::StartInstall()`'s own guard already
  refuses to run when a row is installed. The same reasoning is why
  `OnDrawItem`'s row affordance label only ever says "Install", using the exact
  condition `sInstalledVersion.empty() && !sDownloadUrl.empty()` that
  `StartInstall()` itself checks.
  **Divergence: the context menu is mouse-only (`NM_RCLICK`), with no keyboard
  equivalent (Menu key / Shift+F10).** `CWnd::DispatchEvent` (in
  `sakura_core/window/CWnd.cpp`, outside this pane's editable scope) handles a
  fixed message set that does not include `WM_CONTEXTMENU`, so a keyboard
  invocation falls through to `DefWindowProc` and never reaches this pane.
  Closing this gap requires adding `WM_CONTEXTMENU` handling to `CWnd` itself,
  which is a `window/`-owned change, not an `extension/`-owned one.
- **Paging: a dedicated "Load More" button, not an inline row.**
  `IOpenVsxRegistryClient::Search` already accepts an offset, so `SJob` gained
  `nOffset`/`bAppendResults` fields and `StartLoadMoreSearch()` requests the
  next page at `m_searchRawRows.size()`, appending into the same raw-result
  buffer that `ApplySearchResultRefinement` re-filters/re-sorts on receipt.
  **Divergence: paging is a dedicated button in a fixed, always-reserved layout
  slot** (hidden, not removed or reflowed, when there is nothing more to load
  or no search result is showing) **rather than an inline "$(more) Show More"
  row appended to the list**, which is how real VS Code's Extensions view
  presents continuation. A future native owner-drawn "more" row inside the
  `ListView` itself could close this gap; the current shape was chosen to avoid
  adding list-item-kind branching (row vs. sentinel) to `OnDrawItem`/
  `OnMeasureItem` in this delivery.
- **Publisher line uses the raw namespace.** `SOpenVsxExtension` has no
  separate "publisher display name" field (only `sNamespace`, the segment also
  used to build the unique ID), so the row's publisher line shows `sNamespace`
  as-is. This differs from VS Code's marketplace publisher display name, which
  can differ from the raw publisher/namespace segment.

## Workspace Trust Gates the Installed Set (2026-08-07)

- Trust gating lives entirely in `CExtensionService::LoadInstalledExtensionRootsWorker`,
  the same enablement-layer predicate that already filters on
  `CExtensionProfileState::IsEnabled`. While the workspace is not trusted, an installed
  extension whose `SInstalledExtension::untrustedWorkspaceSupport` (`CExtensionManager.h`)
  is `EExtensionUntrustedWorkspaceSupport::NotSupported` is excluded from the roots that
  ever reach `host/registerExtensions` -- it is never sent to the shared Node host at all,
  not sent-then-deactivated. This mirrors upstream VS Code's own layering exactly:
  `ExtensionEnablementService` / `DisabledByTrustRequirement` decide *whether an extension
  is eligible to activate* before activation is ever attempted; trust is not a check inside
  the activation path, and this codebase now matches that shape rather than inventing a
  host-side or activation-time trust check. `EExtensionUntrustedWorkspaceSupport::Supported`
  and `::Limited` both mean "may load"; only `::NotSupported` is withheld.
- Trust itself is read through `CExtensionWorkbenchServiceBridge::WorkspaceContextSnapshotForExtensions()`
  -- the identical accessor `SendRegisterExtensionsWorker` already used to populate
  `workspaceTrusted` on registration -- via the new `CurrentWorkspaceTrustedWorker()` helper.
  Nothing under `extension/` re-resolves trust or second-guesses it:
  `CWorkbenchRuntime::ResolveAndApplyWorkspaceTrust` remains the sole authority (see
  `config/CLAUDE.md`'s Workspace Trust Resolution Checkpoint), and the bridge snapshot is
  strictly a read-only projection of that decision. A null `m_workbenchServiceBridge` -- no
  Marker/Output/runtime was wired at construction, the historical no-runtime unit-test shape
  -- and any failure to read a candidate's manifest both resolve to "not trusted" /
  "not supported": every unreadable or absent signal here fails closed, never open.
- A trust transition restarts the extension host session; there is no in-place
  deactivation. `CExtensionService::SetWorkspaceTrusted` now tracks the resolved value on
  two separate members for two separate reasons: `m_sentWorkspaceTrusted` gates the
  existing `extension/workspace/didChangeTrust` RPC and only ever advances while
  `m_registered`, because sending that notification before any host session exists to
  receive it is meaningless; `m_filterWorkspaceTrusted` gates whether the installed-set
  filter needs to rerun and advances unconditionally, because an unregistered service still
  decides what the *next* registration is allowed to contain. Whenever the resolved value
  actually changes, `SetWorkspaceTrusted` calls the existing `RescanInstalledExtensionsWorker()`
  -- the same worker method installing an extension already reruns -- which recomputes the
  roots, compares them to the previous set, and if they differ calls `FailConnectionWorker`
  to tear down the session and then reconnects. This is deliberately upstream's own
  behaviour (VS Code re-derives the whole activation set on a trust change rather than
  deactivating one extension at a time), so no new in-place deactivation path was written.
  A grant re-admits previously withheld roots on the resulting reconnect; a downgrade
  removes them the same way. `RescanInstalledExtensionsWorker`'s existing
  `m_installedRoots.empty()` branch -- release the host lease instead of looping a
  reconnect -- already covers the legitimate case where every installed extension is
  `NotSupported` and an untrusted window's gated set is empty.
  `m_filterWorkspaceTrusted` is seeded explicitly by `WorkerInitialize()` (via the same
  `CurrentWorkspaceTrustedWorker()` helper) rather than left to wait for a change
  notification, because `Start()` installs the `WorkspaceContext().Subscribe(...)` callback
  only after `WorkerInitialize` is already queued on the worker thread; without an explicit
  seed, a window that starts untrusted would have nothing to compare its first real
  notification against. `LoadInstalledExtensionRootsWorker` itself does not depend on this
  seed for correctness -- it always reads live trust off the bridge on every call -- the
  seed only makes the *first* `SetWorkspaceTrusted` delivery correctly recognized as
  changed-or-unchanged relative to what was already in effect at startup.
- Silence is a defect: an extension withheld by trust must be reported, not simply dropped.
  `LoadInstalledExtensionRootsWorker` calls `ReportWithheldExtensionWorker` for every
  withheld ID, which records one `Warning` through
  `CExtensionWorkbenchServiceBridge::AppendExtensionHostLog` — the runtime-owned
  `workbench::output::OutputService` Extension Host log channel — carrying
  `DispatchUnsupportedCapability`'s own fixed-English `"UnsupportedCapability: ..."` message
  discipline naming the extension's unique ID.
- **It deliberately does not write the `m_output` "Extension Compatibility" channel that
  `DispatchUnsupportedCapability` uses, and that is a correction, not a shortcut.** `m_output`
  (`CExtensionOutputChannel`) is the *fallback* cache the bridge falls back to only when a
  window has no runtime-owned `OutputService`; in a production window every real Output
  mutation routes to the service instead. Nothing projects the fallback cache to any
  user-visible surface: `CExtensionService::OutputChannels()` has no production caller, and
  `CEditWnd`'s `MYWM_EXTENSION_WORKBENCH_CHANGED` handler branches on `StatusBar`, `Views`,
  `Contributions`, `Diagnostics`, `Progress`, and `Notifications` but has no `Output` branch
  at all. Writing there would satisfy the "never silent" rule on paper while the user saw
  nothing — precisely the faked-capability failure the root `CLAUDE.md` forbids.
  `DispatchUnsupportedCapability` has this same defect today; it is pre-existing, is not
  fixed here, and must not be copied into new code.
- The Extension Host log is also the *categorically* right channel, not merely the visible
  one. A withheld extension never ran, so there is no capability it requested and was refused,
  no host-assigned generation to build a `compatibility:<id>:<generation>` handle from, and no
  RPC operation ID to route a channel mutation with — the three things every
  `CreateOutput`/`AppendOutput` bridge call requires. This is a host decision taken before the
  extension existed, which is where upstream records a trust-disabled extension too.
  `AppendExtensionHostLog` owns its own bounded operation IDs and its own host-owned channel,
  and it never reveals the Output panel: a Restricted Mode window must not pop a panel open,
  because real VS Code does not.
- Reporting is deduplicated by `m_reportedWithheldExtensions` (worker-thread-only, like
  `m_installedRoots`): an ID already reported for the current withheld state is skipped on
  every subsequent rescan that recomputes the identical withheld set, so a trust-unrelated
  rescan (for example, installing an unrelated extension) does not re-spam the log. An ID is
  inserted into that set **only after the append actually succeeded**, so a window whose
  `OutputService` is absent or has stopped retries on the next rescan rather than inheriting
  permanent silence from one failed attempt. `LoadInstalledExtensionRootsWorker` removes an ID
  from that set the moment it stops being withheld (trust granted, the extension disabled, or
  uninstalled), so a later re-withholding -- trust revoked again, or the extension reinstalled
  -- reports again instead of staying silent forever because of a report tied to a now-stale
  reason.
- `Limited` and `Supported` both still admit the extension, and #37's
  `restrictedConfigurations` support did **not** change that: upstream marks a property
  restricted from the manifest's `restrictedConfigurations` list regardless of what
  `supported` says, so this codebase collects that list from every profile-enabled extension
  the same way and the `supported` value never gates it. What #37 removed is the *reason* the
  earlier divergence note gave — "there is no mechanism to express which settings remain
  live" — because that mechanism now exists and is enforced (see "Restricted Configurations
  and the User Override" below). What remains true is that `Limited` and `Supported` reach
  the identical admission decision here, which is also what they do upstream.
- No per-extension activation prompt returns anywhere in this delivery, consistent with the
  standing rule in "Activation and Installed-Set Changes" above: Workspace Trust is the only
  gate, it is workspace-scoped rather than per-extension, and withholding is silent to the
  user's workflow (no dialog, no blocked-action toast, no revealed panel) and loud only in
  the Extension Host log.

## Restricted Configurations and the User Override (2026-08-07, #37)

- `CExtensionManager::ParseRestrictedConfigurations` reads
  `capabilities.untrustedWorkspaces.restrictedConfigurations` off the same bounded picojson
  path and the same single `ReadManifestBody` string `ParseUntrustedWorkspaceSupport` already
  uses, so no third file open was added. Non-string members, non-canonical keys, and anything
  past `kMaxRestrictedConfigurationEntries` are dropped; a malformed manifest yields an empty
  list, never a partially honoured one.
- **That predicate is a file-local copy of `config/CConfigurationService.cpp`'s
  `IsCanonicalKey`, deliberately duplicated rather than included**, so `extension/` keeps its
  independence from `config/`. The two must be kept in sync: a key this parser accepts and
  the configuration service later rejects is silently dropped, which is exactly the kind of
  quiet no-op a security boundary must not have.
- `LoadInstalledExtensionRootsWorker` collects those keys from **every profile-enabled
  installed extension, including one withheld by trust in that same scan**, deduplicates
  them, and publishes them once per scan through
  `CExtensionWorkbenchServiceBridge::PublishExtensionRestrictedConfigurations`. Withholding
  means this one extension is not loaded *this scan*, not that the setting stops needing
  protection: the same key can still be read by another loaded extension, by native settings
  code, or by this extension the moment trust is granted and it reconnects. An empty publish
  is meaningful and is issued unconditionally — it is how a previously published set is
  cleared. A failed publish never fails the scan; the roots it computed are still correct.
- The bridge forwards to `IWorkbenchRuntime::SetExtensionRestrictedConfigurations`, which
  takes **only** the key set. Trust is read by the runtime from its own workspace context, so
  nothing under `extension/` can publish a key set against a trust state the runtime does not
  hold. See [`../config/CLAUDE.md`](../config/CLAUDE.md)'s Restricted Configurations
  Checkpoint for what happens to the value after that.
- `extensions.supportUntrustedWorkspaces` is the per-extension **user** override, shaped
  `{ "<publisher>.<name>": { "supported": true | false | "limited", "version": "x.y.z" } }`.
  `ParseExtensionUntrustedWorkspaceOverrides` and `ResolveUntrustedWorkspaceSupport`
  (`ExtensionUntrustedWorkspaceOverride.h`) are pure — no I/O, no service handle — so the
  whole matrix is unit-testable.
- **The override works in both directions.** It can widen a `NotSupported` manifest
  declaration to `Supported` and it can narrow a `Supported` one to `NotSupported`. Treating
  it as an exemption-only mechanism would silently discard the user's ability to distrust an
  extension the manifest declares safe, which is the direction that actually protects them.
- A `version` that is present applies the override **only** to that exact installed version;
  a mismatch makes the override inapplicable and the manifest declaration stands. An override
  recorded for one version must not silently carry into the next one, which is upstream's
  rule and the only one that survives an auto-update.
- Extension IDs are matched case-insensitively via `AreExtensionIdsEqual` (an explicit length
  check plus per-character `std::towupper`, **not** `wmemicmp`, because the `wstring_view`
  arguments are not guaranteed NUL-terminated). This matches both upstream's own
  case-insensitive comparison and `CExtensionManager::FindInstalled`'s existing behavior.
- `CExtensionWorkbenchServiceBridge::ExtensionUntrustedWorkspaceOverrides()` reads the value
  once per scan at the selected-profile target, through the same path
  `BuildConfigurationSnapshot` uses, so the worker thread has one read-only accessor and no
  second configuration cache. It is read once **before** the loop, not per extension, so
  every candidate in a scan is resolved against one snapshot of the setting.
- Every failure falls back to the manifest declaration: no runtime bound, a rejected read, a
  non-object value, a malformed entry. That direction is the fail-closed one — it can only
  leave support as declared or narrow it, never widen it on the strength of a read that did
  not succeed.
- `extensions.supportUntrustedWorkspaces` is a **settings value the user writes**, never a
  prompt. The standing "no per-extension activation confirmation" rule is unchanged: nothing
  in this delivery asks the user to approve an extension at activation time.
