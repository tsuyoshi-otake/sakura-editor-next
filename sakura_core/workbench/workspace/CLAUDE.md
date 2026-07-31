# Workspace Artifact Document Guidance

## Ownership boundary

`CWorkspaceArtifactDocumentService` is the pure, process-local router for
`.vscode/tasks.json`, `.vscode/launch.json`, `.vscode/extensions.json`, and
their corresponding `.code-workspace` members. It retains the whole accepted
JSONC source and root object so unrelated members are not discarded.

These documents are **not** `IConfigurationService` inputs. Only the existing
workspace `settings` path may enter configuration. This service exposes separate
Tasks, Launch, and Extensions snapshots, but it does not execute tasks, create a
debug adapter session, or apply extension recommendations.

## Update and lifecycle rules

- Filesystem reads and watches belong to an external interface/adapter; pass its
  immutable `{ source, resource, generation, revision, UTF-8 }` result into the
  service. Never add Win32, HWND, file watching, or direct filesystem I/O here.
- A folder document has higher precedence than a `.code-workspace` member only
  for the matching canonical folder URI. The workspace-file member remains the
  fallback for other folders.
- Invalid UTF-8, JSONC, duplicate keys, invalid schema, malformed source shape,
  and capacity failures return typed terminal results and preserve the last
  accepted document. Do not reinterpret an invalid artifact as Settings.
- Generations and per-source revisions are strictly monotonic. Stale updates are
  rejected without modifying accepted state. A newer generation clears prior
  generation sources before accepting its first update.
- `Stop`/`Dispose` clears retained documents and listeners. Subsequent updates
  are terminal `Stopped` results and must not be delivered.

## Production filesystem composition

`CWorkspaceArtifactDocumentSourceController` is the only production adapter in
this subtree that may call `IFileService`. Construct it with the runtime-owned
file service and a long-lived `CWorkspaceArtifactDocumentService`; the adapter
borrows, but deliberately does not stop, that service. Runtime composition must
stop the adapter before disposing the service.

- Its request is a complete workspace topology: one nonzero generation, optional
  `.code-workspace` resource, and at most 64 folder URIs. Advance the generation
  whenever the workspace identity or folder topology changes. The adapter assigns
  strictly increasing revisions to actual read/remove operations.
- It reads the workspace file for `tasks`, `launch`, and `extensions` members and
  each folder's `.vscode/tasks.json`, `launch.json`, and `extensions.json`.
  `NotFound` is an explicit source removal, restoring workspace-file fallback;
  permission/read failures and corrupt bytes do not replace last-good content.
- Watch topology mirrors the stable configuration watcher: watch each folder for
  `.vscode` lifecycle and `.vscode` for the three named members. Events are
  deduplicated with a bounded debounce; overflow/rescan/disposal cancels, joins,
  rebuilds the full topology, then resnapshots.
- `Stop` cancels every watch and joins all worker/dispatcher threads. It rejects
  self-stop from its reload callback and guarantees no new callback begins after
  it returns. The callback is diagnostic/observation only: the service is updated
  before it receives the result.

## Verified Runtime Composition Checkpoint

`CWorkbenchRuntime` now owns the artifact service before its source controller,
starts artifact reads from the final reconciled semantic workspace topology,
and refreshes that topology after Settings-driven workspace reconciliation.
Artifact documents never enter effective Settings. A topology change advances
the source generation from the semantic workspace revision; unchanged topology
is deduplicated. Stop closes the listener gate, excludes concurrent Start,
joins the controller, and only then stops the borrowed document service.

For Task catalog reconciliation, `TasksForFolders` copies the service state and
all requested folder selections under one service mutex. It accepts at most 64
canonical folder URIs, preserves input order, and returns folder override or
workspace fallback without a separate snapshot/read race. The runtime uses that
single batch to create only explicit folder slots; a stopped or rejected batch
cannot publish a separately read empty topology.

The current runtime/artifact/catalog tests cover workspace-member fallback,
folder override, configuration separation, invalid-update last-good retention,
Empty/Folder/Workspace transitions, stable folder identity across reorder,
watch reload, atomic Task selection, and terminal Stop. Folder-scoped Task
catalog ownership is composed. Folder-scoped Launch catalog ownership and
production debugging remain the next artifact consumers; this checkpoint does
not imply them.
