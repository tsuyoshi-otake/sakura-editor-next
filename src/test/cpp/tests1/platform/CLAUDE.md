# P0 Platform Test Guidance

Keep platform-contract tests deterministic, UI-free, and network-free. Prefer
fake providers, clocks, writers, and temporary directories owned by the test.

Cover duplicate/missing service registration, dependency cycles, startup
rollback and reverse shutdown, URI identity, provider capabilities, profile
migration idempotence, scope/target separation, revision conflicts, operation
replay, batch visibility, flush/recovery, corruption fallback, and
snapshot-before-subscription resync. Every branch must assert an explicit
terminal result and the final durable/in-memory state.

## Phase 3 Selected-Profile Gates

- Decode only a bounded, valid registry document received through the Profile
  RPC facade. Corrupt/unsupported bytes must remain a typed failure and must not
  be converted to the Default profile.
- Cover explicit/workspace/empty-window/Default precedence, stable
  empty-window identity validation, opaque-ID resource isolation, rename
  stability, and the Default-only legacy-root bridge.
- Prove the selected snapshot retains the pinned Control authority without
  conflating its identity with the selected profile, and exposes no secret URI.
- Client/runtime tests cover fresh Hello ordering, one-in-flight arbitration,
  exactly one terminal response, generation mismatch, Stop/resnapshot channel
  cancellation, conflict-triggered resnapshot, and same-operation-ID ambiguous
  mutation replay.

The 2026-07-31 bootstrap suite passes 7/7, the Profile client/runtime suite
passes 35/35, and the integrated profile/runtime/command cohort passes 79/79.
