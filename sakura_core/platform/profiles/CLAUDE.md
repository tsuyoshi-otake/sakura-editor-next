# Profile Authority Guidance

## Ownership and Identity

- The resolved legacy profile directory is the migration anchor. The control
  process is the only owner allowed to acquire and publish its authority.
- `profileId` is opaque, immutable, and independent of the profile display name
  or legacy `-PROF` alias. The alias is validation/compatibility input only and
  is never persisted as durable identity.
- Every successful control-owner acquisition advances a nonzero authority
  generation only after the protected metadata record is durably committed.
  Editors receive the ID and generation through `ControlPlatformEndpoint`; they
  never open the authority store.

## Failure Contract

- Treat the authority file and lock as a durable trust boundary. Use the shared
  current-user-only, non-inheriting security descriptor and retain exclusive
  ownership for the authority lifetime.
- Corrupt, unsupported, duplicate, stale, exhausted, or uncommitted metadata
  fails closed. Never repair it by silently minting a different ID, resetting the
  generation, or deriving identity from a path/name hash.
- Every acquisition and release branch has an explicit terminal result. Fault
  injection tests must prove that a failed flush or atomic replace cannot publish
  an uncommitted generation.

## Durable User Data Profile Registry

- The control process is the single writer. Editors receive resolved immutable
  profile selection results; they neither load nor mutate the durable registry.
- `DurableUserDataProfileRegistryService` uses one bounded UTF-8 `IStorageService`
  machine-state value for profile metadata only. Secret values, installed
  extension payloads, and profile resource contents are outside this format.
- Load validates the complete versioned document before one atomic registry
  replacement. Corrupt or unsupported bytes remain stored and must not be
  replaced with defaults.
- Durable and portable exports omit every transient profile and its
  associations. Portable import detects opaque-ID, display-name, and
  association collisions as terminal typed outcomes; it never silently merges
  a conflicting selection.
- `ControlUserDataProfileRegistry` is the production control-process mutation
  boundary. It loads before the control endpoint is published, accepts only
  bounded caller-supplied operation IDs and CAS revisions, rolls back any
  uncommitted registry mutation, and performs its final save before storage
	  closes. It has no HWND, PID, or UI identity input.
	- `ControlProfileRpc` is the editor-facing projection of that boundary. Its
	  payload v1 is bounded and rides only a completed, generation-pinned control
	  Hello session. Snapshot/list/current/resolve return immutable copies;
	  profile IDs remain opaque and workspace association accepts a validated URI,
	  never a raw path. Create/rename/delete/associate/import preserve the caller's
	  operation ID and storage-revision CAS contract through to the single writer.

## Selected User-Data Bootstrap Checkpoint (2026-07-31)

- `ControlProfileAuthorityIdentity` identifies the control endpoint that
  supplied a registry snapshot. It is never reused as the selected user-data
  profile identity.
- `UserDataProfileBootstrap` resolves explicit profile, workspace association,
  empty-window association, then Default in that order from one immutable,
  bounded registry document obtained through Profile RPC. It performs no I/O
  and fails closed on an invalid authority, selector, registry, descriptor, or
  resource URI.
- Profile resource roots derive from opaque profile IDs, never display names.
  The URI-only snapshot covers Settings, Keybindings, Snippets, Tasks,
  Extensions selection, Global State, Working Copies, and Workbench Layout; it
  deliberately exposes no secret resource.
- A stable empty-window association key is not a PID. Until the control process
  owns durable per-window keys, production uses only
  `empty-window:default-window` as a documented single-window compatibility
  bridge. Do not mint process-derived or random identities in editor startup.
- Only the Default profile may explicitly select the legacy control root.
  Named/transient profiles always use the opaque-ID namespace. Removing this
  compatibility mode requires a durable, crash-safe migration marker rather
  than probing for whichever directory happens to contain data.

The pure bootstrap suite passes 7/7. Native create/select/switch/manage flows,
durable empty-window identity allocation, and the default-root migration remain
open Phase 3 work.

## User-Data Profile Identity vs. Control Authority Identity (2026-08-01)

- Two distinct identity spaces exist and must never validate each other's
  values. The **control authority id** (`ProfileAuthorityIdentity.h`,
  `IsCanonicalProfileAuthorityId`) is exactly 32 lowercase hex characters, the
  form `ProfileAuthorityStore` mints for the control endpoint's durable
  anchor. The **selected user-data profile id** (`UserDataProfileIdentity.h`,
  `IsOpaqueUserDataProfileId`) is `[A-Za-z0-9_-]`, 1..
  `kMaximumUserDataProfileIdCharacters` (128) characters — the shape of
  `Bootstrap().UserDataProfile().SelectedProfileId()`, including the Default
  profile's fixed literal `L"default"` (`UserDataProfileRegistry.cpp:64-67`).
- VS Code precedent: its Default profile also carries a fixed literal id
  (`'__default__profile__'`); only named profiles get a generated
  `hash(generateUuid())` id. A fixed non-hex literal default-profile id is
  therefore upstream behaviour, not a local shortcut, and every profile-scoped
  consumer must accept it.
- A profile-scoped service that receives the selected user-data profile id —
  network policy or any other consumer of `SelectedProfileId()` — validates
  with `IsOpaqueUserDataProfileId`.
  Control-endpoint, durable-storage, and Vault adapters that consume an id
  minted by `ProfileAuthorityStore` keep `IsCanonicalProfileAuthorityId`.
  Applying the canonical-hex predicate to a selected profile id fails closed
  unconditionally (`"default"` can never be 32 hex characters); that
  cross-space mismatch can make every selected-profile consumer fail closed.
  See [`../../config/CLAUDE.md`](../../config/CLAUDE.md) for the network-policy
  call site.
- `UserDataProfileIdentity.h` is the single shared home for the opaque
  predicate. Do not reintroduce a private copy in a bootstrap or workbench
  file — `UserDataProfileBootstrap.cpp` and `WorkbenchBootstrapContext.cpp`
  previously carried byte-identical private copies before this
  consolidation.
