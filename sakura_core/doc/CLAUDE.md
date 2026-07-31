# Document Model Guidance

## Ownership

`CEditDoc` is the aggregate root for one editor document.

During Issue #6 P1 migration, it is the aggregate root of the **legacy backing
document**, not proof that an Editor Input is open, visible, or active. The
workbench owns those states through adapters described in
[`../workbench/CLAUDE.md`](../workbench/CLAUDE.md).

- Logical text is owned by `logic/CDocLineMgr` and `CDocLine`.
- Visual wrapping and layout are owned by `layout/CLayoutMgr` and `CLayout`, above the logical model.
- File path, encoding metadata, and I/O belong to `CDocFile` and `CDocFileOperation`.
- Edit operations and undo/redo belong to `CDocEditor` and the operation-buffer classes.
- Per-document language/type state belongs to `CDocType`.

## Dependency and Update Rules

- Keep the dependency direction logical text → layout → view. Never make visual-wrap state the authoritative document content.
- Add document-wide state through `CEditDoc`; do not create a second owner or global mutable copy of document state.
- File open/save behavior belongs in `CDocFileOperation` or its collaborating load/save agents, not in view or window code.
- A logical-line mutation must account for layout invalidation/rebuild, selection/caret observers, and undo/redo ownership.
- Keep text encoding conversion at the file/charset boundary. Internal editing logic should operate on the established document representation.
- `CEditDoc::HandleCommand` is a document-facing command entry point; command routing policy and dispatcher lifecycle remain under `cmd/`.

## Load and Close Transactions

- A load commits only after the finalization barrier, not when file reading alone
  returns. Map `RESULT_COMPLETE` to `LOADED_OK`, `RESULT_LOSESOME` to
  `LOADED_LOSESOME`, and `RESULT_FAILURE` to `LOADED_FAILURE`; lossy load is a
  committed success and enters view mode, while a failed read resets partial
  logical lines and is never published as an empty successful document.
- `CDocSubject::NotifyFinalLoad` invokes every listener even when one returns
  failure or throws, contains exceptions, and aggregates one terminal status.
  `CDocFileOperation::DoLoadFlow` returns true only for OK/LOSESOME native I/O
  plus a successful all-listener barrier. Open macros and plugin events run only
  after that true terminal.
- `FileCloseOpen` resolves the picker target and completes `NotifyCheckLoad`
  before close prompting, close events, or native mutation. Once close is
  accepted it flushes the old working-copy generation while its line model still
  exists, and one load agent owns the native reset.
- The current replacement bridge cannot reconstruct the full old native object
  graph after I/O has begun. A failed replacement therefore fail-closes the Core,
  leaves a pathless native backing document, and preserves the durable recovery
  backup. Do not document this as in-place rollback.
- Ordinary Close remains prepare/commit: a cancelled or failed prepare does not
  clear native state, and legacy reset runs only after Core accepts the close.

## P1 Working Copy Boundary

- Introduce Working Copy, text-model resolver, encoding, save participant,
  backup, and editor-input contracts above the legacy aggregate. The same URI
  resolves to one shared model even when multiple editor inputs reference it.
- Dirty text and Untitled contents belong to versioned Working Copy Backup, not
  layout/session Memento or shared settings.
- Resolve, save, revert, close, and backup operations use operation IDs and one
  explicit terminal result. A stale async completion cannot replace a newer
  model or active editor.
- Bulk edits and undo/redo groups declare one owner for transaction completion;
  partial edits must roll back or report their exact committed result.
- Dispose the model only after its resolver references, working-copy ownership,
  and editor inputs have all released it.

## Verification

- Cover logical-line behavior independently from layout behavior when possible.
- For edits that affect wrapping, tabs, EOLs, encoding, or undo/redo, verify both the stored logical data and the rebuilt layout/visible result.
- Cover non-visible resolved documents, two inputs for one URI, save
  cancellation, stale resolve, backup generation races, and restore after
  forced termination.
