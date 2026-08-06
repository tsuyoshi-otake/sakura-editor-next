# Phase 4 OpenVSX Registry Guidance

## Boundary

This directory owns OpenVSX protocol parsing, the typed registry-client
contract, its shared-request adapter, and the production dependency factory.
It does not own extension installation layout, UI controls, profile selection,
general HTTP policy, credentials, or SecretStorage.

## Protocol and Capability Rules

- `IOpenVsxRegistryClient` is the only production network boundary for search,
  VSIX bytes, and optional SHA-256 metadata. Do not construct `CHttpClient`,
  WinHTTP handles, or registry URLs in `CExtensionPane` or
  `CExtensionManager`.
- Every operation returns a typed terminal status plus an endpoint-specific
  value. Never turn cancellation, limit failure, authentication, TLS failure,
  malformed response, or HTTP failure into a false success.
- Registry and endpoint URIs must be absolute HTTPS URIs without userinfo.
  Redirects remain governed by the shared request service.
- Keep response limits endpoint-specific. The current production bounds are
  8 MiB for search JSON, 64 KiB for SHA-256 metadata, 512 MiB for VSIX bytes,
  64 KiB for headers, three redirects, and two retries.
- Diagnostics crossing the UI boundary never contain endpoint URLs, profile
  IDs, proxy values, credentials, response bodies, or expected/actual hashes.

## Production Composition and Lifetime

- `CreateOpenVsxProductionClient` validates the selected user-data profile's
  opaque identity (`platform::profiles::IsOpaqueUserDataProfileId`), not the
  control authority's canonical-hex form; see
  [`../../platform/profiles/CLAUDE.md`](../../platform/profiles/CLAUDE.md) for
  the two identity spaces (2026-08-01). Its parameter was renamed from
  `canonicalProfileId` to `userDataProfileId` to match — the old name
  asserted the control-authority shape while the value was always the
  selected user-data profile id. It reads exactly one coherent network-policy
  snapshot and constructs a self-contained client; a successful client
  retains no configuration-service or workbench-runtime reference. The
  gallery endpoint itself comes from `IProductService`/`product.json` and has
  no profile-scoped branching, so switching the selected profile never
  changes which registry is searched — only the network policy (proxy, TLS,
  response limits) is profile-scoped.
- `CExtensionPane` creates that client synchronously before starting a Search or
  Install worker. The shared job owns it; a detached worker may therefore
  finish or cancel after the pane/window starts teardown without dereferencing
  UI or runtime state.
- Search and install use the same client generation. `CExtensionManager`
  receives the client explicitly, stages fetched bytes, verifies optional
  SHA-256 metadata, safely extracts, and atomically commits the installation.
  Failure and cancellation remove the staging tree.
- Uninstall is local and does not construct a registry client.

## Phase 4 Verified Checkpoint (2026-07-31)

- The legacy production `COpenVsxClient`/`CHttpClient` bypass has been removed
  from the Extensions pane and manager.
- Focused deterministic evidence currently covers configuration-backed proxy
  snapshot lifetime, request-to-OpenVSX outcome mapping, production-client
  factory validation/lifetime/limits, install cancellation, redaction, and
  staging cleanup.
- Authentication is not yet production-complete. The control-owned Secret Vault
  now exists for ExtensionContext SecretStorage, but the factory intentionally
  uses a no-credential adapter until a separate challenge-scoped credential
  service is composed. SecretStorage must never be treated as implicit registry
  or proxy authentication.
- VSIX transfer now streams: `IOpenVsxRegistryClient::FetchVsixStreamed` (default
  `Unsupported`, implemented by `OpenVsxRequestServiceAdapter` and delegated by
  `OpenVsxProductionClient`) delivers the response body to a caller-supplied
  chunk sink instead of returning a fully buffered value. `CExtensionManager::
  Install` writes each chunk straight to the staging temp file and folds it into
  an incremental BCrypt SHA-256 hash, so peak resident memory for a VSIX no
  longer scales with archive size. The 512 MiB response-body ceiling
  (`kMaximumVsixResponseBytes`) is unchanged and is still enforced by the shared
  `RequestService`/WinHTTP transport while streaming, not relaxed. A client that
  does not implement streaming (returns `Unsupported`) falls back to the
  original fully-buffered `FetchVsix` + post-write hash path unchanged, so this
  is strictly additive. `platform::request::Request`/`TransportRequest` gained
  one optional trailing `bodySink` field (a `ResponseBodyChunkSink`) rather than
  a broader streaming-interface redesign: both structs are always built by
  default-construction plus named-field assignment in production code, so a new
  trailing optional field is a backward-compatible addition, and the WinHTTP
  transport's existing 64 KiB chunked `WinHttpReadData` loop only needed a
  branch on whether a sink is present, not a new read strategy. `RequestService`
  rejects combining `bodySink` with deduplication or non-`OnlineOnly` caching
  rather than trying to make a streamed body cacheable/replayable.

## Tests

- Use fake `IRequestService` or `IOpenVsxRegistryClient` implementations for
  routine tests. Never depend on the public OpenVSX service.
- Keep live search/install checks disabled and opt-in. They do not replace
  deterministic failure, cancellation, cleanup, and redaction tests.
