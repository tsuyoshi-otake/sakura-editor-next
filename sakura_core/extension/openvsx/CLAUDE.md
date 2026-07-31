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

- `CreateOpenVsxProductionClient` validates the canonical immutable profile ID,
  reads exactly one coherent network-policy snapshot, and constructs a
  self-contained client. A successful client retains no configuration-service
  or workbench-runtime reference.
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
- VSIX transfer is bounded but currently materialized in memory before staging.
  A future streaming request/file sink may reduce peak memory, but it must keep
  the same limits, cancellation, hash verification, and atomic commit contract.

## Tests

- Use fake `IRequestService` or `IOpenVsxRegistryClient` implementations for
  routine tests. Never depend on the public OpenVSX service.
- Keep live search/install checks disabled and opt-in. They do not replace
  deterministic failure, cancellation, cleanup, and redaction tests.
