# Phase 4 Extension Registry Tests

## OpenVSX

- Routine tests use fake `IRequestService` or
  `extension::openvsx::IOpenVsxRegistryClient` implementations. Public network
  access is prohibited.
- Verify the production factory rejects invalid profile/configuration state
  before network work, captures one coherent policy snapshot, and returns a
  client that outlives configuration objects without borrowing them.
- Cover every typed request outcome, HTTPS/redirect validation, endpoint body
  limits, cancellation, timeout, authentication failures, response parsing,
  and value-free diagnostics.
- Installer tests use an isolated explicit base directory. Every fetch,
  integrity, extraction, cancellation, and commit failure must leave no staging
  tree or partial installed extension.
- Live OpenVSX tests remain `DISABLED_` and opt-in. They are smoke tests only,
  not acceptance evidence for deterministic contracts.

## Secret and Credential Boundary

- Never place real credentials or secret values in test fixtures, command
  lines, diagnostics, snapshots, or repository files.
- Production SecretStorage compatibility is not proven by the legacy
  per-editor DPAPI tests. Control-owned vault, authenticated IPC, namespace
  binding, CAS/replay, redaction, and shutdown require their own deterministic
  tests.

