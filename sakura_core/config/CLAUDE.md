# P0/P1/P2 Configuration Guidance

## Scope

This directory owns configuration contracts and static configuration. Legacy
Sakura INI serialization remains behind an adapter under `env/` until migrated.

## Configuration Model

- Resolve default, application, profile/user, workspace, folder, and language
  override scopes in a documented order.
- Keep configuration values separate from memento state, secrets, working-copy
  backups, and profile association.
- Configuration changes use CAS revisions, bounded replay, and ordered
  notifications. Every accepted update has one revisioned terminal outcome;
  stale writers must observe a conflict rather than overwrite newer state.
- Consumers that derive one policy from multiple keys must use `ReadSnapshot`.
  It resolves all requested effective values under one lock and returns one
  service-wide committed revision; repeated `GetValue` calls are not a
  coherent policy read.
- Parse errors never cause the original configuration file to be overwritten;
  retain the last valid model and publish a diagnostic.

## Shipped Caption Defaults

- The window and tab caption defaults in `CShareData.cpp` and the migrations
  in `CShareData_IO.cpp` are one unit: changing a shipped default without
  adding its migration leaves every existing profile on the old string.
- Migration compares the **whole** previous shipped default and rewrites only
  an exact match, so a caption the user edited is left alone. Keep each
  superseded default as its own named constant rather than editing the
  current one in place.
- `CShareDataTest.InitShareData001` asserts the shipped defaults verbatim.
  A default that changes without that test changing is a defect in one of
  the two.

## Workspace Context

- Workspace identity is explicit: `EMPTY` has no root, `FOLDER` has one folder
  identity, and `WORKSPACE` has a multi-root/workspace-file identity. A
  configuration URI is optional metadata, not a substitute for that identity.
- `CWorkspaceContextService` owns only this in-memory workspace identity and
  its revisioned transitions. Workspace Trust, Restricted Mode, Trusted
  Folders, and trust mementos are not configuration concepts and are not
  represented in context snapshots.
- `CWorkbenchRuntime` is the production owner of file-backed configuration. It
  loads the resolved Profile `settings.json` and each explicitly established
  Folder/Workspace root's `.vscode/settings.json` through the scheme-aware file
  service. A loose initial document never authorizes workspace discovery.
- Reads are bounded and JSONC documents are applied atomically. Parse, read,
  and apply failures retain the last accepted model and become typed,
  path-free workbench diagnostics. Unknown keys remain latent source entries
  so a later descriptor can activate them without losing source identity.
- `CConfigurationFileWatchController` owns cancellable, non-recursive advisory
  watches for profile settings, explicit folder settings, and `.code-workspace`.
  It coalesces events and reports only resource classes; the workbench must
  resnapshot through `CConfigurationFileSourceController::Reload` rather than
  applying callback contents directly.
- The watch controller owns topology identity. `Update` with a request whose
  resources are URI-identical (in order) to the current one is a no-op that
  still returns `Updated`; only a changed topology rebuilds and announces
  `FullRescan`. Consecutive rebuilds back off (100 ms doubling to 30 s, +25%
  jitter, reset after 5 s quiet). Issue #395: `CWorkbenchRuntime` refreshes
  the topology from its own `FullRescan` handler, and while an unchanged
  `Update` rebuilt, that pair recreated every watch thread each ~25 ms for the
  window's lifetime (114-170 threads/s measured). The NVIDIA D3D11 driver keeps
  ~192 B per thread ever created, so commit grew ~90-130 MiB/h per window until
  the system commit limit failed. Keep both guards; a rebuild is never free.
- A watch worker that cannot start never ends the process (#397). Thread
  creation fails exactly under the commit pressure #395 produced, and
  `RebuildTopology` runs on the dispatcher inside `noexcept` code, so before
  this an exception there called `std::terminate` and lost unsaved work for an
  advisory watch.
  - `StartWorkersLocked` is `noexcept` and returns false on any failure:
    thread creation, a throwing watch provider, or an allocation. It cancels
    every watch it holds, and its caller joins the workers that did start
    outside `m_mutex`, because `WorkerMain` takes that lock in `Queue`.
  - `Start` keeps its contract: `StartFailed`, every started worker joined,
    and the controller reusable.
  - A failed rebuild leaves no watch, announces `WatchUnavailable` instead of
    `FullRescan`, and requests another rebuild. That retry is an ordinary
    consecutive rebuild, so it waits under the same backoff (100 ms doubling
    to 30 s). The first retry that succeeds announces `FullRescan`. A
    per-resource change consumed by the failed iteration is not announced
    separately; that later `FullRescan` covers it.
  - Residual gap: `CWin32FileWatch::Start` contains its own reader-thread
    failure and returns `Failed`, and `add()` treats every failed `Watch` as
    a capability gap, as it must for the `NotFound` of a missing `.vscode`.
    Under commit pressure a rebuild can therefore succeed with a partial
    topology and set no diagnostic. Telling a resource failure apart from a
    missing directory needs a typed provider status first.
  - `CWorkbenchRuntime` reports `WatchUnavailable` as the
    `settings.watch.workers` diagnostic and clears it on the next
    `FullRescan`.
  - Worker creation goes through the `WorkerStarter` seam. Production leaves
    it empty and uses `std::thread`; the tests inject a starter that throws
    `std::system_error` (`ConfigurationFileWatchController.*CannotStartItsWorkers*`
    and `*KeepsFailingToStartWorkers*`).
  - `CWorkspaceArtifactDocumentSourceController` already contained the same
    failure for its own workers.
- `CSettingsWritebackCoordinator` owns one bounded JSONC writeback transaction:
  it performs a versioned read and conditional atomic replace through
  `CJsoncConfigurationEditor`, replays a CAS conflict at most once, then
  resnapshots through the same source controller that owns external-watch
  reloads. A failed resnapshot is terminal.
- `.vscode/tasks.json` and `launch.json`, plus their `.code-workspace` members,
  are routed by the separate bounded `CWorkspaceArtifactDocumentService` and
  its file-source/watch controller. They never become effective Settings
  entries.

Configuration UI consumes these services and may not read or write
`CShareData_IO`, INI files, or workspace JSON directly.

## Network Policy Identity

- The network-policy target's `profileId` is the selected user-data profile
  handed down from bootstrap, never the control authority id. Validate it with
  `platform::profiles::IsOpaqueUserDataProfileId`.
- Network policy is profile/application scoped. Workspace files cannot inject a
  proxy, proxy credentials are never configuration keys, and
  `http.proxyStrictSSL=false` terminates as unsupported at a production
  transport boundary rather than disabling certificate validation silently.
- `CConfigurationProxyService::SelectProxy` keeps "no proxy applies"
  (`NoProxyRequired` -> `Direct()`) separate from "the system could not answer"
  (`Unavailable` -> `Unsupported`, unless a configured fallback proxy exists).
