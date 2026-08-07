# P0/P1/P2 Configuration Guidance

## Scope

This directory owns configuration contracts and static configuration. Legacy
Sakura INI serialization remains behind an adapter under `env/` until migrated.

## Configuration Model

- Resolve default, application, profile/user, workspace, folder, and language
  override scopes in a documented order.
- Keep configuration values separate from Memento state, secrets, working-copy
  backups, extension enablement, and profile association.
- P0 defines descriptors, inspection, effective-value resolution, revisioned
  change events, workspace identity/trust, and variable-resolution boundaries.
- P2 adds JSONC-preserving edits, `.vscode/settings.json`, `tasks.json`,
  `launch.json`, `extensions.json`, and `.code-workspace` multi-root handling.
- Parse errors never cause the original configuration file to be overwritten;
  retain the last valid model and publish a diagnostic.
- One configuration document is one transaction. Commit its base entries and
  every language-override section through `ReplaceSources`; never expose a
  partially applied `settings.json` when one source conflicts or is invalid.
- Consumers that derive one policy from multiple keys must use `ReadSnapshot`.
  It resolves all requested effective values under one lock and returns one
  service-wide committed revision; repeated `GetValue` calls are not a coherent
  policy read. Source-local revisions remain the CAS tokens for writes.
- Network policy is profile/application scoped. Workspace files cannot inject a
  proxy or OpenVSX registry, proxy credentials are never configuration keys, and
  the OpenVSX endpoint must be HTTPS. `http.proxyStrictSSL=false` is represented
  for compatibility but must terminate as unsupported at a production transport
  composition boundary rather than disabling certificate validation silently.
- `CConfigurationProxyService::SelectProxy` must keep "the system reports that
  no proxy applies" (`NoProxyRequired` -> `Direct()`) separate from "the system
  could not answer" (`Unavailable` -> `Unsupported`, unless `Fallback` has a
  configured `http.proxy`). Merging them made every unproxied machine fail
  closed; see
  [`../platform/request/CLAUDE.md`](../platform/request/CLAUDE.md).

## Workspace Context Checkpoint

- Workspace identity is explicit: `EMPTY` has no root, `FOLDER` has one folder
  identity, and `WORKSPACE` has a multi-root/workspace-file identity. A
  configuration URI is optional metadata, not a substitute for that identity.
- Trust is explicit workspace state and is carried with context snapshots; do
  not infer trust from a path, file extension, or successful configuration load.
  See the Workspace Trust Resolution Checkpoint below for how that state is
  actually resolved.
- Context/configuration changes use CAS revisions, bounded replay, and ordered
  notifications. Every accepted update has one revisioned terminal outcome;
  stale writers must observe a conflict rather than overwrite newer state.
- `CWorkbenchRuntime` is the production owner of file-backed configuration. It
  loads the resolved Profile `settings.json` and each explicitly established
  Folder/Workspace root's `.vscode/settings.json` through the scheme-aware file
  service. A loose initial document never authorizes workspace discovery.
- Reads are explicitly bounded and JSONC documents are applied atomically. Parse,
  read, and apply failures retain the last accepted model and become typed,
  path-free workbench diagnostics. Unknown keys remain latent source entries so
  a later extension descriptor can activate them without losing source identity.
- `CConfigurationFileWatchController` owns cancellable, non-recursive advisory
  watches for profile settings, explicit folder settings, and `.code-workspace`.
  It coalesces events and reports only resource classes; `CWorkbenchRuntime`
  must resnapshot through `CConfigurationFileSourceController::Reload` rather
  than applying callback contents directly. Overflow, rescan, and disposal
  rebuild topology and require a full reload. Parse/read failure retains the
  last accepted model and becomes a typed, path-free runtime diagnostic.
- `CSettingsWritebackCoordinator` owns one bounded JSONC writeback transaction:
  it performs a versioned read and conditional atomic replace through
  `CJsoncConfigurationEditor`, replays a CAS conflict at most once, then
  resnapshots through the same `CConfigurationFileSourceController` that owns
  external-watch reloads. A failed resnapshot is terminal; callers may never
  mutate effective configuration from the requested value or from watcher
  callback contents.
- JSONC writeback retains comments and unrelated text. Language overrides edit
  only the VS Code-compatible top-level `"[languageId]"` object; a combined
  selector is not silently split because that could create overlapping source
  contributions. Missing `settings.json` creation uses expected-missing CAS.
- `.vscode/tasks.json`, `launch.json`, and `extensions.json`, plus their
  `.code-workspace` members, are routed by the separate bounded
  `CWorkspaceArtifactDocumentService` and its file-source/watch controller.
  They never become effective Settings entries. Tasks execution, DAP launch
  consumption, and extension-recommendation application remain pending backend
  consumers of those accepted artifact snapshots.

Configuration UI and extension RPC consume the same service. They may not read
or write `CShareData_IO`, INI files, or workspace JSON directly.

## Network Policy Identity Checkpoint (2026-08-01)

- The network-policy target's `profileId` is the selected user-data profile
  handed down from bootstrap (`Bootstrap().UserDataProfile().SelectedProfileId()`),
  never the control authority id. `CConfigurationNetworkPolicy::Snapshot`
  therefore validates it with `platform::profiles::IsOpaqueUserDataProfileId`,
  not the control authority's canonical-hex predicate; see
  [`../platform/profiles/CLAUDE.md`](../platform/profiles/CLAUDE.md) for the
  two identity spaces and why conflating them fails every read closed.
- `LayerIdentity` matches a Profile-scope source by exact string equality of
  `"profile" + target.profileId` (`CConfigurationService.cpp:259-262`). A
  target whose `profileId` matches no registered source does **not** fail:
  `ReadSnapshot` still returns `EConfigurationOutcome::Applied`
  (`CConfigurationService.cpp:659-690`) and `EffectiveLocked` silently falls
  back to the descriptor default because no Profile-scope candidate was
  collected (`CConfigurationService.cpp:558-598`). A caller that reads
  network policy, or any other profile-scoped key, through the wrong
  `profileId` gets plausible-looking defaults instead of an error — which is
  why correctness here depends on validating the *right* identity space
  up front rather than on the read path catching a mismatch.
- VS Code registers `http.proxy`, `http.proxyStrictSSL`, `http.proxySupport`,
  and `http.systemCertificates` with `ConfigurationScope.APPLICATION`
  ("Application specific configuration, which can be configured only in
  default profile user settings"). **Documented divergence:** our network
  policy is instead read through the selected profile's layered
  configuration (Profile scope), because this repository has no separate
  Application-scope configuration layer yet — `CConfigurationNetworkPolicy`
  is scoped to whichever profile is selected, not pinned across profiles the
  way VS Code pins it. Closing this divergence requires adding a true
  Application scope shared by every profile, not merely validating the
  selected profile's id correctly.

## Workspace Trust Resolution Checkpoint (2026-08-07, #33)

- `config::ResolveWorkspaceTrust` (`WorkspaceTrustPolicy.h`/`.cpp`) is a pure
  function: no I/O, no HWND, no service handle. It reads only the workspace
  kind, the optional workspace-configuration URI, the folder URIs, the
  `security.workspace.trust.enabled` / `.emptyWindow` settings, and the
  Trusted Folders and Workspaces entry list. It is fully covered by
  `WorkspaceTrustPolicyTest`.
- Precedence is fixed and ordered. `security.workspace.trust.enabled = false`
  trusts everything and outranks every other rule, including an explicit
  empty-window opt-out. An empty window otherwise follows
  `security.workspace.trust.emptyWindow` (default `true`). Otherwise a
  `.code-workspace` file that is itself trusted covers the whole multi-root
  workspace regardless of where its folders live. Otherwise every folder root
  must be covered; trust is not the union of the roots, because extension
  code runs once for all of them, so one uncovered root leaves the whole
  window untrusted.
- The resolver never produces `EWorkspaceTrustState::Untrusted`. `Untrusted`
  denotes an explicit user denial, and no amount of derived state can
  establish one; withheld trust resolves `Unknown` instead. A dedicated test
  asserts this distinction.
- Ancestor containment ("trust the parent folder") is a path-segment-boundary
  rule, not a string-prefix rule. `WorkspaceTrustEntryCovers` rebuilds the
  resource URI at each of its own `/` boundaries and asks
  `UriIdentityService` whether the rebuilt URI is the entry, so every
  case-folding, authority-aliasing, and escaping rule stays in one place.
  Comparing `MakeComparisonKey` outputs as strings would be wrong: those keys
  are length-prefixed and structured, so a prefix test on one carries no
  ancestor meaning. Proven by test: `file:///c:/codes/app` does not cover
  `file:///c:/codes/application`.
- `CWorkbenchRuntime::ResolveAndApplyWorkspaceTrust` is the only production
  caller of `CWorkspaceContextService::SetTrust`. It runs once during
  `Start()` after the profile settings load (because it reads
  `security.workspace.trust.*`), and again at the top of
  `OnWorkspaceContextChanged`, because trust follows the workspace shape and
  must be re-resolved even when the configuration shape is unchanged and the
  settings reload below it is skipped.
- Each resolution mints a fresh `workbench.trust.resolve.<N>` operation ID
  from a runtime-owned atomic counter. `SetTrust`'s replay/conflict
  bookkeeping is keyed on `(operationId, fingerprint)` with the trust value
  folded into the fingerprint, so a reused identifier carrying a different
  value is reported as a conflict rather than accepted as a new request.
- The trust settings read targets only the selected user-data profile
  (`Bootstrap().UserDataProfile().SelectedProfileId()`); trust policy is
  profile-owned and is never accepted from a workspace or folder document. A
  failed or short read falls back to the struct defaults, which withhold
  trust for every folder — a failed read must never widen trust.
- Trust is context state, so committing it advances the semantic workspace
  revision. A topology change therefore settles as two revisions (topology,
  then trust), and `WorkspaceArtifacts` generation tracks whichever revision
  was current when artifacts were last routed. Asking to set trust to its
  current value resolves `NotApplicable` and commits nothing, so resolution
  converges after one step and never recurses through its own notification.
  Tests must not hard-code a literal post-`Start()` revision —
  `CWorkbenchRuntime.ArtifactTopologyTransitionsClearDocumentsAndAdvanceSemanticGeneration`
  was rewritten to settle on the generation/revision relationship instead of
  a fixed count.
## Trusted Folders Store and Grant Path Checkpoint (2026-08-07, #35)

- `config::ITrustedFoldersStore` is the port for the durable Trusted Folders
  and Workspaces list. `CControlPlatformTrustedFoldersStore` is its only
  production implementation, and it is the only thing allowed to write that
  list. The record is the profile-scoped `workbench.trust` owner's
  `trustedFolders` key at `Machine` target, using the canonical profile ID —
  the same shape as the layout memento, for the same reason: a machine-owned
  decision must not travel with synchronized user settings.
- `CTrustedFoldersCodec` owns the payload. It is bounded machine-owned JSON
  (`kMaximumTrustedFolderEntries = 256`, `kMaximumTrustedFolderUriBytes =
  2048`, `kMaximumTrustedFoldersJsonDepth = 8`), validated as UTF-8 before
  parsing, and checked against
  `platform::storage::kMaximumStorageMutationPayloadBytes` on both ends.
  Two deliberate divergences from the sibling layout codec:
  duplicate entries are **accepted** on decode, because a list that already
  covers a resource twice is a redundancy and not a corruption, and the grant
  path is what prevents duplicates from accumulating; and a `formatVersion`
  that is present but not a finite non-negative integral number resolves
  `UnsupportedSchema` rather than `CorruptPayload`, because a newer writer
  choosing a different version encoding is a schema fact, and both statuses
  preserve the payload untouched anyway.
- Persistence discipline mirrors the layout memento store exactly: a double
  read of the storage-cache coordinates brackets the `find`, the write is a
  CAS on the captured revision, an ambiguous transport failure retries **at
  most once with the identical operation ID**, and a `Conflict` is terminal —
  never retried, never overwritten. An invalid stored payload is preserved for
  diagnosis, marked with a sticky flag, and never replaced with defaults.
- `CWorkbenchRuntime::GrantWorkspaceTrust` writes the durable bytes **first**
  and only then re-runs `ResolveAndApplyWorkspaceTrust`. A runtime that cannot
  commit refuses the grant rather than trusting for this session only: a
  session-only grant would report trust the window cannot keep, and the next
  launch would silently drop back to withheld trust with nothing recorded to
  explain why. Trust is therefore still decided by exactly one code path — the
  pure resolver — so a grant that does not actually cover the workspace cannot
  fake it.
- `BuildTrustGrantEntries` is pure and is shared by the prompt and the grant,
  so the two can never disagree about what a choice means. `CurrentWorkspace`
  on a `Workspace` writes exactly one entry, the `.code-workspace` file, with
  `includesDescendants = false` — the resolver already treats a trusted
  workspace file as covering every folder it lists, and descendant coverage
  would silently trust whatever directory the file happens to sit in.
  `CurrentWorkspace` on a `Folder` writes each root with descendants.
  `ParentFolder` is offered only for a **single** folder root, matching
  upstream: with several roots the label names one folder while the grant
  would widen every root's parent at once, which is not the decision shown.
- `config::WorkspaceTrustParentFolder` is path-only and performs no filesystem
  lookup, so it cannot follow a link or resolve a relative segment. A parent
  that would be the scheme root — a drive root, a UNC share root, a path that
  is only a separator — returns nothing. "Trust the parent" must never
  silently widen into a whole volume or host.
- An existing entry that already covers the requested resource with at least
  the same reach suppresses the append, so repeating a grant returns
  `AlreadyTrusted` and writes nothing. Without that check the durable list
  would grow by one entry every time the user confirms, since the codec
  accepts duplicates.
- The still-unimplemented surfaces are: Restricted Mode UI (banner and the
  `$(shield) Restricted Mode` status-bar entry), the full Workspace Trust
  editor page, activation gating on `capabilities.untrustedWorkspaces`,
  `restrictedConfigurations`, `extensions.supportUntrustedWorkspaces`, and
  `security.workspace.trust.startupPrompt` / `.banner` / `.untrustedFiles`.
  Each must stay an explicit typed boundary; none may be approximated.

### `workbench.trust.manage` divergence

Upstream's `workbench.trust.manage` opens the Workspace Trust **editor page**,
a full editor input with its own body copy, per-folder table, and settings
links. This product opens a native `TaskDialogIndirect` modal instead, because
that page is an editor-hosted rich surface this shell has no renderer for, and
because a browser engine is a product-level non-goal.

What the modal keeps identical to upstream: the command ID, the title
(`Workspaces: Manage Workspace Trust`), the set of grantable choices and their
exact scope semantics, the shield iconography, and that dismissal grants
nothing. Each button is labelled with the canonical URI the grant would
actually write, so the consent names a resource rather than a category.

Upstream additionally gates the command on
`config.security.workspace.trust.enabled`. This registry has no `config.`
context-key namespace, so the `when` clause is `workbenchReady` and the palette
entry stays listed where upstream would hide it. It cannot grant trust the
settings did not allow: with the feature disabled every workspace already
resolves `Trusted`, so the modal reports that state and offers no button.

**This command is a workspace-level decision and must never be framed, titled,
or triggered as a per-extension activation gate.**
