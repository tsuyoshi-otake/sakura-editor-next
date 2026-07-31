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
