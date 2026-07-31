# Secret Vault Contract Guidance

## Scope

`platform/secrets/` owns the UI-independent, per-profile secret-vault contract
introduced by Issue #6. It is deliberately separate from general state storage:
secret bytes must never be placed in settings, mementos, profile export,
diagnostics, normal logs, or change events.

## Authority and Identity

- A vault is bound once to the control authority's immutable canonical
  `profileId`; display names and legacy profile aliases are never identities.
- Extension identities are canonical lowercase ASCII `publisher.name`-style
  identifiers. Callers may supply case variants, but persisted/event addresses
  are canonical.
- The in-memory class is a deterministic reference authority only. Production
  editor and extension-host calls use the capability-checked control IPC bridge;
  they may not instantiate independent writers.
- Installed-ID membership authorizes a logical namespace for the trusted shared
  extension host. It is not proof of a mutually isolated extension principal.
  A hostile-extension isolation claim requires dedicated authenticated
  per-extension processes.

## Mutation and Event Rules

- Set/Delete use global vault revision CAS and an immutable operation ID. Exact
  replay returns the original terminal outcome; reuse with a different canonical
  payload is rejected.
- Only effective committed mutations advance a revision and notify. Idempotent
  Set/Delete is `NotApplicable` and has no event.
- Events carry only profile ID, canonical address, kind, and revision. Callbacks
  run after the state commit and outside the vault lock; exceptions cannot block
  other subscribers or later mutations.
- `Stop()` is terminal and idempotent. It closes subscriptions, discards pending
  notifications, and no later request may revive the authority.

## Phase 4 Production Checkpoint (2026-07-31)

- `CWindowsDpapiSecretVaultService` is the sole production durable writer. It
  preserves the public revision/CAS/replay contract, encrypts value and replay
  payloads at rest, writes atomically, bounds plaintext lifetime, and securely
  clears transient buffers.
- `CSecretVaultCapabilityService` and
  `CSecretVaultExtensionGrantAuthority` bind short-lived capabilities to the
  canonical profile, control generation, active host session, eligible
  extension namespace, and authorized editor process.
- `CSecretVaultLegacyMigrationCoordinator` performs bounded lazy import from the
  historical per-editor Vault and records terminal migration state. Migration
  failure never enables an independent fallback writer.
- Control-owned RPC is the only production bridge. Unknown extension IDs,
  stale sessions/generations, replay mismatches, conflicts, corrupt bytes,
  shutdown, and ambiguous transport loss remain distinct terminal outcomes.
