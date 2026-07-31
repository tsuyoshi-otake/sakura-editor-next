# Phase 2 Working-Copy Persistence Guidance

## Scope and Dependency Direction

This subtree owns presentation-neutral backup/session DTOs and codecs, the
working-copy lifecycle state machine, recovery adoption contracts, and narrow
legacy adapters. It depends on editor-core contracts and stable storage
interfaces; it must not depend on `CEditWnd`, HWNDs, menus, timers, dialogs,
command IDs, or direct control-process IPC.

`CEditDocWorkingCopyPersistenceAdapter` may translate one native `CEditDoc`
into a bounded snapshot/staged recovery request, but it is not the source of
editor identity, active selection, or durable policy. The composition root owns
the control-platform store and supplies it through `IWorkingCopyPersistenceStore`.

## Durability and Recovery Contracts

- Backups and sessions are independent records. Persist a dirty backup before
  its session manifest reference; when clearing a clean input, remove the
  session reference before deleting the generation. A failed step leaves the
  prior recoverable state intact.
- Each backup carries scope, schema, identity, content version, generation,
  encoding/EOL metadata, and checksum. Generation removal is conditional on the
  exact generation and cannot delete a newer snapshot.
- Capture text only on scheduled/forced flush. Save/close clean events contain
  identity and version only, so the hot success path remains O(1) with respect
  to document size.
- Restore prepares durable data, adopts it as an **inactive dirty** core input,
  then commits the staged native document. Any native rejection must compensate
  by rolling back the exact inactive core input; an inability to compensate is
  an explicit terminal failure, never a hidden partial restore.
- Every branch after successful `Prepare` that does not call `Commit` owns
  `AbortPrepared()` through a no-throw guard. Adoption rejection/exception,
  shutdown-after-prepare, validation failure, and allocation failure must discard
  the native stage, clear the in-flight owner, and allow a later retry.
- Clean completion reloads and revalidates session and backup scope, identity,
  session generation, backup generation, and content version against the opaque
  completion fence. It first unpublishes the session reference and then deletes
  only the exact backup generation. If deletion fails, the unreferenced backup is
  retained and the same token can safely retry; it may never delete a newer
  generation.
- Corrupt, unsupported, or scope-mismatched durable bytes are diagnosed and
  preserved. They are not replaced by an empty session or backup at startup.

## Lifecycle and Shutdown

The lifecycle is explicitly `Running`, `ShuttingDown`, or `Stopped`. Debounced
changes only schedule persistence; a forced shutdown flush owns final capture
while the native document remains valid. Composition stops the lifecycle before
destroying its adapter/core dependencies, and stops the editor control runtime
only after the final lifecycle/store work has reached a terminal outcome.

Restore is suppressed for an explicit document launch, multi-file launch, and
debug/grep modes. It runs only after the workbench group and registered input
handlers are ready. Recovered inputs remain inactive unless a later explicit
selection policy activates them. In the current singleton composition, that
later owner validates the persisted/effective input ID and activates it exactly;
this subtree never derives active selection from native state.
