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
- The durable Trusted Folders and Workspaces list is not implemented yet. The
  entry list is empty by construction, so no folder can currently resolve to
  `Trusted`; only the disabled-feature and empty-window rules can. That is
  the honest answer for a folder whose trust was never granted, and it must
  not be replaced with a placeholder that assumes trust.
  `workbench.trust.manage`, the trust dialog, Restricted Mode UI,
  `security.workspace.trust.untrustedFiles`, `startupPrompt`, `banner`, and
  activation gating on `capabilities.untrustedWorkspaces` all remain
  unimplemented.
