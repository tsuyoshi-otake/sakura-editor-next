# Explorer View Guidance

## Scope

`CExplorerTool` is the native Files Explorer view (Win32 TreeView, worker-thread
enumeration, overlay scrollbar). `ExplorerContextMenuModel.h` is its HWND-free
context-menu row model. Command identity, surface bindings, and the wire payload
live in [`../commands/CLAUDE.md`](../commands/CLAUDE.md); this file owns the
Explorer surface's own behavior and its recorded divergences from VS Code.

## Context Menu

- `BuildExplorerContextMenuRows` is the only authority for the menu's contents
  and order. It reproduces upstream `MenuId.ExplorerContext` sorting —
  `navigation` first, remaining groups lexicographic, one separator between
  adjacent non-empty groups — restricted to the eight commands
  `RegisterExplorerCommands` implements. The native projection renders rows and
  resolves titles/keybinding hints from the command registry; it must not
  reorder, insert, or retitle items.
- The model emits command IDs only. Titles are the registry's facts; a second
  title table here would drift.
- A workspace root gets no rename/deletion rows (upstream negates them via
  `ExplorerRootContext`), and the trash/permanent-deletion split follows
  upstream's two separate menu items on `ExplorerResourceMoveableToTrash` —
  never one item reading a flag.

## Recorded Divergences (omit, don't fake)

Upstream shows more in this menu than this product implements. Unimplemented
items are **omitted entirely**, never rendered disabled or approximated with
legacy behavior:

- `navigation`: Open to Side (`explorer.openToSide`), Open With...
  (`explorer.openWith`).
- `2_workspace`: Add Folder to Workspace / Remove Folder from Workspace on
  roots (multi-root workspace management is not implemented).
- `3_compare`: Compare with Selected / Select for Compare / Compare Selected.
- `5_cutcopypaste`: Cut / Copy / Paste.
- `5b_importexport`: Download / Upload (web/remote-only upstream).
- Multi-select: the native Explorer has no multi-select, so no menu item acts
  on a resource list; the command payload contract rejects upstream's optional
  second (multi-select) argument rather than acting on its first element.

Each of these must appear in its upstream group/order when implemented; adding
one means extending the model and its tests, not patching the projection.

- **Writable precondition is not yet modeled.** Upstream grays New File/New
  Folder/Rename/Paste through `ExplorerResourceWritableContext` (a non-writable
  filesystem provider). This provider serves the local Win32 filesystem only,
  which registers as writable, so the model has no `writable` input yet. Add
  the input when a read-only provider exists; do not pre-invent a context key
  that can never be false.

## File-Operation Executors (2026-08-13, #146)

The eight commands' native executors live in `CEditWnd` (`ExecuteExplorer*` /
`CommitExplorerRename` / `CommitExplorerCreate`), writing through a
window-owned lazy `platform::filesystem::IFileService`. The confirmation
wording model is `ExplorerDeleteConfirmation.h` and the relative-label/name
validity model is `ExplorerResourcePath.h`; both are pure and header-only so
their tests transcribe upstream wording/rules instead of a native dialog.

Recorded divergences of this flow (omit, don't fake):

- **Delete confirmation has no "Do not ask me again" checkbox.** Upstream's
  checkbox writes `explorer.confirmDelete`; until that setting is read and
  written here, every delete asks. A checkbox that remembered nothing would be
  the faked capability.
- **Permanent-delete file detail reads "This action is irreversible!"** instead
  of upstream's file-only "You can restore this file using the Undo command.",
  because this product has no file-operation Undo. The folder wording is
  upstream's own.
- **Operation failures surface as a modal error dialog.** Upstream reports
  them through the notification center; this product's notification surface
  cannot carry these yet, so the report is modal rather than silently logged.
- **An invalid entered name is no commit.** Upstream's inline rename box shows
  a live validation message under the input; the native TreeView label editor
  has no message surface, so a name failing `IsValidExplorerEntryName` simply
  does not commit and the edit ends with the entry unchanged.
