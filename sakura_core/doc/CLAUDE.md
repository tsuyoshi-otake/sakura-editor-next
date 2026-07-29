# Document Model Guidance

## Ownership

`CEditDoc` is the aggregate root for one editor document.

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

## Verification

- Cover logical-line behavior independently from layout behavior when possible.
- For edits that affect wrapping, tabs, EOLs, encoding, or undo/redo, verify both the stored logical data and the rebuilt layout/visible result.
