# Workspace Artifact Document Guidance

## Ownership boundary

`CWorkspaceArtifactDocumentService` is the pure, process-local router for
`.vscode/tasks.json`, `.vscode/launch.json`, and their corresponding
`.code-workspace` members. It retains the whole accepted
JSONC source and root object so unrelated members are not discarded.

These documents are **not** `IConfigurationService` inputs. Only the existing
workspace `settings` path may enter configuration. This service exposes separate
Tasks and Launch snapshots, but it does not execute tasks or create a debug
adapter session.

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
this subtree that may call `IFileService`. Construct it with shared ownership
of the runtime file service and a long-lived `CWorkspaceArtifactDocumentService`;
the adapter deliberately does not stop either shared service. Runtime
composition must stop the adapter before stopping the document service.

- Its request is a complete workspace topology: one nonzero generation, optional
  `.code-workspace` resource, and at most 64 folder URIs. Advance the generation
  whenever the workspace identity or folder topology changes. The adapter assigns
  strictly increasing revisions to actual read/remove operations.
- It reads the workspace file for `tasks` and `launch` members and each folder's
  `.vscode/tasks.json` and `launch.json`.
  `NotFound` is an explicit source removal, restoring workspace-file fallback;
  permission/read failures and corrupt bytes do not replace last-good content.
- Watch topology mirrors the stable configuration watcher: watch each folder for
  `.vscode` lifecycle and `.vscode` for the two named members. Events are
  deduplicated with a bounded debounce; overflow/rescan/disposal replaces watch
  handles in place, preserving pre-admitted worker slots while it resnapshots.
- `Stop` cancels every watch and transfers dispatcher/worker ownership to the
  fixed-capacity `WorkerRetirementService`; it never waits for document
  operations or joins on the caller (especially the UI destruction path). It
  rejects self-stop from its reload callback and exposes pending/finalized
  retirement state. The callback is diagnostic/observation only: the service is
  updated before it receives the result.

## Verified Runtime Composition Checkpoint

`CWorkbenchRuntime` now owns the artifact service before its source controller,
starts artifact reads from the final reconciled semantic workspace topology,
and refreshes that topology after Settings-driven workspace reconciliation.
Artifact documents never enter effective Settings. A topology change advances
the source generation from the semantic workspace revision; unchanged topology
is deduplicated. Stop closes the listener gate, excludes concurrent Start,
transfers the controller's workers to bounded retirement, and only then stops
the shared document service.

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
